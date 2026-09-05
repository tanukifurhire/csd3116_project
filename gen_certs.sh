#!/usr/bin/env bash
# Generates a CA plus a certificate/key pair for the server and up to
# 5 clients, all signed by that CA. Run this once, then distribute:
#   - ca.pem                  -> copy to every machine (server + all clients)
#   - server.pem, server.key  -> server machine only
#   - clientN.pem, clientN.key -> that specific client machine only
#
# Usage: chmod +x gen_certs.sh && ./gen_certs.sh
set -e

mkdir -p certs && cd certs

# --- 1. Certificate Authority ---
# Used as both the Identity CA and the Permissions CA (fine for a class
# project; a production system would usually separate these).
openssl genrsa -out ca.key 2048
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
    -out ca.pem \
    -subj "/O=CSD3116Project/CN=DDS Identity CA"

# --- 2. Server certificate ---
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr \
    -subj "/O=CSD3116Project/CN=server"
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
    -out server.pem -days 730 -sha256

# --- 3. Client certificates ---
for NAME in client1 client2 client3 client4 client5; do
    openssl genrsa -out ${NAME}.key 2048
    openssl req -new -key ${NAME}.key -out ${NAME}.csr \
        -subj "/O=CSD3116Project/CN=${NAME}"
    openssl x509 -req -in ${NAME}.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
        -out ${NAME}.pem -days 730 -sha256
done

rm -f *.csr *.srl

echo ""
echo "Done. Files are in ./certs"
echo ""
echo "Subject names (needed for permissions.xml) - RFC2253 format:"
for NAME in server client1 client2 client3 client4 client5; do
    printf "  %-8s " "$NAME"
    openssl x509 -in ${NAME}.pem -noout -subject -nameopt RFC2253
done
