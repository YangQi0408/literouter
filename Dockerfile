# syntax=docker/dockerfile:1
#
# literouter in a container.
#
# Two stages, because the build needs a toolchain this image must not ship: the
# builder installs mcpp and compiles the CLI, and the runtime stage carries one
# binary and nothing else.
#
# Why the runtime base is not `bookworm`: the toolchain links against its own
# glibc, and the resulting binary imports symbols up to GLIBC_2.38. Debian 12
# (bookworm) is glibc 2.36 and the binary will not start there — the failure is
# an "unsupported version" message from the loader rather than anything the
# application can report. Debian 13 (trixie) is glibc 2.41, so it runs. Check
# the requirement before changing this default:
#
#     objdump -T cli/target/*/*/bin/literouter | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1

ARG RUNTIME_BASE=debian:trixie-slim

# Which stage supplies the binary. `builder` compiles from source; `prebuilt`
# packages one the release workflow already built. See the note on that stage.
#
# BuildKit only builds the stages the target actually depends on, so picking
# `prebuilt` skips the compile stage entirely — which is the whole point: the
# toolchain fetch is what made a source build take twelve minutes and fail
# intermittently when a download crossed xlings' ten-minute deadline.
ARG BINARY_SOURCE=builder

# ── prebuilt ─────────────────────────────────────────────────────────────────
# The cheap path, used by the image job in release.yml.
#
# That job downloads the Linux binary it just published as a release asset — the
# same artifact users get from the Releases page — and drops it at
# `prebuilt/literouter` in the build context. Packaging a file that already
# exists beats recompiling it, and the file is already relocatable: release.yml
# runs patch_elf_interp.py and `patchelf --set-rpath '$ORIGIN'` over it before
# packaging, so its interpreter is the standard /lib64/ld-linux-x86-64.so.2 and
# its only shared dependencies are libm and libc.
#
# This stage is never reached by a plain `docker build .`, because the default
# BINARY_SOURCE is `builder`. The directory therefore does not have to exist in
# the repository — only the release workflow creates it.
FROM ${RUNTIME_BASE} AS prebuilt
COPY prebuilt/literouter /out/literouter
RUN chmod 0755 /out/literouter

# ── builder ──────────────────────────────────────────────────────────────────
FROM ${RUNTIME_BASE} AS builder

ENV DEBIAN_FRONTEND=noninteractive

