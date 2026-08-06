# `thingsboard/`

Server-side files for BMT: ThingsBoard CE + PostgreSQL + nginx OTA
stack, plus the rule chains, dashboard, and dev TLS bundle that the
firmware expects.

## Contents

| Path | What it is |
|---|---|
| [`docker-compose.yml`](docker-compose.yml) | ThingsBoard CE 3.7, PostgreSQL, and the `bmt-ota-server` nginx container (HTTPS 8443) serving `firmware/`. |
| [`ota-nginx.conf`](ota-nginx.conf) | nginx config for the OTA fileserver. |
| [`dashboard/`](dashboard/) | `indoor_tracking.json` — dashboard export, ready to import. |
| [`rulechain/`](rulechain/) | `ble_tag_zone_detection.json` (zone algorithm, default chain for `ble_tag` profile) and `ble_mesh_node.json` (persists OTA attributes, default chain for `ble_mesh_node` profile). Plus metadata backups. |
| [`tls/`](tls/) | Dev CA + server cert (CN = SAN = `bmt-tb.local`; firmware verifies CN). `gen_certs.sh` regenerates them. |

## Getting started

- One-page end-to-end setup (host + firmware + ThingsBoard):
  [`../docs/00-quickstart.md`](../docs/00-quickstart.md).
- Detailed ThingsBoard configuration reference (device profiles, rule
  chain, dashboard aliases, `ZONE_MAP`, verification, common
  problems): [`../docs/04-thingsboard-setup.md`](../docs/04-thingsboard-setup.md).
- Daily operation (start / stop, docker commands, troubleshooting):
  [`Thingsboard.md`](Thingsboard.md).
- MQTT topic and payload reference:
  [`../docs/05-thingsboard-mqtt.md`](../docs/05-thingsboard-mqtt.md).
- HTTPS OTA server and TLS trust:
  [`../docs/06-http-tls.md`](../docs/06-http-tls.md).
