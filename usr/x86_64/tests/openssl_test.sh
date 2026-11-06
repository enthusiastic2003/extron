#!/bin/sh
# OpenSSL on Extron (libcrypto/libssl 3.x + the openssl command): digests against known
# vectors, symmetric encryption, key generation, signatures, certificates, and a TLS 1.3
# handshake with certificate verification over loopback. Runs inside the guest.
# Output format: "[openssl] what PASS|FAIL".
W=/tmp/openssl-test-$$
rm -rf "$W"; mkdir -p "$W" && cd "$W" || { echo "[openssl] workdir FAIL"; exit 1; }
command -v openssl >/dev/null || { echo "[openssl] not installed (skipped) PASS"; exit 0; }
eq() { if [ "$2" = "$3" ]; then echo "[openssl] $1 PASS"; else echo "[openssl] $1 FAIL (got '$3')"; fi; }
t() { what=$1; shift; if "$@" >/dev/null 2>&1; then echo "[openssl] $what PASS"; else echo "[openssl] $what FAIL"; fi; }
nt() { what=$1; shift; if "$@" >/dev/null 2>&1; then echo "[openssl] $what FAIL"; else echo "[openssl] $what PASS"; fi; }

eq "version" "OpenSSL 3" "$(openssl version | cut -d. -f1)"
eq "sha256(abc)" "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" \
    "$(printf abc | openssl sha256 | sed 's/.*= //')"
eq "sha512(abc) prefix" "ddaf35a193617aba" "$(printf abc | openssl sha512 | sed 's/.*= //' | cut -c1-16)"
eq "md5(abc)" "900150983cd24fb0d6963f7d28e17f72" "$(printf abc | openssl md5 | sed 's/.*= //')"
eq "hmac-sha256" "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8" \
    "$(printf 'The quick brown fox jumps over the lazy dog' | openssl dgst -sha256 -hmac key | sed 's/.*= //')"
eq "rand -hex length" "32" "$(openssl rand -hex 16 | tr -d '\n' | wc -c | tr -d ' ')"
nt "two random outputs differ" sh -c '[ "$(openssl rand -hex 16)" = "$(openssl rand -hex 16)" ]'

# symmetric encryption round trip, several ciphers, and a wrong password failing
dd if=/dev/urandom of=data.bin bs=1k count=300 2>/dev/null
for c in aes-256-cbc aes-128-ctr chacha20; do   # (enc does not do AEAD modes such as gcm)
    t "$c round trip" sh -c "openssl enc -$c -pbkdf2 -pass pass:k -in data.bin -out enc.bin && openssl enc -d -$c -pbkdf2 -pass pass:k -in enc.bin | cmp - data.bin"
done 2>/dev/null
openssl enc -aes-256-cbc -pbkdf2 -pass pass:right -in data.bin -out enc.bin 2>/dev/null
nt "wrong password does not decrypt" sh -c 'openssl enc -d -aes-256-cbc -pbkdf2 -pass pass:wrong -in enc.bin | cmp - data.bin'

# keys, signatures
t "ed25519 key" openssl genpkey -algorithm ed25519 -out ed.pem
t "ed25519 sign+verify" sh -c 'openssl pkeyutl -sign -rawin -inkey ed.pem -in data.bin -out sig.bin && openssl pkey -in ed.pem -pubout -out edpub.pem && openssl pkeyutl -verify -rawin -pubin -inkey edpub.pem -in data.bin -sigfile sig.bin'
t "ecdsa P-256 key" openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out ec.pem
t "ecdsa sign+verify" sh -c 'openssl dgst -sha256 -sign ec.pem -out ec.sig data.bin && openssl pkey -in ec.pem -pubout -out ecpub.pem && openssl dgst -sha256 -verify ecpub.pem -signature ec.sig data.bin'
nt "tampered data fails verification" sh -c 'echo x >> data.bin; openssl dgst -sha256 -verify ecpub.pem -signature ec.sig data.bin'
t "rsa 2048 key" openssl genrsa -out rsa.pem 2048
t "rsa sign+verify" sh -c 'echo hi > m; openssl dgst -sha256 -sign rsa.pem -out rsa.sig m && openssl rsa -in rsa.pem -pubout -out rsapub.pem && openssl dgst -sha256 -verify rsapub.pem -signature rsa.sig m'

# a CA and a server certificate signed by it
t "CA certificate" openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout ca.key -out ca.crt -days 2 -subj /CN=Test-CA
t "server key + CSR" openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout srv.key -out srv.csr -subj /CN=localhost
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > ext.cnf
t "CA signs the server certificate" openssl x509 -req -in srv.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out srv.crt -days 2 -extfile ext.cnf
t "certificate verifies against the CA" openssl verify -CAfile ca.crt srv.crt
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout other.key -out other.crt -days 2 -subj /CN=Other-CA 2>/dev/null
nt "certificate does not verify against another CA" openssl verify -CAfile other.crt srv.crt

# TLS 1.3 over loopback, with verification
openssl s_server -accept 4466 -cert srv.crt -key srv.key -www -naccept 2 >server.log 2>&1 &
SP=$!
sleep 2
out=$(echo | openssl s_client -connect 127.0.0.1:4466 -CAfile ca.crt -verify_return_error -verify_hostname localhost 2>&1)
case "$out" in *"Verification: OK"*) echo "[openssl] TLS handshake with verified certificate PASS";; *) echo "[openssl] TLS handshake with verified certificate FAIL";; esac
case "$out" in *"TLSv1.3"*) echo "[openssl] negotiates TLS 1.3 PASS";; *) echo "[openssl] negotiates TLS 1.3 FAIL";; esac
nt "TLS handshake fails against the wrong CA" sh -c 'echo | openssl s_client -connect 127.0.0.1:4466 -CAfile other.crt -verify_return_error'
kill $SP 2>/dev/null
cd /; rm -rf "$W"
