# Builder image for the Linux Hull host. Lets you produce a Linux binary from any
# OS that runs Docker (used by `npm run build:linux`). Installs the WebKitGTK / GTK4
# stack plus OpenSSL, libsecret (keychain) and CUPS (printing) dev packages.
#
# The host sources are mounted at build time (not copied), so the image is reused
# across source changes:
#   docker build -t hull-linux-builder -f linux.Dockerfile .
#   docker run --rm -v <host>:/work/host:ro -v <out>:/out hull-linux-builder
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git pkg-config ca-certificates \
      libssl-dev libsecret-1-dev \
      libcups2-dev libgnutls28-dev libavahi-client-dev \
      libgtk-4-dev libwebkitgtk-6.0-dev \
      libsqlcipher-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
# Configure out-of-source from the mounted, read-only sources, build, and stage the
# binary into the mounted /out directory.
CMD sh -c '\
  cmake -S /work/host -B /tmp/build -DCMAKE_BUILD_TYPE=Release && \
  cmake --build /tmp/build -j"$(nproc)" && \
  cp /tmp/build/bin/hull-host /out/hull-host && \
  echo "built: $(/out/hull-host --help >/dev/null 2>&1; file /out/hull-host 2>/dev/null || echo /out/hull-host)"'
