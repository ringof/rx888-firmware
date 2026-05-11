# CLAUDE.md — Working Agreement for Claude Code

## Commit and Push Policy

- **Always ask before committing and pushing.** Never commit or push without explicit user approval.

## Git Branch Discipline

**Do not create new git branches unless I have explicitly told you to
create that specific branch.** Authorization to do work is NOT
authorization to make a branch.

- When making any change, use the currently checked-out branch. Do not
  `git checkout -b`, `git branch`, or otherwise create a new branch
  without my explicit instruction naming it.
- Conceptual "scope cleanliness" is not a reason for a new branch.
  Commits manage scope; branches do not. An 8-line fix unrelated to
  the current branch's purpose still goes on the current branch unless
  I say otherwise.
- If I have told you to keep a specific branch focused on one topic,
  that means "don't open a PR with mixed scope," NOT "spawn a new
  branch for anything else that comes up." Carry the unrelated work
  on the same branch as separate commits, or ASK.
- If you genuinely think a change should land somewhere other than
  the current branch, ASK. Do not preemptively create or switch
  branches and present the result as a fait accompli.
- This applies recursively: if you've already created an unauthorized
  branch, do not propose moving the work to *another* new branch.
  Move it to a branch that already exists, or ask.

## Planning Policy

- For tasks that generate multiple needs or planned changes, **write a plan first** and add it to a document (e.g., `PLAN.md` or a specifically named Markdown file) before beginning implementation.
- Get user approval on the plan before proceeding with changes.

## Change Documentation Requirements

Before any approved commit, provide the user — in the chat — with a **copy-pastable block** containing all of the following:

1. **Detailed description of the change** — what was changed and why.
2. **Build/update instructions** — how to build or update after applying the change. If the process is identical to the documented build instructions, state that explicitly. If it differs, provide the exact steps.
3. **Validation test** — a concrete procedure to demonstrate the change works as intended. If a runtime validation test is not feasible, provide a clear rationale explaining why code inspection is sufficient.
4. **Regression test** — a procedure or set of checks demonstrating that other functions of the code remain unaffected by the change.

## Issue Filing Policy

- **Always audit the codebase before filing issues.** Issue descriptions must be derived from actual findings, not speculation about what might be wrong.
- Never assume a problem exists — verify it with concrete evidence (grep, file reads, build output) before writing it up.

## GitHub Issues from Plans

- When a plan contains many changes/tasks, offer to generate a **run-once shell script** that uses the local `gh` CLI to populate each planned task as a GitHub issue in the repository.
- The script should be self-contained, idempotent where practical, and use `gh issue create` with appropriate titles, bodies, and labels derived from the plan document.
