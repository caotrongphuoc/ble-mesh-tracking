# ThingsBoard everyday operation

> Quick-run guide. For the first-time setup from zero (create profiles,
> import rule chain, import dashboard, etc.), see `thingsboard/SETUP.md`.

---

## Option 1 — Open through the Docker Desktop app (easiest)

1. Open the **Docker Desktop** app (Start menu, type "Docker Desktop",
   or use the desktop icon).
2. Wait for the whale icon in the taskbar to stop spinning (~30-60 s).
3. **Nothing else to do** — the two containers `thingsboard` and
   `tb-postgres` auto-start via their restart policy.
4. Give ThingsBoard another 1-2 minutes to boot, then open
   **http://localhost:8080**.

In the Docker Desktop app, the **Containers** tab should show two
green rows:

- `thingsboard` — Running
- `tb-postgres` — Running

If a container did not auto-start (rare): press ▶ (Start) on its row
inside the app.

## Option 2 — Open from the terminal (PowerShell / CMD)

```powershell
# Start Docker Desktop if it is not already running:
Start-Process "$env:LOCALAPPDATA\Programs\DockerDesktop\Docker Desktop.exe"

# Wait for the daemon (this command should stop erroring):
docker info

# Start ThingsBoard (only if the containers did not auto-start):
cd <path>\thingsboard
docker compose up -d
```

---

## Access URLs

| What | Address |
|---|---|
| Web UI (dashboard) | http://localhost:8080 — from another LAN machine: `http://<host-ip>:8080` |
| MQTTS (Gateway ESP32 connects here) | `<host-ip>:8883` (TLS) |
| Tenant login | `tenant@thingsboard.org` |

Gateway ESP32 **auto-reconnects** once ThingsBoard is back up — no
board reset needed.

---

## Common commands (run inside your `thingsboard/` directory)

```powershell
docker compose ps           # status of the containers
docker compose logs -f tb   # live ThingsBoard log (Ctrl+C to exit)
docker compose stop         # stop (KEEPS the data)
docker compose start        # start again
docker compose restart      # restart (when TB is stuck)
```

> **NEVER run `docker compose down -v`** — the `-v` flag wipes the
> volume, which erases the database (devices, rule chain, dashboard,
> every telemetry row). `down` without `-v` is safe.

---

## Common problems

| Symptom | Cause | Fix |
|---|---|---|
| Gateway serial prints `esp-tls: select() timeout` + `MQTT disconnected` | ThingsBoard is down (Docker not running) | Open Docker Desktop, wait 2 minutes — the Gateway reconnects on its own |
| `docker: failed to connect to the docker API...` | Docker Desktop is not running | Launch Docker Desktop first, then run docker commands |
| Web UI on 8080 spins forever | TB is still booting (especially just after power-on) | Wait 1-2 minutes; watch progress with `docker compose logs -f tb` |
| Dashboard has no new data but the web UI works | Gateway lost WiFi / MQTT, or the mesh died | Check the Gateway serial: press `3` (MQTT / mesh stats), `1` (node table) |

---

## Tip: full auto-start

Docker Desktop -> **Settings -> General -> tick "Start Docker Desktop
when you sign in"**.

After that: power on the machine -> Docker starts -> ThingsBoard comes
up -> Gateway reconnects. Nothing to launch by hand.
