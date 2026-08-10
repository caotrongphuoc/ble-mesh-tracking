## Summary

<!-- What changed and why. One paragraph. Link to the issue if there is one. -->

## Type

- [ ] Bug fix
- [ ] New feature
- [ ] Refactor / cleanup
- [ ] Docs
- [ ] Build / CI
- [ ] Other:

## Scope

- [ ] Tag firmware
- [ ] Scanner firmware
- [ ] Relay firmware
- [ ] Gateway firmware
- [ ] Shared `components/bmt_ota`
- [ ] ThingsBoard rule chain / dashboard
- [ ] Docs

## Test plan

<!-- What you actually ran to verify this works. Reference the scenario from docs/06-testing.md when it applies. -->

- [ ] All four apps build (`tools/build-all.sh` or CI).
- [ ] Flashed and verified on hardware (list which boards).
- [ ] Relevant test from `docs/06-testing.md` passes.

## Checklist

- [ ] `tools/format.sh` clean (no diff).
- [ ] Commit messages follow `[TAG] subject` convention (see [CONTRIBUTING.md](../CONTRIBUTING.md)).
- [ ] Docs updated if behavior changed.
- [ ] No secrets, WiFi credentials, or personal LAN IPs committed.
- [ ] By opening this PR I agree the contribution is under Apache-2.0.
