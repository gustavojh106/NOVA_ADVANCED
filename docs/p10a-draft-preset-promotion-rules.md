# P10A Draft Preset Promotion Rules

Status: NOT_RUN

## Allowed States After Listening

- `LISTENING_CANDIDATE`
- `NEEDS_GAIN_STAGING_ADJUSTMENT`
- `NEEDS_MORE_LISTENING`
- `TECHNICAL_INVESTIGATION`
- `REJECTED`

## Prohibited States In P10A

- `FACTORY_APPROVED`
- `FACTORY_CANDIDATE`, unless a future explicit decision opens that phase.
- `RELEASE_READY`

## Rules

- A `PASS` with evidence can keep a preset at `LISTENING_CANDIDATE`.
- A light `WARN` can become `NEEDS_MORE_LISTENING` or `NEEDS_GAIN_STAGING_ADJUSTMENT`.
- A technical `FAIL` must become `TECHNICAL_INVESTIGATION`.
- A `P0` or `P1` issue blocks promotion.
- A `P2` issue can allow a workaround only with an issue and explicit follow-up.
- A `P3` issue can remain a taste or preset note.
- No preset can become factory-approved in P10A.
- No manual listening status can become complete without evidence.

## Decision Examples

Use `NEEDS_GAIN_STAGING_ADJUSTMENT` for level mismatch, excessive output, too-low output, or bypass jump when the tone is otherwise valid.

Use `TECHNICAL_INVESTIGATION` for repeatable clipping, clicks, runaway tails, unsafe output, broken loading, denormal-like behavior, or clear technical fault.

Use `NEEDS_MORE_LISTENING` for setup uncertainty, inconsistent perception across monitors, or taste concerns that need another listener.

Use `REJECTED` only when the draft is not worth revising as a factory preset candidate.

Manual listening QA remains pending or NOT_RUN until the session is executed. Distortion manual listening QA remains pending. P7F/Reaper remains pending.
