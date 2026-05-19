FROM docker.io/library/ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libwayland-dev wayland-protocols libxkbcommon-dev \
    libcairo2-dev libpango1.0-dev libgdk-pixbuf-2.0-dev librsvg2-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY CMakeLists.txt /build/mdview/
COPY src/ /build/mdview/src/
COPY protocol/ /build/mdview/protocol/
COPY thirdparty/ /build/mdview/thirdparty/
RUN cmake -S /build/mdview -B /build/mdview/build -DCMAKE_BUILD_TYPE=Release
RUN cmake --build /build/mdview/build --config Release -- -j"$(nproc)"
