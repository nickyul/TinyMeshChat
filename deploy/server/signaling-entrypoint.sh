#!/usr/bin/env bash
source /usr/local/lib/tinymesh-common.sh

server=/opt/tinymesh/bin/TinyMeshSignalingServer
authority=/var/lib/tinymesh/authority.key

# Administrative commands use the existing authority without opening listeners.
if (($#)); then
    [[ -s "$authority" ]] || fail 'Start the signaling service once to initialize its authority'
    exec "$server" --authority-key "$authority" "$@"
fi

check_turn_endpoints
[[ "$TURN_TTL" =~ ^[1-9][0-9]{2,5}$ ]] || fail 'TURN_TTL must be in 600..604800'
((TURN_TTL >= 600 && TURN_TTL <= 604800)) || fail 'TURN_TTL must be in 600..604800'
mkdir -p /var/lib/tinymesh /var/lib/turn-auth
if [[ ! -e "$authority" ]]; then
    "$server" --init-authority "$authority"
fi

# Publish a complete secret atomically; never rotate it during an image update.
if [[ ! -e /var/lib/turn-auth/shared-secret ]]; then
    secret_tmp=$(mktemp /var/lib/turn-auth/.secret.XXXXXX)
    trap 'rm -f "$secret_tmp"' EXIT
    openssl rand -hex 32 > "$secret_tmp"
    ln "$secret_tmp" /var/lib/turn-auth/shared-secret || [[ -e /var/lib/turn-auth/shared-secret ]]
    rm -f "$secret_tmp"
    trap - EXIT
fi
check_secret

exec "$server" \
    --authority-key "$authority" \
    --listen-address 0.0.0.0 --port 8443 \
    --tls-cert /certs/fullchain.pem --tls-key /certs/privkey.pem \
    --turn-secret-file /var/lib/turn-auth/shared-secret \
    --turn-ttl "$TURN_TTL" \
    --turn-url "turn:$PUBLIC_IP:$TURN_PORT?transport=udp" \
    --turn-url "turn:$PUBLIC_IP:$TURN_PORT?transport=tcp" \
    --turn-url "turns:$PUBLIC_IP:$TURN_TLS_PORT?transport=tcp"
