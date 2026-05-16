# PLAN — GitHub Pages site for rx888-firmware

> **Status: archived.**  This plan shipped in PR #121.  The site is
> live at https://ringof.github.io/rx888-firmware/ and the source
> lives under `docs/`.  This document is preserved for design rationale.

## Goal

Stand up a public project page at
`https://ringof.github.io/rx888-firmware/` that serves as the developer-
facing front door for the firmware, in the same vein as the project
pages used by `rx888-tools` and `ka9q-radio`.

The site must satisfy three audiences:

1. **End users / integrators** evaluating whether to use this firmware
   on an RX888mk2 — quick overview, compatibility, where to get a
   release.
2. **Host-application developers** writing software that drives the
   firmware — needs a complete, accurate USB vendor-command reference,
   GPIO map, argument list, and behavior notes.
3. **Firmware contributors** — needs links into the architecture and
   recovery docs already present in `docs/`.

## Non-goals

- No backend / no JS app. Static content only.
- No duplication of `README.md` content. The site links to or includes
  the canonical document; it does not maintain a second copy that can
  drift.
- No new vendor commands, no firmware behavior changes. This plan is
  documentation-and-site-config only.

## Hosting decision

**Use GitHub Pages with the GitHub Actions deployment source**
(`actions/deploy-pages`), built from the existing `docs/` directory
plus a small set of new top-level site files. Build with Jekyll using
the `just-the-docs` remote theme.

Why this combination:

- **GitHub Actions source (not "deploy from /docs branch")** — gives us
  a single deploy workflow we can extend later (e.g., to auto-generate
  the API reference page from `protocol.h`). Keeps `docs/` as a
  filesystem directory that contributors edit directly; CI handles
  publishing.
- **Jekyll** — natively supported by GitHub Pages, no Node toolchain
  to maintain, renders the existing `.md` files in `docs/` with no
  conversion.
- **just-the-docs theme** — purpose-built for technical/API
  documentation: persistent sidebar, search, anchor-linkable headings,
  and good rendering of reference tables (which we'll need heavily for
  the vendor-command table). Matches the visual idiom used by
  comparable SDR project pages.

Alternatives considered and rejected:

- **Render `README.md` only via the default Pages theme.** Insufficient
  — the README is already long and the API reference would dominate
  it. We need multiple pages with navigation.
- **mkdocs / Sphinx / Docusaurus.** Add a build toolchain (Python or
  Node) for marginal benefit over Jekyll. Not justified for a site of
  this size.
- **Hand-rolled static HTML.** Rejected on maintenance grounds.

## Site structure

All site sources live in `docs/` (the directory already exists and
already contains the canonical reference docs). New files marked
`(NEW)`; existing files marked `(EXISTING)`:

```
docs/
  _config.yml                 (NEW)  Jekyll site config, theme, nav order
  index.md                    (NEW)  Landing page — overview, badges, quick links
  api.md                      (NEW)  USB vendor-command reference (primary deliverable)
  building.md                 (NEW)  Pulled from README "Building" section, links back
  compatibility.md            (NEW)  Host-app compatibility matrix (ka9q-radio, rx888_tools, ExtIO)
  architecture.md             (EXISTING — front-matter added)
  gpif-and-recovery.md        (EXISTING — front-matter added)
  diagnostics_side_channel.md (EXISTING — front-matter added)
  wedge_detection.md          (EXISTING — front-matter added)
  vendor-protocol-plan.md     (EXISTING — front-matter added; marked "design notes")
  ka9q-compat-audit.md        (EXISTING — front-matter added; marked "design notes")
  LICENSE_ANALYSIS.md         (EXISTING — front-matter added; marked "reference")
  assets/
    favicon.png               (NEW, optional)
```

The eight existing `.md` files in `docs/` need a small Jekyll
front-matter block prepended (title, nav_order, parent group) so they
slot into the sidebar correctly. Their body content is not modified.

## Primary content: `docs/api.md`

This is the deliverable that distinguishes this site from a vanilla
README render. It must contain, verified against `SDDC_FX3/protocol.h`:

1. **USB device identity** — VID/PID for both bootloader and programmed
   states (`04b4:00f3` and `04b4:00f1`, verified from `USBDescriptor.c`).
2. **Vendor command table** — one row per `enum FX3Command` entry in
   `protocol.h`. Columns: name, bRequest, direction (IN/OUT), wValue
   semantics, wIndex semantics, data-stage description, error
   conditions, notes. Source of truth is `protocol.h` + the
   handler implementations in `USBHandler.c`.
