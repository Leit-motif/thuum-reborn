#pragma once

// THE ONE GLOBAL PENDING CAST-INTENT SLOT, WITH NO GAME IN IT (ADR-0008).
//
// ShoutMCO owns one pending intent at a time. That was first true of the player's own
// shout press; this is the same slot generalised so a driver's intent can occupy it, and so the
// two can displace each other. Everything about WHEN to release lives in the engine; everything
// about WHO is waiting and the exactly-once guarantee lives here.
//
// IT IS DELIBERATELY FREE OF `RE::`, SKSE AND THE ENGINE LOCK. That is not tidiness -- it is the
// only reason the version negotiation, replacement, exactly-once and stale-handle rules can be
// tested at all. The game cannot run in a test, so the rules that must not regress are kept
// somewhere that can. `tests/cast_intent_tests.cpp` is the whole of that coverage.
//
// DISPATCH IS NOT DONE HERE, AND THAT SEPARATION IS THE POINT. Every retiring call returns the
// entry to notify rather than calling it. A callback invoked while this slot's mutex was held
// could re-enter `Take` from inside the callback -- which the public header explicitly allows, as
// it is how a driver chains -- and deadlock. So: mutate under the lock, return the victim, let the
// caller call it once the lock is gone. See `Notify`.

#include <cstdint>
#include <mutex>
#include <optional>

#include "ShoutMCO_CastIntent.h"

namespace ShoutMCO {
    // Who owns the pending intent. The engine needs this because the two are released by
    // different machinery: a vanilla press is replayed as a synthesized button event, a driver
    // intent is a callback. The RELEASE CONDITION is not chosen from here -- that stays with
    // `ShoutChainEngine::ShoutHoldReason`, which knows whether we are waiting on an MCO attack
    // becoming shout-admissible (`IsAttacking` false) or a shout's own `shoutStop`.
    enum class CastIntentOwner : std::uint8_t {
        kNone = 0,
        kVanilla,  // the player's own shout press, replayed through the vanilla handler
        kDriver    // an external driver, released through its C callback
    };

    // A retirement that still owes somebody exactly one callback. Returned by every call that can
    // empty the slot; `Notify()` is what actually spends it.
    struct CastIntentNotice {
        ShoutMCO_CastCallback callback = nullptr;
        void*                 context = nullptr;
        ShoutMCO_CastHandle   handle = SHOUTMCO_CAST_HANDLE_INVALID;
        ShoutMCO_CastOutcome  outcome = SHOUTMCO_CAST_ABANDON;
        ShoutMCO_CastCause    cause = SHOUTMCO_CAUSE_NONE;

        // Spend it. Safe to call on a notice that owes nothing, which is the common case -- it is
        // how "there was nothing to displace" travels back without the caller branching.
        void Notify() const {
            if (callback) callback(handle, outcome, static_cast<std::uint32_t>(cause), context);
        }
        [[nodiscard]] bool Owed() const { return callback != nullptr; }
    };

    class CastIntentSlot {
    public:
        // Put a DRIVER intent in the slot, displacing whatever was there.
        //
        // The returned notice carries the displaced owner's ABANDON/REPLACED callback when the
        // previous occupant was a driver. A displaced VANILLA press owes no callback here -- it has
        // no callback to owe -- so the notice comes back empty and the caller reads `displaced`
        // to know it must abandon the vanilla press through its own path.
        struct TakeResult {
            ShoutMCO_CastHandle handle = SHOUTMCO_CAST_HANDLE_INVALID;
            CastIntentOwner     displaced = CastIntentOwner::kNone;
            CastIntentNotice    notice{};
        };

        // `a_reasonTag` is WHICH confirmed state this intent is waiting for, carried opaquely so
        // this file stays free of the engine's enum. The caller maps
        // `ShoutChainEngine::ShoutHoldReason` onto it.
        //
        // IT IS NOT A DIAGNOSTIC. A cold review of the first build found the defect it exists to
        // close: with no reason recorded, ANY confirmed state released ANY pending intent, so an
        // intent deferred behind a live shout could be released early by an unrelated MCO attack's
        // `inRdy`. The release gate has to be able to ask what this intent is actually waiting for.
        [[nodiscard]] TakeResult TakeDriver(ShoutMCO_CastCallback a_callback, void* a_context,
                                            std::uint32_t a_reasonTag) {
            TakeResult result{};
            std::scoped_lock lock(m_mutex);
            result.displaced = m_owner;
            result.notice = MakeNoticeLocked(SHOUTMCO_CAST_ABANDON, SHOUTMCO_CAUSE_REPLACED);

            // Monotonic and never reused, so a handle from a retired intent can never name a live
            // one no matter how many intents a session sees. Starting at 1 keeps 0 invalid.
            m_handle = ++m_nextHandle;
            m_callback = a_callback;
            m_context = a_context;
            m_reasonTag = a_reasonTag;
            m_owner = CastIntentOwner::kDriver;
            result.handle = m_handle;
            return result;
        }

        // Put the player's own press in the slot. Same replacement rule from the other side: a
        // real shout press is the newest valid intent and displaces a driver's, which gets its one
        // ABANDON/REPLACED. The vanilla press itself is tracked by `ShoutInputHook`'s `g_held`;
        // this only records that the slot is no longer the driver's.
        [[nodiscard]] CastIntentNotice TakeVanilla() {
            std::scoped_lock lock(m_mutex);
            auto notice = MakeNoticeLocked(SHOUTMCO_CAST_ABANDON, SHOUTMCO_CAUSE_REPLACED);
            m_handle = SHOUTMCO_CAST_HANDLE_INVALID;
            m_callback = nullptr;
            m_context = nullptr;
            m_reasonTag = 0;
            m_owner = CastIntentOwner::kVanilla;
            return notice;
        }

