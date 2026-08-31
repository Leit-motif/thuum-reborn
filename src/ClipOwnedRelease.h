#pragma once

// CLIP-OWNED SPELLFIRE ROUTING (ADR-0009).
//
// Vanilla `shout_behavior` fires `Voice_SpellFire_Event` at 0.100 s on every exhale generator.
// This project's pack stamps the same name on the playing HKX at the visual release. When that
// annotation is later than `kClipOwnedSpellFireMinS`, the player hook intercepts the generator
// fire so the shout system sees the clip's event instead.
//
// FREE OF `RE::` AND SKSE, for the same reason `CastIntentSlot.h` is: the fail-open rule has to
// be checkable without launching the game. Sampling the playing clip is a graph walk and lives
// in `ShoutChainEngine.cpp`.
//
// NPCs never reach this function. The anim-event hook is `VTABLE_PlayerCharacter[2]` only.

#include <optional>

namespace ShoutMCO {
    // ADR-0009: later than ~0.15 s. Equal to the floor fails open, so a pack that
    // stamped vanilla's own 0.100 s (or anything up to the floor) is not intercepted.
    inline constexpr float kClipOwnedSpellFireMinS = 0.15f;

    enum class SpellFireRoute {
        kDeliver,             // shout system sees this event
        kInterceptGenerator,  // swallow before TESObjectREFR; wait for the clip's event
    };

    [[nodiscard]] inline bool ClipOwnsLateSpellFire(std::optional<float> a_clipSpellFireS) {
        return a_clipSpellFireS.has_value() && *a_clipSpellFireS > kClipOwnedSpellFireMinS;
    }

    // `a_clipSpellFireS` is the playing exhale's `Voice_SpellFire_Event` annotation time in
    // seconds, or nullopt when it cannot be read. `a_clipLocalTimeS` is that clip's current
    // local time when the event arrived; nullopt if unread. `a_alreadyInterceptedThisShout`
    // is the one-intercept latch, so a missing local-time sample still delivers the second fire.
    [[nodiscard]] inline SpellFireRoute DecideClipOwnedSpellFire(
        std::optional<float> a_clipSpellFireS, std::optional<float> a_clipLocalTimeS,
        bool a_alreadyInterceptedThisShout) {
        if (!ClipOwnsLateSpellFire(a_clipSpellFireS)) {
            return SpellFireRoute::kDeliver;
        }
        const float ownedS = *a_clipSpellFireS;
        // Already at or past the annotation: this IS the clip fire (or we missed the generator).
        constexpr float kFrameSlopS = 0.02f;
        if (a_clipLocalTimeS.has_value() && *a_clipLocalTimeS + kFrameSlopS >= ownedS) {
            return SpellFireRoute::kDeliver;
        }
        if (a_alreadyInterceptedThisShout) {
            return SpellFireRoute::kDeliver;
        }
        return SpellFireRoute::kInterceptGenerator;
    }
}
