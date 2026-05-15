FROM docker.io/library/ubuntu:24.04 AS builder
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
FROM docker.io/library/ubuntu:24.04 AS artifacts
RUN apt-get update && apt-get install -y --no-install-recommends \
    libwayland-client0 libwayland-cursor0 libxkbcommon0 \
    libcairo2 libpangocairo-1.0-0 libgdk-pixbuf-2.0-0 librsvg2-2 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/mdview/build/mdview /opt/mdview/mdview
