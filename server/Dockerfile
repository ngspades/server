# syntax=docker/dockerfile:1
# The dedicated server (no client), both profiles: protocol 1.0 by default,
# the classic 0.75 game with SP_PROFILE=075. Build from the repository root,
# with the submodules checked out (git submodule update --init --recursive):
#   docker build -f server/Dockerfile -t ngspades-server .
# or use server/docker-compose.yml.
#
# The image is the release's static Linux binary (tools/build_server_static.sh)
# on scratch: no system, no shell, ~11 MB. /data is the server's root: an
# empty folder is enough, the server writes its profile's default config
# there and downloads the maps its rotation names (ngspades/maps).

# ---------- builder ----------
FROM alpine:3.22 AS builder

WORKDIR /src
COPY . /src
# a checkout without its submodules fails here, with the files it misses
RUN test -f common/enet/include/enet6/enet.h && test -f common/vxl/libvxl.h
RUN sh tools/build_server_static.sh /build \
    && cd server \
    && /build/ngspades-server --check-config \
    && /build/ngspades-server --profile 075 --check-config

# the runtime's folders, owned by the user the server runs as (scratch has
# no mkdir): /data (its root, a volume) and /tmp
RUN mkdir -p /rootfs/data/Resources/plugins /rootfs/tmp \
    && cp server/Resources/plugins/README.md server/Resources/plugins/server_tools.lua /rootfs/data/Resources/plugins/ \
    && chmod 1777 /rootfs/tmp

# ---------- runtime ----------
FROM scratch

COPY --from=builder /build/ngspades-server /app/ngspades-server
COPY --from=builder --chown=1000:1000 /rootfs/ /

# no /etc/passwd on scratch: a numeric user, never root
USER 1000:1000
WORKDIR /data
# 1.0: the game (UDP); SP_PROFILE=075: 32897. (WebRTC, the browser
# transport, stays off on a dedicated server unless its config enables it,
# so no port for browsers is published here.) The remote console's port (32890)
# only listens on the container's own 127.0.0.1 unless SP_RCON_BIND says
# otherwise.
EXPOSE 32887/udp 32897/udp
VOLUME ["/data"]

ENTRYPOINT ["/app/ngspades-server", "--root", "/data"]
CMD []
