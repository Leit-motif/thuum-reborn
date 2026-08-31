#include "PCH.h"
#include "CastIntentApi.h"

#include <atomic>

#include "EngineLock.h"
#include "Settings.h"
#include "ShoutChainEngine.h"
#include "ShoutInputHook.h"
#include "Trace.h"

using namespace SKSE;
using namespace SKSE::log;

namespace ShoutMCO {
    namespace {
        // THE one slot. Not one per driver: ADR-0008's rule is a single global pending intent, so
        // two drivers racing is the same replacement case as a driver racing the player.
        CastIntentSlot g_slot;

        // When the pending driver intent was taken, for the watchdog. Written under the slot's own
        // discipline (only ever alongside a take) and read on the watchdog pass; atomic because
        // those are different threads and it is an independent value, not part of a snapshot
        // (EngineLock.h rule 4).
        std::atomic<double> g_takenAtMs{0.0};

        void DispatchNow(const CastIntentNotice& a_notice) {
            if (!a_notice.Owed()) return;
            CastIntentApi::Dispatch(a_notice);
        }

        // --- the exported vtable -----------------------------------------------------------

        ShoutMCO_CastDecision Api_Request(const ShoutMCO_CastRequest* a_request,
                                          ShoutMCO_CastHandle*        a_outHandle) {
            if (a_outHandle) *a_outHandle = SHOUTMCO_CAST_HANDLE_INVALID;

            // Malformed or wrong-major requests are refused, not negotiated. The driver's fallback
            // is its own native behaviour, which is exactly what it does with no ShoutMCO present.
            if (!CastIntentRequestValid(a_request)) return SHOUTMCO_CAST_REJECTED;

            // A DISABLED ENGINE IS AN ABSENT ENGINE, and that is what the header promises a
            // rejection means: the driver behaves exactly as it would with no ShoutMCO present.
            // This returned PASS_THROUGH, which says something different -- "nothing to wait for,
            // cast now" -- while the enum documentation had already listed "disabled" among the
            // rejection causes.
            //
            // THE TWO OUTCOMES ARE NOT INTERCHANGEABLE IN THE CONTRACT, and it would be wrong to
            // land this on the grounds that they are: the header's own usage sketch casts on
            // PASS_THROUGH and refuses on REJECTED. What makes the change safe today is narrower
            // and specific to the one shipped driver -- Spell Hotbar 2 collapses every
            // non-deferred decision into its native refusal, so it cannot tell them apart. A
            // future driver that follows the sketch will, and for it a disabled engine correctly
            // means "behave as though I am not installed".
            const auto settings = Settings::Snapshot();
            if (!settings->enabled) return SHOUTMCO_CAST_REJECTED;

            // The hold decision is the ENGINE's, and it is the same one a vanilla press gets --
            // that is the whole point of ADR-0008. Nothing here inspects cooldowns, spells or the
            // driver's payload.
            //
            // ONE LOCKED BREATH for the verdict, the take, and the displaced press's abandonment,
            // so a press arriving between the verdict and the take would make
            // the decision describe a slot that is no longer the one being filled.
            CastIntentSlot::TakeResult taken{};
            {
                std::scoped_lock lock(detail::g_engineLock);
                const auto verdict = ShoutChainEngine::ShouldHoldShoutLocked();
                if (!verdict.hold) return SHOUTMCO_CAST_PASS_THROUGH;

                taken = g_slot.TakeDriver(a_request->callback, a_request->context,
                                          static_cast<std::uint32_t>(verdict.reason));
                g_takenAtMs.store(ShoutChainEngine::ElapsedMs(), std::memory_order_relaxed);

                // A displaced VANILLA press owes no C callback, but it is still a held press that
                // must be handed back rather than dropped. `AbandonHeldLocked` is
                // vanilla-only on purpose -- it must not retire the driver intent we just took.
                if (taken.displaced == CastIntentOwner::kVanilla) {
                    ShoutInputHook::AbandonHeldLocked("replaced by a driver cast intent"sv);
                }

                SHOUTMCO_TRACE("[{:10.2f}] >>> CAST INTENT deferred for a driver (handle {}, {})",
                               ShoutChainEngine::ElapsedMs(), taken.handle,
                               verdict.reason == ShoutChainEngine::ShoutHoldReason::kBehindShout
                                   ? "behind a live shout"sv
                                   : "behind a live MCO attack"sv);
            }

            // Outside the lock: the displaced driver's one callback, and the handle the caller
            // needs to cancel with.
            if (a_outHandle) *a_outHandle = taken.handle;
            DispatchNow(taken.notice);

            // AND THE CUT, WHICH A DRIVER INTENT DID NOT GET UNTIL NOW. A shout taken behind a
            // LIVE shout whose chain window is already open should end that exhale rather than
            // wait it out -- the same treatment the player's own swallowed press gets on its down
            // edge in `ShoutInputHook::ProcessButton`. That was the only caller, so a hotbar shout
            // fired into an open window sat through the rest of the exhale and then the 300 ms
            // pacing delay, while the identical press on the shout key cut it immediately.
            // Measured on one pair of standing shouts: 861 ms against 326 ms.
            //
            // Unconditional: the engine rechecks every precondition under its own lock -- whether a
            // shout is live, whether the window is open, whether anything is actually holding
            // behind it. `IsHoldingBehindShoutLocked` already counts a pending driver intent, so
            // the decision logic needed nothing; only this call was missing.
            //
            // QUEUED FOR THE MAIN THREAD, NOT CALLED HERE, AND THE DIFFERENCE IS THE WHOLE POINT.
            // The cut notifies the animation graph. The vanilla caller does that from the input
            // hook, whose stack belongs to the game. THIS caller's stack belongs to a DRIVER, in
            // the middle of its own cast attempt -- and a synchronous notify can dispatch graph
            // events straight back into that driver's own animation-event handler while its call
            // to us is still on the stack. Re-entering a mod that is mid-attempt is a hang waiting
            // for the right two frames, and it is not a hazard the vanilla site has or has ever
            // had to reason about.
            //
            // So the driver's `Request` returns first and the cut lands on the next drain. It
            // costs about a frame against the several hundred milliseconds this saves, and it
            // matches how every other callback out of this file is delivered -- `Dispatch` marshals
            // for the same reason, one line below where the same rule was already written down.
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([]() { ShoutChainEngine::CutShoutForQueuedShoutChain(); });
            }
            return SHOUTMCO_CAST_DEFERRED;
        }