# `python3` is for scripts/patch_elf_interp.py, which rewrites the interpreter
# path away from the build machine's toolchain directory. Node is NOT installed:
# web/dist is committed, so the console is embedded from the checked-in bundle.
# Rebuilding the front end is a separate step (`npm --prefix web run build`)
# whose output is committed, which is what keeps this image build offline-safe.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl git xz-utils build-essential python3 \
 && rm -rf /var/lib/apt/lists/*

# mcpp installs itself and then fetches the pinned toolchain (llvm@22.1.8) and
# the compat packages on first build. That download is the slow part of this
# stage; the registry is kept in one layer so a rebuild of the source does not
# repeat it.
RUN curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh \
      | MCPP_NO_PATH=1 bash
ENV PATH="/root/.mcpp/bin:${PATH}"

WORKDIR /src
COPY . .

# The cache is mounted on `registry/data` and NOT on `registry` as a whole.
# mcpp's global config uses the bundled xlings (`[xlings] binary = "bundled"`),
# which resolves to `$MCPP_HOME/registry/bin/xlings` — a real file installed by
# the mcpp installer, not something that gets downloaded during the build. A
# cache mount over `/root/.mcpp/registry` is empty on the first build and would
# shadow that binary, and the build dies with "xlings binary not found" before
# compiling anything. `registry/data` is the directory that actually grows (the
# pinned toolchain and every fetched package), so caching exactly that keeps the
# expensive part and leaves `bin/` visible.
RUN --mount=type=cache,target=/root/.mcpp/registry/data \
    --mount=type=cache,target=/root/.mcpp/build-cache \
    mcpp build -p core --release \
 && mcpp build -p cli --release

# The interpreter and rpath point into the builder's toolchain directory, which
# does not exist in the runtime stage. Rewritten in place, then staged where the
# runtime stage can find it without guessing a build hash.
#
# The binary lives under the *workspace* `target/`, not `<member>/target/`: mcpp
# builds a virtual workspace into one root-level tree keyed by
# `target/<triple>/<hash>/bin/<member>/<name>`, and `mcpp build -p cli` writes
# there too. CI and release.yml both search `target/` for exactly this reason.
#
# The binary is picked by modification time, newest first, rather than by
# `find | head -1`. `find` returns entries in directory order, so if more than
# one build tree is ever present it silently stages an arbitrary — and possibly
# stale — binary. `.dockerignore` keeps `target/` out of the context so there
# should be exactly one; newest-first keeps the stage honest if that ever stops
# being true.
RUN set -eu; \
    bin="$(find target -type f -name literouter -path '*/bin/*' \
            -printf '%T@ %p\n' \
            | sort -nr | head -1 | cut -d' ' -f2-)"; \
    test -n "$bin"; \
    python3 scripts/patch_elf_interp.py "$bin"; \
    mkdir -p /out; \
    cp "$bin" /out/literouter; \
    chmod 0755 /out/literouter

# ── binary ───────────────────────────────────────────────────────────────────
# Resolves to whichever stage BINARY_SOURCE named, so the runtime stage can copy
# from one fixed place. A direct `COPY --from=${BINARY_SOURCE}` would work, but
# going through a named stage keeps the error readable when the arg names a stage
# that does not exist.
FROM ${BINARY_SOURCE} AS binary

# ── runtime ──────────────────────────────────────────────────────────────────
FROM ${RUNTIME_BASE} AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# ca-certificates is not optional: every real relay is https, and without a
# trust store every request fails with "upstream certificate rejected".
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --system --gid 8787 literouter \
 && useradd --system --uid 8787 --gid 8787 --no-create-home \
      --home-dir /var/lib/literouter --shell /usr/sbin/nologin literouter \
 && mkdir -p /etc/literouter /var/lib/literouter \
 && chown -R literouter:literouter /etc/literouter /var/lib/literouter

COPY --from=binary /out/literouter /usr/local/bin/literouter
COPY LICENSE NOTICE THIRD_PARTY_NOTICES.md /usr/share/licenses/literouter/

# Config and state are separate mounts on purpose. The config holds keys and is
# meant to be edited and backed up; the state directory holds the telemetry file,
# which must survive a container replacement — telemetry on an anonymous volume
# is a history that resets when the container does.
ENV LITEROUTER_CONFIG=/etc/literouter/config.json
ENV LITEROUTER_STATE_DIR=/var/lib/literouter

# Loopback by default would make the published port useless, so the container
# listens on every interface and relies on the operator to publish it to the
# host's loopback (`-p 127.0.0.1:8787:8787`). `literouter serve` refuses nothing
# here, but validate() warns when a non-loopback host has no api_key, which is
# the check that matters.
VOLUME ["/etc/literouter", "/var/lib/literouter"]

USER literouter
EXPOSE 8787

# `--host` is pinned and `--port` deliberately is not. Pinning the host is what
# makes the published port useful at all, since the seed config listens on
# 127.0.0.1. The port is left to the config so that `server.port` means the same
# thing inside the container as outside it — a `--port` here would silently
# override the mounted file, and an operator editing `server.port` would have no
# way to see why nothing changed. Change the port in the config, then publish
# the matching port (`docker run -p 127.0.0.1:9000:9000`).
EXPOSE 8787

# `status` talks to the admin API over the same socket a client would, so this
# checks the whole path — listener, config, admin surface — rather than only that
# a process exists. It exits non-zero when nothing answers. It reads the port
# from the same config the server does, so it follows a changed `server.port`.
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD ["literouter", "status", "--json", "--quiet"]

# `exec` form so the binary is PID 1 and receives SIGTERM directly: the server
# installs no signal handler and relies on stop() being reached, so an
# intermediate shell that ignores the signal would leave the container hanging
# until the kill timeout.
ENTRYPOINT ["literouter"]
CMD ["serve", "--host", "0.0.0.0"]