3. **GPIO control bits** — the `OUTXIO*` / `enum GPIOPin` map from
   `protocol.h`, with the physical-function annotations (ATT_LE,
   SHDWN, DITH, etc.) already in the header.
4. **`SETARGFX3` argument list** — the `enum ArgumentList` from
   `protocol.h` (`DAT31_ATT`, `AD8370_VGA`, `WDG_MAX_RECOV`), with
   value ranges and defaults.
5. **`GETSTATS` byte layout** — the diagnostic-counter byte offsets.
   Source: the `GETSTATS` handler in `USBHandler.c` and the
   `boot_count` reference in the README. Must be verified by reading
   the handler, not transcribed from memory.
6. **Streaming pipeline summary** — endpoint number, transfer type,
   packet size, expected throughput. Source: `USBDescriptor.c`.
7. **Test-only commands** (`HANGFX3`, `HANGMAIN`) — explicitly marked
   test-only; documented because they are useful to host-side
   integrators writing recovery tests, but flagged as not for
   production use.
8. **Versioning** — `FIRMWARE_VER_MAJOR`/`MINOR` from `protocol.h`,
   plus how to read it back via `TESTFX3`.

**Verification discipline:** every row in `api.md` cites a source
file:line. No claim about command behavior goes into the page without
being checked against the C source. If a behavior is ambiguous in the
source, the page says so rather than guessing.

## CI workflow

Add `.github/workflows/pages.yml`:

- Triggers: `push` to default branch on paths `docs/**`, `README.md`,
  `.github/workflows/pages.yml`; plus `workflow_dispatch`.
- Single job using the standard `actions/configure-pages`,
  `actions/jekyll-build-pages`, `actions/upload-pages-artifact`,
  `actions/deploy-pages` sequence.
- Permissions block: `pages: write`, `id-token: write`, `contents: read`.
- Concurrency group `pages` so overlapping pushes don't race.

The workflow does **not** modify `build.yml` or `release.yml`.

## Repository settings change (manual, one-time)

After the workflow lands and the first run is green, the repo admin
must flip **Settings → Pages → Source** from "Deploy from a branch"
(default) to **"GitHub Actions"**. This step requires the repo admin
and cannot be automated from the PR; it will be called out in the PR
description.

## Optional follow-up (not in this PR)

- **Auto-generate `api.md` from `protocol.h`** via a small script run
  in the pages workflow. Worth doing once the hand-written page is
  stable and we know exactly what shape we want; premature now.
- **Link the site from `README.md`** at the top, once the site is
  actually published and the URL is confirmed live.
- **Custom domain.** Out of scope.

## Work breakdown

1. Add `docs/_config.yml` with `just-the-docs` remote theme + nav
   configuration.
2. Add `docs/index.md` landing page.
3. Add `docs/api.md` — verified against `protocol.h`, `USBHandler.c`,
   `USBDescriptor.c`.
4. Add `docs/building.md` and `docs/compatibility.md` (short pages
   linking to README sections and to host-app repos).
5. Prepend Jekyll front-matter to the 7 existing `docs/*.md` files
   (architecture, gpif-and-recovery, diagnostics_side_channel,
   wedge_detection, vendor-protocol-plan, ka9q-compat-audit,
   LICENSE_ANALYSIS). No body edits.
6. Add `.github/workflows/pages.yml`.
7. Local sanity check: render with `bundle exec jekyll serve` is not
   required (CI is authoritative), but `jekyll-build-pages` will fail
   loudly on bad front-matter; we'll rely on the first workflow run
   for validation.
8. Open PR as draft. Call out the manual Pages-source toggle in the PR
   description.

## Validation

After merge and the manual Pages-source toggle:

- `https://ringof.github.io/rx888-firmware/` loads and shows the
  landing page.
- Sidebar lists API, Building, Compatibility, Architecture, GPIF &
  Recovery, Diagnostics, Wedge Detection, License Analysis.
- `https://ringof.github.io/rx888-firmware/api/` renders the vendor-
  command table with anchor-linkable command names
  (e.g. `…/api/#startfx3`).
- Spot-check three command rows against `protocol.h` and
  `USBHandler.c`.

## Regression

- `make -C SDDC_FX3 clean all` still produces `SDDC_FX3.img` (this PR
  changes no firmware sources).
- `build.yml` and `release.yml` runs are unaffected (this PR adds a
  new workflow on disjoint paths/triggers; existing workflows trigger
  on `push`/`pull_request` and `push:tags` respectively).
- The 7 existing `docs/*.md` files render unchanged on GitHub's
  in-repo Markdown preview (Jekyll front-matter is fenced YAML at the
  top, which GitHub's renderer hides).
