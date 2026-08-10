#!/usr/bin/env bash
# ============================================================================
# BMT MQTTS certificate generator
# ----------------------------------------------------------------------------
# MUST run once on a fresh clone before `docker compose up` — the repo
# does not ship any key material. Also run whenever certs expire or
# after changing HOSTNAME below.
#
# Produces (in this directory):
#   ca.pem / ca.key         - Certificate Authority (self-signed)
#   server.pem / server.key - Server cert for ThingsBoard, CN + SAN = HOSTNAME
#
# ca.pem     -> mounted into the nginx OTA container AND copied into
#               firmware EMBED_TXTFILES paths (script does that below)
# server.*   -> mounted into the ThingsBoard MQTTS container
#
# RUN:  bash gen_certs.sh
# Requires: openssl (Git Bash on Windows already has it).
# ============================================================================

set -e

# Disable MSYS path conversion on Git Bash Windows. Without this,
# /C=VN/... gets turned into C:/Program Files/Git/C=VN/...
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL="*"

# ---- CONFIG: change the hostname here if needed ----
HOSTNAME="bmt-tb.local"
DAYS=3650              # 10 years — no expiry surprises during a project
# ----------------------------------------------------

echo "=== BMT MQTTS cert generator ==="
echo "Hostname (CN + SAN): $HOSTNAME"
echo ""

# 1. CA private key
openssl genrsa -out ca.key 2048

# 2. CA self-signed cert
openssl req -new -x509 -days $DAYS -key ca.key -out ca.pem \
    -subj "//C=VN/O=BMT/CN=BMT-Root-CA"

echo "[1/4] CA created: ca.pem"

# 3. Server private key
openssl genrsa -out server.key 2048

# 4. Server CSR
openssl req -new -key server.key -out server.csr \
    -subj "//C=VN/O=BMT/CN=$HOSTNAME"

echo "[2/4] Server key + CSR created"

# 5. SAN extension file. Firmware verifies CN, but SAN is set as well
# so tools like openssl / curl are also happy.
cat > server_ext.cnf <<EOF
subjectAltName = DNS:$HOSTNAME
extendedKeyUsage = serverAuth
EOF

# 6. Sign the server cert with the CA
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key \
    -CAcreateserial -out server.pem -days $DAYS \
    -extfile server_ext.cnf

echo "[3/4] Server cert signed by CA (CN + SAN = $HOSTNAME)"

# 7. Verify the signing chain
openssl verify -CAfile ca.pem server.pem

echo "[4/4] Verification OK"

# 8. Copy the fresh CA cert into both firmware EMBED_TXTFILES paths so
#    subsequent firmware builds trust this server. Paths are relative to
#    this script.
FW_MQTT_CA="../../apps/gateway/components/bmt_mqtt/ca.pem"
FW_OTA_CA="../../components/bmt_ota/ota_ca.pem"
mkdir -p "$(dirname "$FW_MQTT_CA")" "$(dirname "$FW_OTA_CA")"
cp ca.pem "$FW_MQTT_CA"
cp ca.pem "$FW_OTA_CA"
echo "[+] Copied ca.pem -> $FW_MQTT_CA"
echo "[+] Copied ca.pem -> $FW_OTA_CA"

echo ""
echo "=== DONE ==="
echo "Files:"
echo "  ca.pem / ca.key         -> this directory (CA — keep ca.key private)"
echo "  server.pem / server.key -> mounted by docker-compose into ThingsBoard + nginx"
echo "  ca.pem                  -> also copied into both firmware EMBED_TXTFILES paths"
echo ""
echo "Next: cd .. && docker compose up -d, then rebuild any node whose OTA CA must trust this server."
echo ""
echo "Inspect SAN for a sanity check:"
openssl x509 -in server.pem -noout -text | grep -A1 "Subject Alternative Name"
