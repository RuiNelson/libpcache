# syntax=docker/dockerfile:1
FROM debian:testing

RUN apt-get update && apt-get install -y \
    cmake \
    gcc \
    g++ \
    git \
    pkg-config \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

RUN --mount=type=bind,source=./CMakeLists.txt,target=/build/CMakeLists.txt \
    --mount=type=bind,source=./include,target=/build/include \
    --mount=type=bind,source=./src,target=/build/src \
    --mount=type=bind,source=./tests,target=/build/tests \
    cmake -B build && cmake --build build && ctest --test-dir build --exclude-regex benchmark --output-on-failure
