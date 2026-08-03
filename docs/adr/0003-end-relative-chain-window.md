# ADR-0003 — Chain window timing is end-relative, not fractional

Date: 2026-07-29
Status: accepted — amended 2026-07-29 and 2026-08-03, see the "Amendment" sections below

## Context

The chain window has to open partway through the exhale so a shout reads as a complete gesture
before an attack takes over. The obvious formulation is a fraction — "open at 60% through".

Two facts make that the wrong choice.

**Fractions are not expressible.** `hkbClipTriggerArray` takes `localTime` in seconds, either
absolute from clip start or relative to clip end. There is no fractional form, and the DLL does
not cheaply know the active clip's duration.

**Pack durations vary wildly for the same filename.** `exhale_long` measures 1.033333s in one
pack and 3.933333s in another, across 137 files from two authors. A fixed fraction would
therefore produce window *lengths* of 0.41s and 1.57s for the same setting — so the same button
press would feel different depending on which animation pack the player installed. That is
exactly the kind of pack-dependent behavior the engine exists to eliminate.

Vanilla already demonstrates the alternative: the `*_Shout_ExhaleSlowTime` clips carry
`shoutStop` at `localTime = −0.915`, `relativeToEndOfClip = true`.

## Decision

The chain window opens at a fixed offset **before the end of the exhale clip** —
`localTime = −0.4`, `relativeToEndOfClip = true`. The offset is **fixed by the patch a player
installs and is not tunable at runtime** (amended — the original wording said "INI-tunable as a
length in seconds", which is not achievable; see below).

This generalises to principle 3 in the spec: **no absolute-from-start trigger times for
pack-facing timing.** The one absolute trigger that matters, vanilla's `Voice_SpellFire_Event`
at 0.100s, is safe only because it is smaller than any pack's shortest exhale.

Attack input before the window opens is buffered for the duration of the shout and fires the
instant the window opens; input inside the window cuts immediately.

## Amendment, 2026-07-29 — "INI-tunable" is wrong

The decision as first written said the window is "INI-tunable as a length in seconds". It cannot
be, and the Decision section above now reads as amended. The
window is an `hkbClipTriggerArray` `localTime`, which is **static graph data baked by Nemesis**,
and BDI injects variables rather than trigger times. No DLL setting can move it, so the window
length is fixed by the patch a player installs.

Two ways to recover tunability if it turns out to matter, neither yet chosen: ship the trigger at
the widest useful offset and have the DLL *shrink* the window by ignoring the signal for a
configurable interval after it arrives; or ship more than one patch variant. The rest of the
decision — end-relative rather than fractional — is unaffected and still holds.

The design intent stands too: nothing in this engine should need per-player tuning. See
`CONTEXT.md` runtime finding 10a.

## Consequences

- Window length is constant across packs. A 1.03s exhale opens at 61% and a 3.93s exhale at
  90%, but both give the player the same 0.4s of opportunity.
- Long exhales demand real commitment — 3.5s on `exhale_long` — which suits deliberate,
  weighty combat. Input buffering keeps that from punishing an early press.
- Acceptance gate A9 must measure the window on both a 1.03s and a 3.93s exhale and show they
  match.
- Buffering is scoped to the whole shout. This mirrors MCO's own `MCO_InputBuffer` concept and
  naming. **Amended 2026-08-03 — see below.**

## Amendment, 2026-08-03 — the INI cap is gone

This decision's Consequences said buffering was "scoped to the whole shout by default, with an INI
cap for stricter play". That cap was `iBufferMs`, and ticket 17 demoted it to an internal constant
along with eleven others when the shipped configuration surface was cut from nineteen settings to
five. The cap still exists in code at its default of `0`, i.e. buffer for the whole shout — the
behaviour this ADR describes is unchanged — but a player can no longer set it, so "for stricter
play" no longer describes anything they can reach.

This is the same conclusion the 2026-07-29 amendment reached from the other direction, and it is
worth stating plainly rather than leaving two amendments to be read together: **nothing in this
engine should need per-player tuning** (`CONTEXT.md` finding 10a). A ticket that needs the cap back
adds its parse case and its INI comment together; until then it is a constant.
