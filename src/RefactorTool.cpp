#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Lex/Lexer.h"

#include <unordered_set>
#include <string>

#include "RefactorTool.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

namespace details
{
    std::optional<SourceLocation> GetOverrideInsertLoc(llvm::StringRef iStr, SourceRange SR)
    {
        std::optional<SourceLocation> res;
        auto pos = iStr.find(')');
        if (pos == StringRef::npos)
            return res;

        std::optional<size_t> preCommentPos;
        size_t offset = 0;
        auto controlOffset = [&offset](size_t newOffset, bool comment = false)
        {
            if (newOffset != StringRef::npos)
                offset += newOffset;
            return newOffset;
        };

        for (auto tmpStr = iStr.substr(pos + 1); !tmpStr.empty();)
        {
            tmpStr = tmpStr.substr(controlOffset(tmpStr.find_first_not_of(" \t\r\n")));
            if (tmpStr.empty() || tmpStr.front() == '{' || tmpStr.front() == '=' || tmpStr.front() == ';')
                break;

            if (tmpStr.front() == '/')
            {
                auto endP = tmpStr.find("*/");
                if (endP != StringRef::npos)
                    endP += 2;

                preCommentPos = offset;
                tmpStr = tmpStr.substr(controlOffset(endP, true));
            }
            else if (std::isalnum(tmpStr.front()))
            {
                preCommentPos = std::nullopt;
                auto it = std::find_if_not(tmpStr.begin(), tmpStr.end(), [](auto c)
                                           { return std::isalpha(c); });
                auto word = tmpStr.substr(0, it - tmpStr.begin());
                std::optional<size_t> bracketOffset;
                if ((word == "noexcept" || word == "throw") && (tmpStr[word.size()] == '('))
                    if (auto pp = tmpStr.find(')', word.size()); pp != StringRef::npos)
                        bracketOffset = pp + 1;
                tmpStr = tmpStr.substr(controlOffset(bracketOffset.value_or(word.size())));
            }
            else if (tmpStr.front() == '&')
            {
                preCommentPos = std::nullopt;
                size_t p = 1;
                if (tmpStr.size() > 1 && tmpStr[1] == '&')
                    p += 1;
                tmpStr = tmpStr.substr(controlOffset(p));
            }
        }

        auto begin = SR.getBegin();
        res = begin.getLocWithOffset(static_cast<int>(pos + preCommentPos.value_or(offset)));

        return res;
    }
} // end namespace details

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

// Метод run вызывается для каждого совпадения с матчем.
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult &Result)
{
    DiagnosticsEngine &Diag = Result.Context->getDiagnostics();
    SourceManager &SM = *Result.SourceManager;

    // Невиртуальные деструкторы
    if (const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("nonVirtualDtor"))
        handle_nv_dtor(Dtor, Diag, SM);

    // Методы без override
    if (const auto *Method = Result.Nodes.getNodeAs<CXXMethodDecl>("missingOverride"))
        if (Method->size_overridden_methods() > 0 && !Method->hasAttr<OverrideAttr>())
            handle_miss_override(Method, Diag, SM);

    // range-for без & (const T -> const T&)
    if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("loopVar"))
        handle_crange_for(LoopVar, Diag, SM);
}

