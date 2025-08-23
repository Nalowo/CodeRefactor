FROM ubuntu@sha256:79efa276fdefa2ee3911db29b0608f8c0561c347ec3f4d4139980d43b168d991

# Update the package database and install required tools
RUN apt clean
RUN apt update
RUN apt install -y python3 python3-pip pipx git less vim sudo cmake make g++-15 libjemalloc-dev gdb linux-tools-common
RUN apt install -y libllvm-20-ocaml-dev libllvm20 llvm-20 llvm-20-dev llvm-20-doc llvm-20-examples llvm-20-runtime
RUN apt install -y clang-20 clang-tools-20 clang-20-doc libclang-common-20-dev libclang-20-dev libclang1-20 clang-format-20 python3-clang-20 clangd-20 clang-tidy-20

# Update  gcc/g++ version to the latest ones
RUN ln -fs /usr/bin/g++-15 /usr/bin/g++
RUN ln -fs /usr/bin/gcc-15 /usr/bin/gcc

# Create a non-root user 'dev'
RUN useradd -ms /bin/bash dev \
    && echo "dev ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers.d/dev \
    && chmod 0440 /etc/sudoers.d/dev

# Switch to the 'dev' user
USER dev

RUN git config --global core.editor code

# Set the default command
CMD ["/bin/bash"]