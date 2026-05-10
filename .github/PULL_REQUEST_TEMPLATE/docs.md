<!--
Use this template for PRs that ONLY touch documentation: docs/,
README.md, top-level *.md, in-source comments.  If the same PR also
changes code or build, use one of the other templates instead.
-->

## Summary

<!-- 1-3 sentences: what was clarified, added, or removed. -->

## Author self-check

- [ ] Per CLAUDE.md "Issue Filing Policy" / general doc-change discipline: every code or behaviour claim is grounded in actual source — no speculation.
- [ ] Cross-references (`docs/architecture.md`, `docs/vendor-protocol-plan.md`, audit docs, etc.) still resolve.
- [ ] Markdown renders correctly (lists, code fences, tables) — preview locally or via the GitHub Files Changed tab.
- [ ] Deprecated commands or removed features are clearly flagged where readers would otherwise stumble on them.

## Reviewer checks

- [ ] Random-spot grep / `Read` of one or two cited file:line references confirms they say what the PR claims.
- [ ] No new orphaned or contradictory cross-references introduced.
