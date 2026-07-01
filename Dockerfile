# syntax=docker/dockerfile:1
#
# Multi-stage build for MatrixDB (BP-2: packaging artifact).
#   docker build -t matrixdb .
#   docker run -p 7070:7070 matrixdb
#   docker run -p 7070:7070 -e MATRIXDB_OPEN=/data/mydata.db -e MATRIXDB_TOKEN=s3cret \
#       -v ./mydata.db:/data/mydata.db matrixdb
#
# Build stage: has the C++ toolchain, compiles matrixdb + matrixdbd via build_all.sh
# (single source of truth for compiler flags — see build_all.sh at repo root).
FROM debian:12-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN ./build_all.sh

# Runtime stage: no compiler/toolchain — minimal attack surface. Only the two
# built binaries are copied in from the build stage above.
FROM debian:12-slim AS runtime

RUN useradd -m -u 10001 matrixdb \
    && mkdir -p /data && chown matrixdb:matrixdb /data

COPY --from=build /src/matrixdb /src/matrixdbd /usr/local/bin/

USER matrixdb
WORKDIR /data

EXPOSE 7070

# MATRIXDB_OPEN and MATRIXDB_TOKEN are optional; when unset/empty they contribute
# nothing to the command line (POSIX ${VAR:+...} expansion), matching matrixdbd's
# own optional --open/--token flags. PORT defaults to 7070 and can be overridden
# with `docker run -e PORT=...` without needing to rewrite the entrypoint.
ENV PORT=7070
ENTRYPOINT ["sh", "-c", "exec matrixdbd \"$PORT\" ${MATRIXDB_OPEN:+--open \"$MATRIXDB_OPEN\"} ${MATRIXDB_TOKEN:+--token \"$MATRIXDB_TOKEN\"}"]
