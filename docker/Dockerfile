FROM ubuntu:16.04
LABEL maintainer="neil@zzrcxb.me"
LABEL description="A unified environment for compiling & running gem5"

ARG DEBIAN_FRONTEND=noninteractive

RUN apt update && \
    apt install -y --no-install-recommends \
    build-essential \
    git \
    m4 \
    zlib1g \
    zlib1g-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libprotoc-dev \
    libgoogle-perftools-dev \
    python-dev \
    python-six \
    python3-six \
    python \
    python-setuptools \
    python-wheel \
    python-pip \
    libboost-all-dev \
    pkg-config \
    wget \
    curl \
    vim \
    gosu \
    sudo \
    gcc \
    gdb \
    clang \
    cmake

RUN python -m pip install --upgrade pip && \
    python -m pip install --upgrade wheel && \
    python -m pip install --upgrade scons

COPY entrypoint.sh /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD /bin/bash
