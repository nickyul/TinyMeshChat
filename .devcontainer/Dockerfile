FROM ubuntu:24.04

ARG QT_VERSION=6.9.3
ARG AQT_VERSION=3.1.19
ARG VCPKG_REF=2026.06.24

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        autoconf \
        autoconf-archive \
        automake \
        build-essential \
        bison \
        ca-certificates \
        cmake \
        coturn \
        curl \
        flex \
        gdb \
        gettext \
        gperf \
        git \
        libdbus-1-3 \
        libfontconfig1 \
        libfreetype6 \
        libgl1-mesa-dev \
        libtool \
        libxcb-cursor0 \
        libxcb-icccm4 \
        libxcb-keysyms1 \
        libxcb-shape0 \
        libxcb-xkb1 \
        libxkbcommon0 \
        libxkbcommon-x11-0 \
        nasm \
        ninja-build \
        openssl \
        pkg-config \
        python3 \
        python3-venv \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/aqt \
    && /opt/aqt/bin/pip install --no-cache-dir "aqtinstall==${AQT_VERSION}" \
    && /opt/aqt/bin/aqt install-qt linux desktop "${QT_VERSION}" linux_gcc_64 \
        --outputdir /opt/Qt \
        --modules qtwebsockets

RUN git clone --branch "${VCPKG_REF}" --depth 1 https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    QT_ROOT=/opt/Qt/${QT_VERSION}/gcc_64 \
    VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DEFAULT_TRIPLET=x64-linux \
    VCPKG_DISABLE_METRICS=1
ENV PATH=${QT_ROOT}/bin:${VCPKG_ROOT}:${PATH} \
    CMAKE_PREFIX_PATH=${QT_ROOT}

WORKDIR /workspaces/TinyMeshChat
