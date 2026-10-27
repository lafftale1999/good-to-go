#!/bin/sh
#
# Generates a local CA and a server certificate for goodtogo.local signed by it.
#
# Outputs (in the directory of this script):
#   ca.key, ca.pem      - The CA. Import ca.pem into your browser/OS as a trusted
#                         root. Keep ca.key private; it's reused on later runs so
#                         the CA only has to be trusted once.
#   prvtkey.pem         - Server private key, embedded into the firmware.
#   servercert.pem      - Server certificate, embedded into the firmware.
#
# Usage: ./gen_cert.sh [--force-ca]
#   --force-ca  Regenerate the CA even if one already exists.

set -eu

HOSTNAME="goodtogo.local"
CA_DAYS=3650
SERVER_DAYS=825 # Max validity accepted by Apple platforms

OUT_DIR="$(cd "$(dirname "$0")" && pwd)"
CA_KEY="$OUT_DIR/ca.key"
CA_CERT="$OUT_DIR/ca.pem"
SERVER_KEY="$OUT_DIR/prvtkey.pem"
SERVER_CERT="$OUT_DIR/servercert.pem"

gen_ca() {
    echo "Generating CA..."
    openssl ecparam -name prime256v1 -genkey -noout -out "$CA_KEY"
    chmod 600 "$CA_KEY"

    openssl req -x509 -new -sha256 \
        -key "$CA_KEY" \
        -days "$CA_DAYS" \
        -subj "/CN=Good To Go Local CA" \
        -addext "basicConstraints = critical, CA:TRUE, pathlen:0" \
        -addext "keyUsage= critical, keyCertSign, cRLSign" \
        -addext "subjectKeyIdentifier = hash" \
        -out "$CA_CERT"
}

gen_server_cert() {
    echo "Generating server certificate for $HOSTNAME..."
    tmp_dir="$(mktemp -d)"
    trap 'rm -rf "$tmp_dir"' EXIT

    openssl ecparam -name prime256v1 -genkey -noout -out "$SERVER_KEY"
    chmod 600 "$SERVER_KEY"

    openssl req -new -sha256 \
        -key "$SERVER_KEY" \
        -subj "/CN=$HOSTNAME" \
        -out "$tmp_dir/server.csr"

    cat > "$tmp_dir/server.ext" <<EOF
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:$HOSTNAME
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
EOF

    openssl x509 -req -sha256 \
        -in "$tmp_dir/server.csr" \
        -CA "$CA_CERT" -CAkey "$CA_KEY" -CAcreateserial \
        -days "$SERVER_DAYS" \
        -extfile "$tmp_dir/server.ext" \
        -out "$SERVER_CERT"

    rm -f "$OUT_DIR/ca.srl"
}

main() {
    if [ "${1:-}" = "--force-ca" ] || [ ! -f "$CA_KEY" ] || [ ! -f "$CA_CERT" ]; then
        gen_ca
    else
        echo "Reusing existing CA at $CA_CERT"
    fi

    gen_server_cert

    openssl verify -CAfile "$CA_CERT" "$SERVER_CERT"
    echo "Done. Trust $CA_CERT on your client devices to access https://$HOSTNAME"
}

main "$@"