        // Retire whatever is pending, whoever owns it. Returns an empty notice when the slot was
        // already empty or held a vanilla press -- both mean "nobody is owed a C callback".
        [[nodiscard]] CastIntentNotice Retire(ShoutMCO_CastOutcome a_outcome,
                                              ShoutMCO_CastCause   a_cause) {
            std::scoped_lock lock(m_mutex);
            auto notice = MakeNoticeLocked(a_outcome, a_cause);
            ClearLocked();
            return notice;
        }

        // Retire only if `a_handle` still names the live intent. THIS IS THE STALE-HANDLE RULE:
        // a driver that cancels a handle already retired by a release, a replacement or the
        // watchdog gets an empty notice and `false`, and nothing fires a second time.
        struct CancelResult {
            bool             retired = false;
            CastIntentNotice notice{};
        };

        [[nodiscard]] CancelResult CancelHandle(ShoutMCO_CastHandle a_handle,
                                                ShoutMCO_CastCause  a_cause) {
            CancelResult result{};
            std::scoped_lock lock(m_mutex);
            if (a_handle == SHOUTMCO_CAST_HANDLE_INVALID || a_handle != m_handle ||
                m_owner != CastIntentOwner::kDriver) {
                return result;
            }
            result.notice = MakeNoticeLocked(SHOUTMCO_CAST_ABANDON, a_cause);
            ClearLocked();
            result.retired = true;
            return result;
        }

        // The player's press has ended -- released, abandoned or dropped by a load -- so the slot
        // is free again. Clears ONLY a vanilla occupant: a driver intent taken since must not be
        // wiped by the departure of the press it displaced, which is the desync a cold review
        // found when the vanilla path had no way back to `kNone` at all.
        void ClearVanilla() {
            std::scoped_lock lock(m_mutex);
            if (m_owner != CastIntentOwner::kVanilla) return;
            ClearLocked();
        }

        [[nodiscard]] CastIntentOwner Owner() const {
            std::scoped_lock lock(m_mutex);
            return m_owner;
        }
        [[nodiscard]] bool DriverPending() const {
            std::scoped_lock lock(m_mutex);
            return m_owner == CastIntentOwner::kDriver;
        }
        // Is a driver intent pending AND waiting on this particular confirmed state? The engine's
        // release gate asks this before it lets a confirmed state through, exactly as it asks the
        // input hook whether the held vanilla press is behind an attack or behind a shout.
        [[nodiscard]] bool DriverPendingFor(std::uint32_t a_reasonTag) const {
            std::scoped_lock lock(m_mutex);
            return m_owner == CastIntentOwner::kDriver && m_reasonTag == a_reasonTag;
        }
        [[nodiscard]] ShoutMCO_CastHandle CurrentHandle() const {
            std::scoped_lock lock(m_mutex);
            return m_handle;
        }

    private:
        // Builds the notice the CURRENT occupant is owed, without clearing anything. Only a driver
        // occupant owes a callback; a vanilla one is retired through the input hook's own path.
        [[nodiscard]] CastIntentNotice MakeNoticeLocked(ShoutMCO_CastOutcome a_outcome,
                                                        ShoutMCO_CastCause   a_cause) const {
            if (m_owner != CastIntentOwner::kDriver || !m_callback) return CastIntentNotice{};
            return CastIntentNotice{.callback = m_callback,
                                    .context = m_context,
                                    .handle = m_handle,
                                    .outcome = a_outcome,
                                    .cause = a_cause};
        }

        void ClearLocked() {
            m_handle = SHOUTMCO_CAST_HANDLE_INVALID;
            m_callback = nullptr;
            m_context = nullptr;
            m_reasonTag = 0;
            m_owner = CastIntentOwner::kNone;
        }

        mutable std::mutex    m_mutex;
        ShoutMCO_CastHandle   m_handle = SHOUTMCO_CAST_HANDLE_INVALID;
        ShoutMCO_CastCallback m_callback = nullptr;
        void*                 m_context = nullptr;
        std::uint32_t         m_reasonTag = 0;
        CastIntentOwner       m_owner = CastIntentOwner::kNone;
        std::uint64_t         m_nextHandle = 0;
    };

    // VERSION NEGOTIATION, kept here with the slot because it is the other rule that must be
    // testable without the game. Both halves are pure predicates over the header's constants.

    // Does ShoutMCO implement the major a driver asked `ShoutMCO_GetCastIntentApi` for? A mismatch
    // is not an error: the caller returns null and the driver falls back, which is the fail-open
    // rule from ADR-0008.
    [[nodiscard]] constexpr bool CastIntentMajorSupported(std::uint32_t a_requestedMajor) {
        return a_requestedMajor == SHOUTMCO_CAST_INTENT_VERSION_MAJOR;
    }

    // Is a request well-formed enough to act on? `structSize` is checked against the v1 struct
    // rather than for equality, so a driver compiled against a LATER minor -- whose struct grew at
    // the end -- still passes, and we simply never read past what v1 knows. A smaller struct means
    // fields we would read do not exist in the caller's allocation, which is a rejection and not a
    // negotiation.
    [[nodiscard]] inline bool CastIntentRequestValid(const ShoutMCO_CastRequest* a_request) {
        if (!a_request) return false;
        if (a_request->structSize < sizeof(ShoutMCO_CastRequest)) return false;
        if (!CastIntentMajorSupported(a_request->versionMajor)) return false;
        if (!a_request->callback) return false;
        if (a_request->flags != 0u) return false;  // reserved in v1
        return true;
    }
}
