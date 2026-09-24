#!/usr/bin/env bash
set -euo pipefail
umask 077

fail() { echo "$*" >&2; exit 2; }

check_ipv4() {
    local value="$1" octet
    local -a octets
    [[ "$value" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || fail "Invalid IPv4: $value"
    IFS=. read -r -a octets <<< "$value"
    for octet in "${octets[@]}"; do
        ((10#$octet <= 255)) || fail "Invalid IPv4: $value"
        [[ "$octet" == 0 || "$octet" != 0* ]] || fail "IPv4 must not contain leading zeroes"
    done
}

check_port() {
    [[ "$1" =~ ^[1-9][0-9]{0,4}$ ]] || fail "Invalid port: $1"
    (( $1 >= 1024 && $1 <= 65535 )) || fail "Port must be in 1024..65535: $1"
}

check_turn_endpoints() {
    check_ipv4 "${PUBLIC_IP:?PUBLIC_IP is required}"
    check_port "$TURN_PORT"
    check_port "$TURN_TLS_PORT"
    [[ "$TURN_PORT" != "$TURN_TLS_PORT" ]] || fail 'TURN and TLS ports must differ'
    [[ -r /certs/fullchain.pem && -r /certs/privkey.pem ]] || fail 'Cannot read /certs/fullchain.pem or /certs/privkey.pem'
}

check_secret() {
    local secret
    secret=$(cat /var/lib/turn-auth/shared-secret)
    [[ "$secret" =~ ^[a-fA-F0-9]{64}$ ]] || fail 'Invalid stored TURN secret; expected 64 hex characters'
}
