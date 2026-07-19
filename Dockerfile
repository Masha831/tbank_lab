# Воспроизводимая среда сборки и запуска: Ubuntu 22.04, x64
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ make cmake python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

ENTRYPOINT ["/app/build/graphrank"]
