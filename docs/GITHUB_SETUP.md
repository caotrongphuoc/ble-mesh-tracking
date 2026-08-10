# GitHub repo setup checklist

One-time checklist for what to configure on GitHub after making the
repository public. None of it is done automatically by the code in
this repo — each item is a click in the GitHub UI or an API call.
Work through it top-to-bottom; every step is idempotent, so it is
safe to re-run.

## Repository → Settings → General

- **Description** (1 line, appears on the repo homepage):
  > Room-level indoor tracking on ESP32 / ESP32-S3 with BLE Mesh, ThingsBoard CE and MQTTS OTA.
- **Website**: leave empty or point to the ThingsBoard dashboard
  screenshot / demo URL once you have one.
- **Topics** (used by GitHub search — paste as space-separated tags):
  ```
  esp32 esp32-s3 nrf52840 ble-mesh esp-idf zephyr mcuboot thingsboard indoor-positioning rtls iot mqtts ota kalman-filter hmac secure-boot
  ```
- **Features**: keep **Issues** on. Enable **Discussions**
  (Community → Q&A / Show and tell / Ideas). Leave **Wiki** and
  **Projects** off — `docs/` covers the reference material.
- **Pull Requests**: allow *Squash merging* only; disable merge
  commits and rebase merging to keep `main` linear. Enable
  *Automatically delete head branches* after merge.

## Repository → Settings → Branches → Add rule for `main`

- Require a pull request before merging.
- Require status checks to pass — pick the CI jobs from
  `.github/workflows/build.yml` (`reference-tests`, every
  `esp-idf / <app> (<target>)` and `zephyr / <app> (<board>)`).
- Require branches to be up to date before merging.
- Require conversation resolution before merging.
- Restrict force pushes and deletions.
- Allow bypass for repo admins so you can hotfix directly when
  necessary (loosen only if you accept the risk).

## Repository → Settings → Security → Code security

- **Dependabot alerts** — enable.
- **Dependabot security updates** — enable.
- **Secret scanning** — enable. Also enable **Push protection** so
  secrets are refused at `git push` time instead of caught after the
  fact.
- **Code scanning** — enable *Default* CodeQL setup; C coverage is
  partial but catches obvious issues at zero maintenance cost.
- **Private vulnerability reporting** — enable so
  [`SECURITY.md`](../SECURITY.md)'s "Report a vulnerability" button
  actually works on the Security tab.

## Actions

- Confirm `build.yml` runs on the first push and every job goes
  green. Fix any environment-specific breakage before opening the
  repo for contributors.
- **Settings → Actions → General → Workflow permissions**: set to
  *Read repository contents and packages* by default (the current CI
  does not need write). Also uncheck *Allow GitHub Actions to
  create and approve pull requests*.

## About widget (front page, right sidebar → gear icon)

Mirror the description, website and topics from *Settings → General*
so the front page shows them alongside the README.

## Optional but recommended

- Add [`.github/FUNDING.yml`](https://docs.github.com/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/displaying-a-sponsor-button-in-your-repository)
  if you want a Sponsor button.
- Add [`CODEOWNERS`](https://docs.github.com/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/about-code-owners)
  so every PR auto-requests review from you.
- Pin the repo on your GitHub profile once it is public.

## Not needed here

- GitHub Pages — the `docs/` folder is meant to be read on GitHub
  directly, not published as a static site.
- Environments / deployments — no deploy target from this repo.
- Custom labels — GitHub's defaults plus the `bug` and `enhancement`
  labels the [issue templates](../.github/ISSUE_TEMPLATE/) already
  apply are enough for a small project.

## After the first PR / issue lands

- Answer within a few days — a public repo with stale unanswered
  issues signals dead project and drives contributors away.
- If interest picks up, revisit [`CONTRIBUTING.md`](../CONTRIBUTING.md)
  and this file to tighten the process (e.g. require signed commits,
  require two-reviewer sign-off).
