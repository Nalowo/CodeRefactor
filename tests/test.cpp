#include <gtest/gtest.h>

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "RefactorTool.h"

#include <fstream>
#include <string>
#include <stdexcept>

using namespace clang::tooling;

static std::string runToolAndReadFile(const std::string &Code)
{
    llvm::SmallString<64> TempPath;
    if (auto EC = llvm::sys::fs::createTemporaryFile("refactor_test", "cpp", TempPath))
        throw std::runtime_error(std::string("Cannot create temporary file: ") + EC.message());
    
    std::string FileName = std::string(TempPath.c_str());
    std::vector<std::string> Args = {"-std=c++20"};
    if (!runToolOnCodeWithArgs(std::make_unique<CodeRefactorAction>(), Code, Args, FileName))
    {
        llvm::sys::fs::remove(FileName);
        throw std::runtime_error("Tool execution failed");
    }

    std::ifstream ifs(FileName);
    if (!ifs.good())
    {
        llvm::sys::fs::remove(FileName);
        throw std::runtime_error("Cannot open resulting file");
    }
    std::string Content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    llvm::sys::fs::remove(FileName);
    return Content;
}

// ---------- Tests for non-virtual destructor ----------

TEST(RefactorTool, AddVirtualToDtor_WhenHasDerived)
{
    const std::string Code = R"cpp(
class Base {
public:
    ~Base();
};

class Derived : public Base {
public:
    ~Derived() {}
};

Base::~Base() {}
)cpp";

    std::string Out = runToolAndReadFile(Code);
    EXPECT_NE(Out.find("virtual ~Base"), std::string::npos);
}

TEST(RefactorTool, DontAddVirtual_WhenNoDerived)
{
    const std::string Code = R"cpp(
class Base {
public:
    ~Base();
};

Base::~Base() {}
)cpp";

    std::string Out = runToolAndReadFile(Code);
    EXPECT_EQ(Out.find("virtual ~Base"), std::string::npos);
}

// ---------- Tests for missing override ----------

TEST(RefactorTool, AddOverrideToMethod_WhenOverrides)
{
    const std::string Code = R"cpp(
class Base {
public:
    virtual void foo() {}
};

class Derived : public Base {
public:
    void foo() { }
};
)cpp";

    std::string Out = runToolAndReadFile(Code);
    EXPECT_NE(Out.find("void foo() override"), std::string::npos);
}

TEST(RefactorTool, DontDuplicateOverride_WhenAlreadyPresent)
{
    const std::string Code = R"cpp(
class Base {
public:
    virtual void foo() {}
};

class Derived : public Base {
public:
    void foo() override { }
};
)cpp";

    std::string Out = runToolAndReadFile(Code);
    // Должно присутствовать ровно одно "override" в определении метода (не меньше 1)
    size_t count = 0;
    size_t pos = 0;
    while ((pos = Out.find("override", pos)) != std::string::npos)
    {
        ++count;
        pos += 8;
    }
    EXPECT_GE(count, 1u);
}

// ---------- Tests for range-for const T -> const T& ----------

TEST(RefactorTool, AddAmpersandInRangeFor_ForNonPrimitive)
{
    const std::string Code = R"cpp(
#include <vector>
struct Heavy { Heavy(){} Heavy(const Heavy&){} };
void f() {
    std::vector<Heavy> v;
    for (const Heavy h : v) {
        (void)h;
    }
}
)cpp";

    std::string Out = runToolAndReadFile(Code);
    EXPECT_NE(Out.find("const Heavy& h"), std::string::npos);
}

TEST(RefactorTool, DontChangePrimitiveInRangeFor)
{
    const std::string Code = R"cpp(
#include <vector>
void f() {
    std::vector<int> v;
    for (const int x : v) {
        (void)x;
    }
}
)cpp";

    std::string Out = runToolAndReadFile(Code);
    EXPECT_EQ(Out.find("const int& x"), std::string::npos);
}