// Обработка невиртуального деструктора: добавляем 'virtual ' перед '~' если есть наследники.
void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor,
                                     DiagnosticsEngine &Diag,
                                     SourceManager &SM)
{
    if (!Dtor)
        return;

    auto loc = Dtor->getLocation();
    if (loc.isInvalid() || !SM.isInMainFile(loc) || SM.isInSystemHeader(loc))
        return;

    if (Dtor->isVirtual())
        return;

    const auto *Parent = Dtor->getParent();
    if (!Parent)
        return;
    if (!Parent->hasDefinition())
        return;

    // Найдём, есть ли производные классы в TU
    auto &Ctx = Parent->getASTContext();
    bool hasDerived = false;
    for (auto *D : Ctx.getTranslationUnitDecl()->decls())
    {
        if (const auto *RD = dyn_cast<CXXRecordDecl>(D))
        {
            if (!RD->hasDefinition())
                continue;
            for (const auto &Base : RD->bases())
            {
                if (const auto *T = Base.getType().getTypePtrOrNull())
                {
                    if (const auto *RT = T->getAs<RecordType>())
                    {
                        if (const auto *BaseDecl = dyn_cast<CXXRecordDecl>(RT->getDecl()))
                        {
                            if (BaseDecl->getCanonicalDecl() == Parent->getCanonicalDecl())
                            {
                                hasDerived = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (hasDerived)
                break;
        }
    }
    if (!hasDerived)
        return;

    unsigned raw = loc.getRawEncoding();
    if (virtualDtorLocations.count(raw))
        return; // уже обработано

    Rewrite.InsertTextBefore(loc, "virtual ");
    virtualDtorLocations.insert(raw);

    unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Добавлен 'virtual' к деструктору");
    Diag.Report(loc, DiagID);
}

// Обработка методов без override: вставляем ' override' после закрывающей ')'
void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method,
                                           DiagnosticsEngine &Diag,
                                           SourceManager &SM)
{
    if (!Method)
        return;

    auto loc = Method->getLocation();
    if (loc.isInvalid() || !SM.isInMainFile(loc) || SM.isInSystemHeader(loc))
        return;

    // Метод должен переопределять базовый
    if (Method->size_overridden_methods() == 0 || Method->hasAttr<OverrideAttr>() || Method->hasAttr<FinalAttr>())
        return;

    auto SR = Method->getSourceRange();
    auto CSR = CharSourceRange::getTokenRange(SR);
    auto &Ctx = Method->getASTContext();
    const auto &LangOpts = Ctx.getLangOpts();

    auto insertLoc = details::GetOverrideInsertLoc(Lexer::getSourceText(CSR, SM, LangOpts), SR);
    if (!insertLoc || insertLoc->isInvalid() || !SM.isInMainFile(*insertLoc))
        return;

    auto raw = insertLoc->getRawEncoding();
    if (virtualDtorLocations.count(raw))
        return; // уже изменяли тут

    Rewrite.InsertTextBefore(*insertLoc, " override");
    virtualDtorLocations.insert(raw);

    auto DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Добавлен 'override' к методу");
    Diag.Report(*insertLoc, DiagID);
}

// Обработка range-for: добавляем '&' после типа, если это const T (не ссылка и не фундаментальный)
void RefactorHandler::handle_crange_for(const VarDecl *LoopVar,
                                        DiagnosticsEngine &Diag,
                                        SourceManager &SM)
{
    if (!LoopVar)
        return;

    auto loc = LoopVar->getLocation();
    if (loc.isInvalid() || !SM.isInMainFile(loc) || SM.isInSystemHeader(loc))
        return;

    auto &Ctx = LoopVar->getASTContext();
    const auto &LangOpts = Ctx.getLangOpts();

    auto QT = LoopVar->getType();
    if (QT.isNull())
        return;
    if (QT->isReferenceType())
        return; // уже ссылка
    if (QT->isPointerType())
        return; // не трогаем указатели
    if (QT->isFundamentalType())
        return; // не трогаем примитивы

    if (!LoopVar->getTypeSourceInfo())
        return;
    auto TL = LoopVar->getTypeSourceInfo()->getTypeLoc();
    auto endLoc = TL.getEndLoc();
    if (endLoc.isInvalid())
        return;

    auto insertLoc = Lexer::getLocForEndOfToken(endLoc, 0, SM, LangOpts);
    if (insertLoc.isInvalid() || !SM.isInMainFile(insertLoc))
        return;

    auto raw = insertLoc.getRawEncoding();
    if (virtualDtorLocations.count(raw))
        return;

    Rewrite.InsertTextBefore(insertLoc, "&");
    virtualDtorLocations.insert(raw);

    auto DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Добавлен '&' в range-for переменной");
    Diag.Report(insertLoc, DiagID);
}

auto NvDtorMatcher()
{
    return cxxDestructorDecl(unless(isVirtual()), unless(isImplicit())).bind("nonVirtualDtor");
}

auto NoOverrideMatcher()
{
    return cxxMethodDecl(
               isOverride(),
               unless(cxxDestructorDecl()) // исключаем деструкторы
               )
        .bind("missingOverride");
}

auto NoRefConstVarInRangeLoopMatcher()
{
    return cxxForRangeStmt(
        hasLoopVariable(
            varDecl(
                hasType(qualType(
                    isConstQualified(),
                    unless(referenceType()))))
                .bind("loopVar")));
}

ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite)
{
    Finder.addMatcher(NvDtorMatcher(), &Handler);
    Finder.addMatcher(NoOverrideMatcher(), &Handler);
    Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

void ComplexConsumer::HandleTranslationUnit(ASTContext &Context)
{
    Finder.matchAST(Context);
}

std::unique_ptr<ASTConsumer> CodeRefactorAction::CreateASTConsumer(CompilerInstance &CI,
                                                                   StringRef file)
{
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<ComplexConsumer>(RewriterForCodeRefactor);
}

bool CodeRefactorAction::BeginSourceFileAction(CompilerInstance &CI)
{
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return true;
}

void CodeRefactorAction::EndSourceFileAction()
{
    if (RewriterForCodeRefactor.overwriteChangedFiles())
        llvm::errs() << "Error applying changes to files.\n";
}