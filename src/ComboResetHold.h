#pragma once

#include <cstdint>
#include <optional>

// MCO'S TEARDOWN RESET, HELD ONE FRAME BEFORE IT REACHES THE QUEUED SNAPSHOT.
//
// `RefreshQueuedResumeLocked` copies every sampled reading into the snapshot a queued shout will
// resume from, and freezes at the end markers (`MCO_AttackExitNotify` / `attackStop`). That freeze
// assumed MCO's reset to the ready values arrives AFTER the markers. It does not always: finding
// F1 caught `@SGVI|MCO_nextattack|1`
// and `@SGVI|MCO_nextpowerattack|1` landing 40 us BEFORE `JumpAttackStop` / `MCO_AttackExitNotify`
// on a non-cancelled attack, in the same graph update -- the refresh took the 1, the freeze kept
// it, and the shout resumed at 1.
//
// Rejecting every write of 1 is wrong too: a combo that wraps writes 1 legitimately, mid-attack.
// What separates the two is the FRAME. The teardown reset shares its frame with the end markers;
// a wrap lands at the attack's advance and is followed by later-frame events (hits, windows, the
// markers themselves). So a reading that moves a counter to its ready value is held, committed
// when a later frame proves the attack was still running, and dropped when an end marker arrives
// in the frame it was taken.
//
// FREE OF `RE::` AND SKSE, like `AttackQueuedRelease.h`: the frame number is a game read and is
// taken in phase A; this file only decides.

namespace ShoutMCO {
    struct ComboReading {
        int nextAttack = 0;
        int nextPowerAttack = 0;
        int currentAttack = 0;
        int currentPowerAttack = 0;

        friend bool operator==(const ComboReading&, const ComboReading&) = default;
    };

    struct ComboResetHold {
        bool          held = false;
        std::uint32_t frame = 0;
        ComboReading  reading{};
    };

    // Does `a_reading` move any counter from a live value back to MCO's ready value (next 1,
    // current 0)?
    [[nodiscard]] inline bool MovesToReady(const ComboReading& a_base, const ComboReading& a_reading) {
        return (a_reading.nextAttack == 1 && a_base.nextAttack > 1) ||
               (a_reading.nextPowerAttack == 1 && a_base.nextPowerAttack > 1) ||
               (a_reading.currentAttack == 0 && a_base.currentAttack > 0) ||
               (a_reading.currentPowerAttack == 0 && a_base.currentPowerAttack > 0);
    }

    struct ComboRefreshResult {
        // The value the snapshot should hold after this reading, or empty to leave it alone.
        std::optional<ComboReading> commit;
        // This reading was held rather than committed.
        bool heldNow = false;
        // A reading held in an earlier frame was committed by this one.
        bool releasedEarlier = false;
    };

    // A sampled reading taken while the attack is live and no end marker has arrived.
    [[nodiscard]] inline ComboRefreshResult OnLiveComboReading(ComboResetHold&     a_hold,
                                                               const ComboReading& a_committed,
                                                               const ComboReading& a_reading,
                                                               std::uint32_t       a_frame) {
        ComboRefreshResult out{};
        ComboReading       base = a_committed;
        if (a_hold.held && a_hold.frame != a_frame) {
            // A later frame, and the attack is still live: the held reading was not the teardown.
            base = a_hold.reading;
            out.commit = a_hold.reading;
            out.releasedEarlier = true;
            a_hold = {};
        }
        if (a_reading == base) {
            // Back to the committed value within the hold's frame: nothing is pending any more.
            a_hold = {};
            return out;
        }
        if (MovesToReady(base, a_reading)) {
            // Same frame as an earlier hold: the reset's second write, which replaces the first.
            a_hold = {.held = true, .frame = a_frame, .reading = a_reading};
            out.heldNow = true;
            return out;
        }
        a_hold = {};
        out.commit = a_reading;
        return out;
    }

    // An end marker, `inRdy`, or the shout beginning. A reading held in this same frame is the
    // teardown reset and is dropped; one held in an earlier frame is committed.
    [[nodiscard]] inline std::optional<ComboReading> SettleComboResetHold(ComboResetHold& a_hold,
                                                                          std::uint32_t   a_frame) {
        if (!a_hold.held) return std::nullopt;
        const auto held = a_hold;
        a_hold = {};
        if (held.frame == a_frame) return std::nullopt;
        return held.reading;
    }
}
