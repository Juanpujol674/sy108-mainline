FROM debian:13
LABEL org.opencontainers.image.title="SY108 OpenWrt source builder"
ENV DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8 PYTHONDONTWRITEBYTECODE=1
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential binutils make ninja-build cmake git rsync bc bison flex \
    gawk gettext file time wget curl ca-certificates patch unzip xz-utils zstd \
    libssl-dev libelf-dev libncurses-dev zlib1g-dev libjson-c-dev libusb-1.0-0-dev \
    dwarves device-tree-compiler kmod cpio pkgconf musl-tools strace \
    python3 python3-setuptools swig perl quilt ccache \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
CMD ["/bin/bash"]
