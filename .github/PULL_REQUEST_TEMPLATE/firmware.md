<!--
Use this template for PRs that touch SDDC_FX3/ (firmware sources,
driver, radio, GPIF, USB handlers, protocol.h, etc.).
-->

## Summary

<!-- 1-3 sentences: what changed and why. -->

## Recoverability check (Principle 2)

<!--
Per docs/vendor-protocol-plan.md, no firmware change may leave the
RX888 in a state requiring a power cycle to recover.  Address each:
-->

- [ ] All new code paths return to EP0 idle within a bounded time (no infinite loops, no busy-waits without timeout).
- [ ] Malformed payloads (wrong length, out-of-range values) are rejected at the EP0 boundary without corrupting internal state.
- [ ] `RESETFX3` and `STOPFX3` still work from any state this change can reach.
- [ ] Soak/fuzz coverage exists (existing scenario or new one in `tests/fx3_cmd.c`); call it out below.

## Author self-check

- [ ] CI is green (`SDDC_FX3 firmware (arm-none-eabi)` + `Host test tools`).
- [ ] Local build with `make -C SDDC_FX3 clean all` produces `SDDC_FX3.img` with no new warnings.
- [ ] If the wire protocol or vendor command set changed, `docs/architecture.md` and `docs/vendor-protocol-plan.md` are updated.
- [ ] If a host-side patch (e.g. `docker/ka9q-radio/patches/`) becomes redundant, it is removed in this PR (or noted as a follow-up commit).

## Reviewer hardware checks (RX888mk2 required)

<!-- These cannot run in CI.  Tick when you have run them locally. -->

- [ ] Targeted regression test passes: `./tests/fx3_cmd <scenario>` (specify which: ____).
- [ ] Soak run is clean: `./tests/soak_test.sh --firmware SDDC_FX3/SDDC_FX3.img --hours 1` — zero FAIL lines.
- [ ] Docker ka9q-radio container streams audio against the new firmware: `cd docker/ka9q-radio && ./ka9q.sh start && ./ka9q.sh monitor`.
- [ ] After all tests, device returns cleanly to either bootloader (`04b4:00f3`) or known-good app state (`04b4:00f1`) — no power cycle required.

## Notes for reviewer

<!-- Anything not obvious from the diff: which scenarios stress this
change, which existing scenarios it relies on, known limitations,
follow-up work. -->
