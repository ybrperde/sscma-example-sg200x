FROM --platform=linux/amd64 ubuntu:22.04

ARG HOST_TOOLS_REF=master

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --no-install-recommends -y ca-certificates cmake git make ninja-build \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "${HOST_TOOLS_REF}" \
        https://github.com/sophgo/host-tools.git /opt/host-tools \
    && test -x /opt/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc \
    && /opt/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc --version

COPY .sdk /opt/sdk

ENV PATH=/opt/host-tools/gcc/riscv64-linux-musl-x86_64/bin:${PATH}
ENV SG200X_SDK_PATH=/opt/sdk/sg2002_recamera_emmc
ENV CC=/opt/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
ENV CXX=/opt/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-g++

COPY docker/build-solution.sh /usr/local/bin/build-solution
RUN chmod +x /usr/local/bin/build-solution

WORKDIR /workspace
ENTRYPOINT ["/usr/local/bin/build-solution"]