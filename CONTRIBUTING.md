# Contributing

Thanks for your interest in improving BLE Mesh Tracking. This document is short — the [docs/](docs/) folder has the details.

## Before you start

- Read [docs/00-quickstart.md](docs/00-quickstart.md) to get the system running once. Every contribution assumes you can build and flash all four apps.
- Skim [docs/01-architecture.md](docs/01-architecture.md) so you know where the change you want to make belongs (Tag / Scanner / Relay / Gateway / server-side).

## Development setup

- **ESP-IDF v6.0.1** for `apps/{gateway,scanner,relay,tag}` (ESP32 / ESP32-S3).
- **Zephyr** (recent) for `apps/Beacon_{ProMicro,Xiao}Nrf52840` (nRF52840 beacon boards).
- **Docker + Docker Compose** for the ThingsBoard + nginx OTA stack (`thingsboard/docker-compose.yml`).
- **Python 3** for the scripts in `tools/`.

Build one app: `cd apps/<app> && idf.py build`. Build all four: `tools/build-all.sh`.

## Code style

Root `.clang-format` (Allman, tabs, `ColumnLimit: 0`) applies to every C source in the repo, including the Zephyr Beacon apps.

Before committing:

```
tools/format.sh                       # reformat everything
clang-format --dry-run --Werror <file># check a single file
```

Comments:

- Explain **why**, not **what**. Well-named identifiers already tell the reader what the code does.
- Do not add historical tags in code (`[FIX-*]`, `[ADD]`, `[REVERTED]`, PR/issue numbers). That context belongs in the commit message and `git log`.
- One short line is usually enough. Multi-paragraph block comments are a sign the code needs to be split.

## Commit message convention

`[TAG] one-line subject in imperative mood`

Common tags: `[DOCS]`, `[FIX]`, `[REFACTOR]`, `[CLEANUP]`, `[FORMAT]`, `[CONFIG]`, `[SECURITY]`, `[CI]`.

Keep the subject under ~72 chars. If more explanation is needed, add a blank line and a body.

Split unrelated changes into separate commits — each commit should stand on its own.

## Testing before opening a PR

Run the checks listed in [docs/07-operation.md#checklists](docs/07-operation.md#checklists). At minimum:

- All four apps build cleanly.
- If your change touches a runtime path, run the relevant scenario in [docs/09-testing.md](docs/09-testing.md).
- If your change touches OTA, run the fault-injection scenarios in [docs/10-testing-ota.md](docs/10-testing-ota.md).

## Pull request flow

1. Fork the repo and create a branch named after the change: `feat/kalman-tuning`, `fix/watchdog-timeout`, etc.
2. Make focused commits following the conventions above.
3. Rebase onto the latest `main` before opening the PR.
4. In the PR description, explain what changed and why, and which tests you ran. Link to the relevant doc section if the change affects behavior documented there.

## Repo layout, at a glance

```
apps/                       # firmware projects
  {gateway,scanner,relay,tag}/     ESP-IDF (ESP32 / ESP32-S3)
  Beacon_{ProMicro,Xiao}Nrf52840/  Zephyr (nRF52840)
components/bmt_ota/         # shared OTA component (scanner + relay)
thingsboard/                # Docker stack, rule chain, dashboard, TLS
docs/                       # numbered guides 00..13
firmware/                   # OTA .bin drop location (gitignored)
tools/                      # format.sh, build-all.sh
```

## Notes on shared code

Some code is duplicated across apps by design (`bmt_types.h`, `bmt_auth.*`, `bmt_uart.*`). The versions look similar but diverge in role-specific ways — do not extract them into a shared component without careful auditing, or you will break behavior in a hard-to-diagnose way. The OTA path is the exception (extracted into `components/bmt_ota/`).

## License

By contributing, you agree that your contributions will be licensed under the [Apache License 2.0](LICENSE), the same license that covers the rest of the project.
