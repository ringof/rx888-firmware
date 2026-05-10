<!--
Use this template for PRs that touch .github/ (workflows, PR
templates, issue templates, repo automation).
-->

## Summary

<!-- 1-3 sentences: what was added, removed, or changed. -->

## Author self-check

- [ ] YAML lint passes: `python3 -c "import yaml; yaml.safe_load(open('<workflow>'))"`.
- [ ] If a new workflow was added, it has run on this PR and the result is visible in the "Checks" tab.
- [ ] If an existing workflow was modified, its run on this PR is green (and matches the intended new behaviour, not just "still passes").
- [ ] No secrets committed (`mcp__github__run_secret_scanning` clean if anything sensitive is referenced).

## Reviewer checks

- [ ] The workflow on this PR ran and passed (look at "Checks" tab — required).
- [ ] If a job was renamed or restructured, branch protection rules (if any) still match the new job names.
- [ ] If a template was added or modified, render it on a test PR via `?template=<name>.md` to confirm.

## Notes for reviewer

<!-- Anything that wouldn't be obvious from the workflow diff:
matrix coverage trade-offs, planned future jobs, deferred concerns. -->
