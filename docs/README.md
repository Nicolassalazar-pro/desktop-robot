# Documentation index

Start here.

## Read before changing anything

| Doc | Why |
|---|---|
| [`PROJECT-HISTORY.md`](PROJECT-HISTORY.md) | 14 months of decisions, **43 already-rejected approaches**, measured constants, open bugs, working agreements. Section 3 and Appendix B are the ones that save time. |
| [`../HARDWARE.md`](../HARDWARE.md) | Pin map, power architecture, measured hardware constants, LCD SPI timing analysis. |
| [`../README.md`](../README.md) | Product overview, architecture, quick start, and the tethered-drive safety rule. |
| [`ENGINEERING-GUIDE.md`](ENGINEERING-GUIDE.md) | Serial CLI, calibration, movement research, and detailed operating procedures. |
| [`../LICENSE.md`](../LICENSE.md) | MIT software scope, CERN-OHL-S hardware scope, and third-party exclusions. |

## Operational

| Doc | Why |
|---|---|
| [`GIT-NOTES.md`](GIT-NOTES.md) | Which account owns the repo, and the two auth traps that make pushes fail with a misleading "Repository not found". |

## Review findings

[`key-issues/`](key-issues/) — one file per domain, produced by the specialist
review agents on 2026-08-07 against commit `3b53db9`.

| File | Domain | Notable |
|---|---|---|
| [`key-issues/RELY.md`](key-issues/RELY.md) | Reliability | RELY-1/2/3 fixed; races and unguarded prints remain |
| [`key-issues/SEC.md`](key-issues/SEC.md) | Security | Unauthorised actuation analysis |
| [`key-issues/DSGN.md`](key-issues/DSGN.md) | Design | DSGN-1/2/7 fixed; STOP was overridable |
| [`key-issues/PERF.md`](key-issues/PERF.md) | Performance | 45 KB of RAM held for an off-by-default feature |
| [`key-issues/TEST.md`](key-issues/TEST.md) | Testing | The WS ownership state machine is untested |
| [`key-issues/READ.md`](key-issues/READ.md) | Readability | `main.cpp` is 10.8k lines; staged-split plan |
| [`key-issues/DEP.md`](key-issues/DEP.md) | Dependencies | Resolved — versions are now pinned exactly |
| [`key-issues/DOCS.md`](key-issues/DOCS.md) | Documentation | Partly resolved by this index and the README rewrite |

**Status is tracked in the finding files themselves**, not here — check the
`Status:` line on each finding rather than trusting this table.

## Measurement records

Individual bench runs, kept because they are evidence rather than narrative:

- `bench-run-001.md` … `bench-run-004.md` — coast compensation, α tuning
- `scale-check-001.md` — gyro scale verification (<0.28 % error)
- [`../IMU-RESEARCH.md`](../IMU-RESEARCH.md) — drift research synthesis
- `research/` — 8 background-agent research reports on IMU/RF topics

## Conventions

- A constant with a comment giving its **measured** value and derivation is
  evidence. Do not "tidy" those comments away.
- `MEASURED` vs `ASSUMED` is called out explicitly in `PROJECT-HISTORY.md` §4.
  Where something is assumed, it is marked as such deliberately.
- Movement cannot be validated while tethered. See the README.
