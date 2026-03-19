# NOVA Session Rules

## Start Of Session

1. Read `docs/active-work.md`.
2. Run `scripts/context-bootstrap.ps1` if the task is larger than a quick answer.
3. Check for uncommitted changes before editing.

## Preferred Task Intake

The fastest requests include:

- the exact goal
- the subsystem involved
- what must not break
- what counts as done

If available, ask the user to keep `docs/active-work.md` current.

## Editing Rules

- Keep changes isolated when the worktree is already dirty.
- Add new files under `docs/`, `scripts/`, or the relevant source folder rather than scattering context notes randomly.
- If source-file membership changes, remember the `NOVA.jucer` update.

## Validation Rules

- Compile at minimum for code changes when practical.
- For state/preset work, sanity-check serialization paths.
- For GUI drag/drop work, check chain ordering and zone assignment behavior.
- For DSP work, assume bypass, smoothing, and latency can regress even if compile passes.
