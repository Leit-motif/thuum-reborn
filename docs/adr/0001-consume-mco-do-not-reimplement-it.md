# ADR-0001 — Consume MCO's events; never reimplement its attacks

Date: 2026-07-29
Status: accepted

## Context

Thu'um chains shouts into attacks today, but does it by injecting a hand-rolled combo state
machine into `shout_behavior` — 49 `hkbStateMachineStateInfo` and its own roll attack and
two-hit chain. That internal combo is precisely what fights MCO: two systems both claim to own
what happens after a shout.

MSCO faced the analogous problem for magic casting and solved it differently. Its
`magicbehavior` patch is large (179 files), but its coupling to MCO is four files wide:
`magicbehavior` imports three MCO names (`MCO_EndAnimation`, `MCO_Recovery`,
`MCO_IsInRecovery`), adds one transition on `MCO_EndAnimation`, activates one modifier on
`MCO_Recovery`, and binds one variable. It owns its own `MSCO_*` vocabulary and never reaches
into MCO's.

MSCO's size is not the lesson. It needed 179 files because it replaced casting with a genuine
multi-stage combo — start, loop, release, per hand, plus dual. Vanilla shout is already just
inhale → exhale.

## Decision

Adopt MSCO's coupling discipline at Thu'um's scale.

- Own a `SHOUT_*` vocabulary, injected as graph data via BDI.
- Couple to MCO only by importing its event and variable names into `shout_behavior`'s string
  data, and by DLL-issued events.
- **Never reimplement MCO's attacks inside `shout_behavior`.** MCO's attacks live in
  `1hm_behavior` where OAR selects them. The shout graph's job is to get out of the way at the
  right moment, not to play an attack.

Budget the `shout_behavior` patch at 5–20 files. Growth past that is the signal that this ADR
is being violated.

## Consequences

- The engine keeps working when MCO updates, because it depends on MCO's public event names
  rather than its internal structure.
- Every weapon family works for free — `1hm_behavior` is the universal attack graph
  (207 `1HM`, 140 `2HM`, 92 `H2H`, 92 `Bow`, 51 `DW`, 6 `Crossbow`, 1 `Staff` states).
- Thu'um's existing `shout_behavior` patch is replaced wholesale, not adapted. Its
  `#0244.txt` transition shape is reused; its destination is not.
- Cross-graph name linkage is load-bearing. Proven: MSCO's `magicbehavior` binds
  `MCO_IsInRecovery`, which MCO declares in `1hm_behavior`.