        int Api_Cancel(ShoutMCO_CastHandle a_handle) {
            const auto result = g_slot.CancelHandle(a_handle, SHOUTMCO_CAUSE_CANCELLED);
            if (!result.retired) return 0;  // stale is normal and needs no reporting

            // TRACED, because this was the one slot transition that was not, and the gap is
            // expensive to read past. A driver that serves a deferred cast from its own latch
            // cancels here on the way -- Spell Hotbar 2 does exactly that -- so a trace showed an
            // intent deferred, a shout cut for it, and then nothing at all: no release, no abandon,
            // no displacement. It reads as a leaked slot that would cut the NEXT shout for an
            // intent nobody is waiting on. It was not one; the cancellation was simply invisible.
            SHOUTMCO_TRACE("[{:10.2f}] >>> CAST INTENT cancelled by its driver (handle {})",
                           ShoutChainEngine::ElapsedMs(), a_handle);
            DispatchNow(result.notice);
            return 1;
        }

        ShoutMCO_CastStatus Api_Status() { return SHOUTMCO_STATUS_ACTIVE; }

        // Filled once, handed out by pointer, and owned by this module for the life of the
        // process -- the header promises exactly that. `constexpr` so it is built at compile time
        // and cannot be caught half-initialised by a driver negotiating during static init.
        constexpr ShoutMCO_CastIntentApi g_api{
            .structSize = sizeof(ShoutMCO_CastIntentApi),
            .versionMajor = SHOUTMCO_CAST_INTENT_VERSION_MAJOR,
            .versionMinor = SHOUTMCO_CAST_INTENT_VERSION_MINOR,
            .reserved = 0,
            .Request = &Api_Request,
            .Cancel = &Api_Cancel,
            .Status = &Api_Status,
        };
    }

    void CastIntentApi::Install() {
        // ONE line, at startup, and nothing per-request. A driver that cannot find us says so on
        // its own status surface; neither side spams the log (ADR-0008).
        log::info("[ShoutMCO] cast-intent driver API v{}.{} available as \"{}\"",
                  SHOUTMCO_CAST_INTENT_VERSION_MAJOR, SHOUTMCO_CAST_INTENT_VERSION_MINOR,
                  SHOUTMCO_CAST_INTENT_EXPORT);
    }

    bool CastIntentApi::DriverPending() { return g_slot.DriverPending(); }

    bool CastIntentApi::DriverPendingBehindAttack() {
        return g_slot.DriverPendingFor(
            static_cast<std::uint32_t>(ShoutChainEngine::ShoutHoldReason::kBehindAttack));
    }

    bool CastIntentApi::DriverPendingBehindShout() {
        return g_slot.DriverPendingFor(
            static_cast<std::uint32_t>(ShoutChainEngine::ShoutHoldReason::kBehindShout));
    }

    void CastIntentApi::VanillaPressLeftSlot() { g_slot.ClearVanilla(); }

    void CastIntentApi::Dispatch(const CastIntentNotice& a_notice) {
        if (!a_notice.Owed()) return;

        // EXACTLY ONCE, ON THE MAIN THREAD. The retirement already happened -- the slot is empty
        // and this notice is the only copy -- so the guarantee survives even if the task drain
        // runs much later. Retiring and notifying are deliberately not one step: retirement must
        // be atomic under the slot's mutex, and delivery must not be.
        //
        // THIS FUNCTION IS SAFE TO CALL UNDER A LOCK, and two callers do (`VanillaPressTookSlot`
        // from the input hook's locked region, `CheckWatchdog` from the engine's locked watchdog
        // pass). It never runs the driver's callback inline -- it only queues one -- so no third
        // party code runs while we hold anything. What must never happen under a lock is calling
        // `CastIntentNotice::Notify()` directly, because a driver is allowed to call `Request`
        // from inside its callback and would then re-enter the slot's mutex.
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            // No task interface means no main thread to marshal onto. Calling inline is worse than
            // dropping: a driver's callback re-enters the game from whatever thread we are on. A
            // dropped RELEASE is a cast that does not happen, which is the failure the driver
            // already handles.
            log::error("[ShoutMCO] no task interface to deliver a cast-intent callback -- dropping "
                       "handle {}", a_notice.handle);
            return;
        }
        task->AddTask([notice = a_notice]() { notice.Notify(); });
    }

    void CastIntentApi::ReleaseDriverIntent() {
        const auto notice = g_slot.Retire(SHOUTMCO_CAST_RELEASE, SHOUTMCO_CAUSE_READY);
        if (!notice.Owed()) return;
        SHOUTMCO_TRACE("[{:10.2f}] >>> CAST INTENT released to its driver (handle {})",
                       ShoutChainEngine::ElapsedMs(), notice.handle);
        Dispatch(notice);
    }

    void CastIntentApi::AbandonDriverIntent(ShoutMCO_CastCause a_cause) {
        const auto notice = g_slot.Retire(SHOUTMCO_CAST_ABANDON, a_cause);
        if (!notice.Owed()) return;
        SHOUTMCO_TRACE("[{:10.2f}] >>> CAST INTENT abandoned (handle {}, cause {})",
                       ShoutChainEngine::ElapsedMs(), notice.handle,
                       static_cast<std::uint32_t>(a_cause));
        Dispatch(notice);
    }

    void CastIntentApi::VanillaPressTookSlot() {
        const auto notice = g_slot.TakeVanilla();
        if (!notice.Owed()) return;
        SHOUTMCO_TRACE("[{:10.2f}] >>> CAST INTENT displaced by the player's own shout press "
                       "(handle {})", ShoutChainEngine::ElapsedMs(), notice.handle);
        Dispatch(notice);
    }

    void CastIntentApi::CheckWatchdog(std::uint32_t a_capMs) {
        if (!g_slot.DriverPending()) return;
        const auto waited = ShoutChainEngine::ElapsedMs() -
                            g_takenAtMs.load(std::memory_order_relaxed);
        if (waited <= static_cast<double>(a_capMs)) return;
        log::warn("[ShoutMCO] cast intent abandoned after {:.1f}ms -- no confirmed state arrived",
                  waited);
        AbandonDriverIntent(SHOUTMCO_CAUSE_WATCHDOG);
    }
}

extern "C" __declspec(dllexport) const ShoutMCO_CastIntentApi* ShoutMCO_GetCastIntentApi(
    std::uint32_t a_requestedMajor) {
    // A major we do not implement returns null, and that is NOT an error -- it is the fail-open
    // rule. The driver carries on natively and reports "incompatible" on its own status surface.
    if (!ShoutMCO::CastIntentMajorSupported(a_requestedMajor)) return nullptr;
    return &ShoutMCO::g_api;
}
