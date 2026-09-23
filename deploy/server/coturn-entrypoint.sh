#!/usr/bin/env bash
source /usr/local/lib/tinymesh-common.sh

check_turn_endpoints
check_ipv4 "$TURN_LISTEN_IP"
check_port "$TURN_MIN_PORT"
check_port "$TURN_MAX_PORT"
((TURN_MIN_PORT <= TURN_MAX_PORT)) || fail 'TURN_MIN_PORT exceeds TURN_MAX_PORT'
check_secret
mkdir -p /run/coturn
config=/run/coturn/turnserver.conf
cat > "$config" <<CONFIG
listening-ip=$TURN_LISTEN_IP
relay-ip=$TURN_LISTEN_IP
listening-port=$TURN_PORT
tls-listening-port=$TURN_TLS_PORT
min-port=$TURN_MIN_PORT
max-port=$TURN_MAX_PORT
realm=$PUBLIC_IP
server-name=$PUBLIC_IP
fingerprint
use-auth-secret
static-auth-secret=$(cat /var/lib/turn-auth/shared-secret)
cert=/certs/fullchain.pem
pkey=/certs/privkey.pem
no-cli
no-multicast-peers
# RFC 6062 relay sockets are disabled; client-to-TURN TCP/TLS stays enabled.
no-tcp-relay
no-dtls
no-tlsv1
no-tlsv1_1
denied-peer-ip=0.0.0.0-0.255.255.255
denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
denied-peer-ip=169.254.0.0-169.254.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.168.0.0-192.168.255.255
log-file=stdout
simple-log
pidfile=/run/coturn/turnserver.pid
userdb=/run/coturn/turndb
CONFIG
if [[ "$PUBLIC_IP" != "$TURN_LISTEN_IP" ]]; then
    printf 'external-ip=%s/%s\n' "$PUBLIC_IP" "$TURN_LISTEN_IP" >> "$config"
fi
exec turnserver -c "$config"
