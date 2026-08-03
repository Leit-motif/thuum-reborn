# ADR-0002 — The engine never inspects shout cooldown state

Date: 2026-07-29
Status: accepted

## Context

One of the three required chain directions is shout → shout. It cannot happen in normal play,
for gameplay reasons rather than graph reasons.

`UnrelentingForceShout` (`013E07:Skyrim.esm`) has `RecoveryTime` values of 15, 20 and 45
seconds across its three words. Skyrim runs a **single voice-recovery timer per actor**, not
one per shout, so during that window no shout of any kind is available. Amulet plus Blessing of
Talos stack to roughly −40%, leaving about 9 seconds. Combo timescale is under a second.

Several mods already solve this by making cooldowns per-shout — TISC (173766, updated
2026-03-02, which explicitly advertises chaining), Individual Shout Cooldown Remake (37099),
Individual Shout Cooldowns (7433), Shout Cooldowns Parallelized (72262). They all work by
writing the same single recovery timer, so any two of them conflict, and so would we.

Building per-shout cooldowns ourselves is not technically hard — a per-shout timer map plus
writes to the actor's voice recovery time. The problem is category: it is a balance system, not
behavior-graph work. Owning it would make this a gameplay overhaul that happens to do
animation, and would inherit every compatibility argument in that list.

## Decision

Shout cooldown policy is out of scope, and the engine **never reads cooldown state** — not the
voice recovery timer, not "is a shout currently available", not anything derived from either.

The chain window opens on animation timing alone. Shout input during the window is honored
unconditionally: issue the attempt and let the game refuse it if it wants to.

Documentation recommends TISC as an optional companion.

## Consequences

- Automatic compatibility with every cooldown mod, including ones not yet written, because we
  never contend for the timer or branch on its value.
- Shout → shout is structurally supported at near-zero cost — it reuses the same code path as
  the other two directions — but is unreachable under vanilla balance. That is correct
  behavior, not a defect.
- Acceptance for shout → shout needs a fixture: `player.setav shoutrecoverymult 0`, or TISC
  installed. The fixture must be named in the evidence.
- If a future feature seems to need cooldown state, that is a signal it belongs in a separate
  optional plugin, not in the engine.
