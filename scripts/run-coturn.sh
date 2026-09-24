#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: bash scripts/run-coturn.sh --host HOST [options]
  --host HOST          Public DNS name/IP advertised to clients (required)
  --listen-ip IP       Local interface address (default: 0.0.0.0)
  --external-ip IP     Public IP when coturn is behind NAT; PUBLIC/PRIVATE is also accepted
  --cert PATH          PEM certificate chain for TURN/TLS (requires --key)
  --key PATH           Unencrypted PEM private key
  --state-dir PATH     Generated config and shared secret (default: build/turn)

Starts coturn in the foreground: UDP/TCP 3478, TLS/TCP 5349 if a certificate is supplied.
Relay UDP ports: 49152..65535. These ports must be reachable, including through Docker/NAT.
Without a certificate only UDP/TCP is enabled. A TLS client must trust the certificate.
The generated secret file is also passed to signaling via --turn-secret-file.
USAGE
}

turn_host=''
turn_listen_ip='0.0.0.0'
turn_external_ip=''
turn_cert=''
turn_key=''
turn_state_dir='build/turn'
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --host|--listen-ip|--external-ip|--cert|--key|--state-dir)
            if (($# < 2)); then echo "Missing value for $1" >&2; exit 2; fi
            case "$1" in
                --host) turn_host=$2 ;;
                --listen-ip) turn_listen_ip=$2 ;;
                --external-ip) turn_external_ip=$2 ;;
                --cert) turn_cert=$2 ;;
                --key) turn_key=$2 ;;
                --state-dir) turn_state_dir=$2 ;;
            esac
            shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
if [[ -z "$turn_host" || "$turn_host" == *[!a-zA-Z0-9.:-]* ]]; then
    echo "--host must be a DNS name or IP address" >&2; exit 2
fi
if [[ -n "$turn_cert" && -z "$turn_key" || -z "$turn_cert" && -n "$turn_key" ]]; then
    echo "--cert and --key must be provided together" >&2; exit 2
fi
for value in "$turn_listen_ip" "$turn_external_ip" "$turn_cert" "$turn_key"; do
    if [[ "$value" == *$'\n'* || "$value" == *$'\r'* ]]; then
        echo "Newlines are not allowed in configuration values" >&2; exit 2
    fi
done
command -v turnserver >/dev/null
command -v openssl >/dev/null
if [[ -n "$turn_cert" ]]; then
    turn_cert=$(realpath "$turn_cert")
    turn_key=$(realpath "$turn_key")
    [[ -r "$turn_cert" && -r "$turn_key" ]] || { echo "Cannot read TLS files" >&2; exit 2; }
fi
umask 077
mkdir -p "$turn_state_dir"
turn_state_dir=$(cd "$turn_state_dir" && pwd)
turn_secret_file="$turn_state_dir/shared-secret"
if [[ ! -e "$turn_secret_file" ]]; then
    openssl rand -hex 32 > "$turn_secret_file"
fi
turn_secret=$(cat "$turn_secret_file")
if [[ ! "$turn_secret" =~ ^[a-fA-F0-9]{64}$ ]]; then
    echo "Expected a 64-character hex shared secret in $turn_secret_file" >&2; exit 2
fi
turn_config="$turn_state_dir/turnserver.conf"
cat > "$turn_config" <<CONFIG
listening-ip=$turn_listen_ip
listening-port=3478
min-port=49152
max-port=65535
realm=$turn_host
server-name=$turn_host
fingerprint
use-auth-secret
static-auth-secret=$turn_secret
no-cli
no-multicast-peers
# Disable RFC 6062 allocations, not client-to-TURN TCP/TLS connections.
no-tcp-relay
no-dtls
log-file=stdout
simple-log
pidfile=$turn_state_dir/turnserver.pid
CONFIG
unset turn_secret
if [[ -n "$turn_external_ip" ]]; then
    printf 'external-ip=%s\n' "$turn_external_ip" >> "$turn_config"
fi
if [[ -n "$turn_cert" ]]; then
    printf 'tls-listening-port=5349\ncert=%s\npkey=%s\n' "$turn_cert" "$turn_key" >> "$turn_config"
else
    printf 'no-tls\n' >> "$turn_config"
fi
turn_uri_host="$turn_host"
if [[ "$turn_uri_host" == *:* ]]; then turn_uri_host="[$turn_uri_host]"; fi
printf 'Signaling options:\n  --turn-secret-file %q\n' "$turn_secret_file"
printf '  --turn-url %q\n' "turn:$turn_uri_host:3478?transport=udp" "turn:$turn_uri_host:3478?transport=tcp"
if [[ -n "$turn_cert" ]]; then
    printf '  --turn-url %q\n' "turns:$turn_uri_host:5349?transport=tcp"
fi
exec turnserver -c "$turn_config"
