---
title: License analysis
nav_order: 11
permalink: /LICENSE_ANALYSIS/
description: License analysis of the RX888mk2 FX3 firmware - MIT application code, proprietary Cypress FX3 SDK / ThreadX RTOS, why the GPL R82xx driver was removed.
---

# License Analysis

## 1. Current License Landscape

The SDDC_FX3 firmware binary is composed of code from three license regimes:

| Component | License | Copyright | Files |
|-----------|---------|-----------|-------|
| SDDC_FX3 application code | MIT | Oscar Steila 2017-2020, David Goncalves 2024-2026 | `*.c`, `*.h` in `SDDC_FX3/` (excluding SDK files) |
| Cypress FX3 SDK | Proprietary (Cypress SLA) | Cypress Semiconductor 2010-2018 | `SDK/fw_lib/`, `SDK/fw_build/`, `cyfxtx.c`, `cyfx_gcc_startup.S`, `makefile` |
| ThreadX RTOS | Proprietary (Express Logic), sublicensed via Cypress SLA | Express Logic Inc. 1996-2007 | Embedded in SDK `.a` libraries; headers in `SDK/fw_lib/1_3_4/inc/tx_*.h` |

The GPL-licensed R82xx tuner driver was removed in commit 600574c to
eliminate a copyleft conflict with the proprietary Cypress SDK (see
§2 below for background).

## 2. The GPL v2 Problem

### What GPL v2 requires

GPL v2 is a strong copyleft license. When GPL code is statically linked into a
binary, the GPL requires the **entire combined work** to be distributed under
GPL-compatible terms (Section 2b).

### Why this is a conflict

The Cypress SDK license is GPL-incompatible:

- **Distribution restricted to object code only** (Cypress SLA §1.3) — GPL
  requires source availability
- **Must incorporate a Cypress IC** (Cypress SLA §1.1) — GPL prohibits
  additional restrictions on recipients
- **Non-transferable license** (Cypress SLA §1.1) — GPL requires sublicensing
  to all downstream recipients
- **Confidentiality obligations** (Cypress SLA §1.4, §5) — GPL requires public
  source availability

The ThreadX RTOS (linked via Cypress `.a` libraries) adds a second proprietary
layer in the same binary.

### Practical reality

Strictly interpreted, no single license can cleanly cover the combined binary.
This same tension exists in virtually all FX3-based open-source SDR projects.
Common defenses include the "system library exception" (GPL v2 §3 exempts
"major components of the operating system on which the executable runs"), but
this is debatable for embedded firmware.

### Cypress SDK redistribution

The Cypress SLA §1.4 and §5 require keeping SDK source confidential. However,
Cypress/Infineon distributes the SDK publicly via their website and numerous
reference projects include it in public repositories. The confidentiality clause
is arguably moot for materials Cypress themselves have made public.

## 3. GPL Removal (Completed)

The GPL-licensed R82xx tuner driver (`tuner_r82xx.c`, `tuner_r82xx.h`) was
removed from the firmware. The driver was statically linked into the same
binary as the proprietary Cypress SDK, creating an irreconcilable copyleft
conflict (see §2 above).

### What was removed

| USB Command | Code | Purpose |
|-------------|------|---------|
| `TUNERINIT` | 0xB4 | Initialize R828D tuner |
| `TUNERTUNE` | 0xB5 | Set RF frequency |
| `TUNERSTDBY`| 0xB8 | Tuner standby |
| `SETARGFX3` wIndex 1-4 | 0xB6 | R82xx gain/sideband/harmonic |

### What was preserved

- **Hardware detection** — R828D I2C probe at `0x74` still identifies the
  RX888mk2 (`RunApplication.c` uses a local `R828D_I2C_ADDR` define)
- **HF direct-sampling path** — ADC clock, GPIO control, DAT-31 attenuator,
  AD8370 VGA are all unaffected
- **I2C passthrough** — `I2CWFX3`/`I2CRFX3` commands remain, allowing a
  host-side driver to control the R828D tuner over I2C if desired
- **Backward compatibility** — old clients sending removed tuner commands
  receive a USB STALL via the existing default handler

## 4. Current License Summary

| Component | License | Distribution Rights |
|-----------|---------|-------------------|
| Application code | MIT | Freely distributable in any form |
| Cypress FX3 SDK | Cypress SLA | Object code distributable royalty-free with Cypress IC products (§1.3) |
| ThreadX RTOS | Sublicensed via Cypress SLA §6.2 | Covered under SDK distribution rights |

The final `.img` binary can be distributed under MIT for the application code,
with the SDK components covered by the Cypress SLA's distribution grant. No
copyleft obligations remain.
