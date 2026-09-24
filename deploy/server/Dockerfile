FROM ubuntu:24.04 AS runtime
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates openssl \
    && rm -rf /var/lib/apt/lists/*
COPY common.sh /usr/local/lib/tinymesh-common.sh

FROM runtime AS signaling
COPY TinyMeshSignalingServer-Linux-x64.tar.gz /tmp/server.tar.gz
RUN mkdir -p /opt/tinymesh \
    && tar -xzf /tmp/server.tar.gz --strip-components=1 -C /opt/tinymesh \
    && rm /tmp/server.tar.gz \
    && test -x /opt/tinymesh/bin/TinyMeshSignalingServer
COPY signaling-entrypoint.sh /usr/local/bin/signaling-entrypoint
ENTRYPOINT ["bash", "/usr/local/bin/signaling-entrypoint"]

FROM runtime AS coturn
RUN apt-get update \
    && apt-get install -y --no-install-recommends coturn \
    && rm -rf /var/lib/apt/lists/*
COPY coturn-entrypoint.sh /usr/local/bin/coturn-entrypoint
ENTRYPOINT ["bash", "/usr/local/bin/coturn-entrypoint"]
