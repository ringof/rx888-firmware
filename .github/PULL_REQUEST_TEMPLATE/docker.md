<!--
Use this template for PRs that touch docker/ (Dockerfiles, helper
scripts, ka9q-radio patches, container READMEs).
-->

## Summary

<!-- 1-3 sentences: what changed and why. -->

## Author self-check

- [ ] CI is green.
- [ ] Image builds cleanly from scratch: `docker build --no-cache -t ka9q-radio docker/ka9q-radio/` (or the equivalent for the touched container).
- [ ] If a `patches/` file was added or removed, `docker/ka9q-radio/patches/README.md` is updated and `docker/ka9q-radio/README.md` "Known compatibility notes" is consistent.
- [ ] No firmware-side fix exists that would supersede a new container patch — if it does, fix the firmware instead.

## Reviewer hardware checks (RX888mk2 required)

- [ ] Image builds locally with `docker build --no-cache`.
- [ ] Helper script flow works end-to-end: `./ka9q.sh start` → `./ka9q.sh monitor` produces audible output on the configured RX888mk2.
- [ ] No regressions in tunes/sample rates that previously worked.

## Notes for reviewer

<!-- Container-specific quirks, expected logs, host kernel/USB
prerequisites, etc. -->
