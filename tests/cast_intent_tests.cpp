// The driver cast-intent slot: version negotiation, replacement, exactly-once callbacks and
// stale-handle rejection.
//
// THESE RUN ON THE HOST, WITH NO SKYRIM AND NO COMMONLIBSSE. That is the whole reason
// `CastIntentSlot.h` is free of game types: a rule that can only be checked by launching the game
// is a rule that gets checked once and then drifts. Everything here is a pure state machine.
//
// WHAT THIS CANNOT SHOW, stated so nobody reads a green run as more than it is: nothing here
// proves a release lands on the main thread, that a real driver's cast plays, or that the timing
// is right. Those are runtime gates and stay open until driven in game (AGENTS.md evidence rules).
//
// No test framework, deliberately -- the repo has none, and one assert macro covers this.

#include <cstdio>
#include <string>
#include <vector>

#include "CastIntentSlot.h"

// Stand-ins for `ShoutChainEngine::ShoutHoldReason`, which this file cannot see and deliberately
// does not need to: the slot carries the tag opaquely.
namespace {
    constexpr std::uint32_t kBehindAttack = 1;
    constexpr std::uint32_t kBehindShout = 2;
}

namespace {
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool a_ok, const char* a_what, int a_line) {
        ++g_checks;
        if (a_ok) return;
        ++g_failures;
        std::printf("  FAIL (line %d): %s\n", a_line, a_what);
    }

#define CHECK(expr) Check((expr), #expr, __LINE__)

    // One recorded callback delivery. The tests assert on the whole log, because "exactly once"
    // is a statement about a sequence and not about any single call.
    struct Delivery {
        ShoutMCO_CastHandle  handle;
        ShoutMCO_CastOutcome outcome;
        std::uint32_t        cause;
        void*                context;
    };

    std::vector<Delivery> g_log;

    void Record(ShoutMCO_CastHandle a_handle, ShoutMCO_CastOutcome a_outcome, std::uint32_t a_cause,
                void* a_context) {
        g_log.push_back(Delivery{a_handle, a_outcome, a_cause, a_context});
    }

    void ResetLog() { g_log.clear(); }

    ShoutMCO_CastRequest ValidRequest() {
        ShoutMCO_CastRequest req{};
        req.structSize = sizeof(ShoutMCO_CastRequest);
        req.versionMajor = SHOUTMCO_CAST_INTENT_VERSION_MAJOR;
        req.versionMinor = SHOUTMCO_CAST_INTENT_VERSION_MINOR;
        req.flags = 0;
        req.callback = &Record;
        req.context = nullptr;
        return req;
    }

    // ---- version negotiation -------------------------------------------------------------

    void TestVersionNegotiation() {
        std::printf("version negotiation\n");
        using namespace ShoutMCO;

        CHECK(CastIntentMajorSupported(SHOUTMCO_CAST_INTENT_VERSION_MAJOR));
        CHECK(!CastIntentMajorSupported(SHOUTMCO_CAST_INTENT_VERSION_MAJOR + 1));
        CHECK(!CastIntentMajorSupported(0));

        auto req = ValidRequest();
        CHECK(CastIntentRequestValid(&req));

        CHECK(!CastIntentRequestValid(nullptr));

        // A driver compiled against a later MINOR has a struct that grew at the end. It must still
        // be accepted -- we only ever read the v1 prefix.
        auto larger = ValidRequest();
        larger.structSize = sizeof(ShoutMCO_CastRequest) + 16;
        larger.versionMinor = SHOUTMCO_CAST_INTENT_VERSION_MINOR + 5;
        CHECK(CastIntentRequestValid(&larger));

        // A struct SMALLER than v1 does not contain the fields we would read. Rejected.
        auto smaller = ValidRequest();
        smaller.structSize = sizeof(ShoutMCO_CastRequest) - 1;
        CHECK(!CastIntentRequestValid(&smaller));

        auto wrongMajor = ValidRequest();
        wrongMajor.versionMajor = SHOUTMCO_CAST_INTENT_VERSION_MAJOR + 1;
        CHECK(!CastIntentRequestValid(&wrongMajor));

        auto noCallback = ValidRequest();
        noCallback.callback = nullptr;
        CHECK(!CastIntentRequestValid(&noCallback));

        // `flags` is reserved in v1. A driver setting a bit we do not implement is asking for
        // behaviour we cannot provide, so it is refused rather than silently ignored.
        auto flagged = ValidRequest();
        flagged.flags = 1;
        CHECK(!CastIntentRequestValid(&flagged));
    }

    // ---- replacement ---------------------------------------------------------------------

    void TestReplacement() {
        std::printf("replacement\n");
        using namespace ShoutMCO;
        ResetLog();

        CastIntentSlot slot;
        CHECK(slot.Owner() == CastIntentOwner::kNone);
        CHECK(slot.CurrentHandle() == SHOUTMCO_CAST_HANDLE_INVALID);

        int firstContext = 1;
        int secondContext = 2;

        const auto first = slot.TakeDriver(&Record, &firstContext, kBehindAttack);
        CHECK(first.handle != SHOUTMCO_CAST_HANDLE_INVALID);
        CHECK(first.displaced == CastIntentOwner::kNone);
        CHECK(!first.notice.Owed());  // nothing was there to displace
        first.notice.Notify();
        CHECK(g_log.empty());
        CHECK(slot.DriverPending());

        // Newest valid intent replaces the previous one, and the displaced owner is told once.
        const auto second = slot.TakeDriver(&Record, &secondContext, kBehindAttack);
        CHECK(second.handle != first.handle);
        CHECK(second.handle > first.handle);  // monotonic
        CHECK(second.displaced == CastIntentOwner::kDriver);
        CHECK(second.notice.Owed());
        second.notice.Notify();
        CHECK(g_log.size() == 1);
        CHECK(g_log[0].handle == first.handle);
        CHECK(g_log[0].outcome == SHOUTMCO_CAST_ABANDON);
        CHECK(g_log[0].cause == SHOUTMCO_CAUSE_REPLACED);
        CHECK(g_log[0].context == &firstContext);

        // The player's own press is also an intent, and it wins the slot the same way.
        ResetLog();
        const auto displacedByVanilla = slot.TakeVanilla();
        CHECK(displacedByVanilla.Owed());
        displacedByVanilla.Notify();
        CHECK(g_log.size() == 1);
        CHECK(g_log[0].handle == second.handle);
        CHECK(g_log[0].cause == SHOUTMCO_CAUSE_REPLACED);
        CHECK(slot.Owner() == CastIntentOwner::kVanilla);
        CHECK(!slot.DriverPending());

        // And a driver displaces a vanilla press. No C callback is owed -- a vanilla press has
        // none -- but the caller is told what it displaced so it can retire the held press
        // through the input hook's own path.
        ResetLog();
        const auto third = slot.TakeDriver(&Record, nullptr, kBehindAttack);
        CHECK(third.displaced == CastIntentOwner::kVanilla);
        CHECK(!third.notice.Owed());
        third.notice.Notify();
        CHECK(g_log.empty());
    }

    // ---- exactly-once --------------------------------------------------------------------

    void TestExactlyOnce() {
        std::printf("exactly-once callbacks\n");
        using namespace ShoutMCO;
        ResetLog();

        CastIntentSlot slot;
        const auto taken = slot.TakeDriver(&Record, nullptr, kBehindAttack);

        const auto release = slot.Retire(SHOUTMCO_CAST_RELEASE, SHOUTMCO_CAUSE_READY);
        CHECK(release.Owed());
        release.Notify();
        CHECK(g_log.size() == 1);
        CHECK(g_log[0].handle == taken.handle);
        CHECK(g_log[0].outcome == SHOUTMCO_CAST_RELEASE);
        CHECK(g_log[0].cause == SHOUTMCO_CAUSE_READY);
        CHECK(slot.Owner() == CastIntentOwner::kNone);

        // Retiring again must deliver NOTHING. This is the case a second confirmed graph event
        // produces -- `inRdy` and `IdleStop` can ride the same update -- and a driver that got two
        // releases would cast twice off one press.
        const auto again = slot.Retire(SHOUTMCO_CAST_RELEASE, SHOUTMCO_CAUSE_READY);
        CHECK(!again.Owed());
        again.Notify();
        CHECK(g_log.size() == 1);

        // A watchdog firing after a release is the same story from the other direction.
        const auto watchdog = slot.Retire(SHOUTMCO_CAST_ABANDON, SHOUTMCO_CAUSE_WATCHDOG);
        CHECK(!watchdog.Owed());
        watchdog.Notify();
        CHECK(g_log.size() == 1);

        // Each retirement cause reaches the driver verbatim.
        ResetLog();
        (void)slot.TakeDriver(&Record, nullptr, kBehindAttack);
        const auto lost = slot.Retire(SHOUTMCO_CAST_ABANDON, SHOUTMCO_CAUSE_CONTEXT_LOST);
        lost.Notify();
        CHECK(g_log.size() == 1);
        CHECK(g_log[0].outcome == SHOUTMCO_CAST_ABANDON);
        CHECK(g_log[0].cause == SHOUTMCO_CAUSE_CONTEXT_LOST);
    }

    // ---- stale handles -------------------------------------------------------------------

    void TestStaleHandleRejection() {
        std::printf("stale handle rejection\n");
        using namespace ShoutMCO;
        ResetLog();

        CastIntentSlot slot;

        // Nothing pending at all.
        CHECK(!slot.CancelHandle(1, SHOUTMCO_CAUSE_CANCELLED).retired);
        // The invalid handle is never live, even when something IS pending.
        const auto live = slot.TakeDriver(&Record, nullptr, kBehindAttack);
        CHECK(!slot.CancelHandle(SHOUTMCO_CAST_HANDLE_INVALID, SHOUTMCO_CAUSE_CANCELLED).retired);

        // A handle from a REPLACED intent is stale: the replacement already spent its callback.
        const auto newer = slot.TakeDriver(&Record, nullptr, kBehindAttack);
        ResetLog();
        const auto staleCancel = slot.CancelHandle(live.handle, SHOUTMCO_CAUSE_CANCELLED);
        CHECK(!staleCancel.retired);
        CHECK(!staleCancel.notice.Owed());
        staleCancel.notice.Notify();
        CHECK(g_log.empty());
        CHECK(slot.CurrentHandle() == newer.handle);  // the live one is untouched

        // The live handle cancels once...
        const auto realCancel = slot.CancelHandle(newer.handle, SHOUTMCO_CAUSE_CANCELLED);
        CHECK(realCancel.retired);
        CHECK(realCancel.notice.Owed());
        realCancel.notice.Notify();
        CHECK(g_log.size() == 1);
        CHECK(g_log[0].handle == newer.handle);
        CHECK(g_log[0].outcome == SHOUTMCO_CAST_ABANDON);
        CHECK(g_log[0].cause == SHOUTMCO_CAUSE_CANCELLED);

        // ...and not twice.
        const auto secondCancel = slot.CancelHandle(newer.handle, SHOUTMCO_CAUSE_CANCELLED);
        CHECK(!secondCancel.retired);
        secondCancel.notice.Notify();
        CHECK(g_log.size() == 1);

        // Handles are never reused, so a handle retired long ago cannot come back to name a live
        // intent however many pass through the slot.
        ShoutMCO_CastHandle firstEver = live.handle;
        for (int i = 0; i < 64; ++i) {
            const auto churn = slot.TakeDriver(&Record, nullptr, kBehindAttack);
            CHECK(churn.handle != firstEver);
            (void)slot.Retire(SHOUTMCO_CAST_ABANDON, SHOUTMCO_CAUSE_CONTEXT_LOST);
        }
        CHECK(!slot.CancelHandle(firstEver, SHOUTMCO_CAUSE_CANCELLED).retired);

        // A vanilla press occupies the slot but owns no handle, so a driver handle cannot cancel
        // it out from under the player.
        ResetLog();
        (void)slot.TakeVanilla();
        CHECK(!slot.CancelHandle(newer.handle, SHOUTMCO_CAUSE_CANCELLED).retired);
        CHECK(slot.Owner() == CastIntentOwner::kVanilla);
    }


    // ---- release gating by reason --------------------------------------------------------

    // THE DEFECT A COLD REVIEW OF `704e4a9` FOUND. The first build recorded no reason, so any
    // confirmed state released any pending intent: an intent deferred behind a live shout could be
    // released early by an unrelated MCO attack's `inRdy`. The engine gates every release on
    // "is something waiting for THIS state", so the slot has to be able to answer it.
    void TestReleaseGatingByReason() {
        std::printf("release gating by confirmed state\n");
        using namespace ShoutMCO;
        ResetLog();

        CastIntentSlot slot;
        (void)slot.TakeDriver(&Record, nullptr, kBehindShout);

        CHECK(slot.DriverPendingFor(kBehindShout));
        CHECK(!slot.DriverPendingFor(kBehindAttack));  // the early-release bug, if this ever flips
        CHECK(slot.DriverPending());

        // Replacement re-tags the slot rather than keeping the old intent's reason.
        (void)slot.TakeDriver(&Record, nullptr, kBehindAttack);
        CHECK(slot.DriverPendingFor(kBehindAttack));
        CHECK(!slot.DriverPendingFor(kBehindShout));

        // A retired slot is waiting for nothing at all.
        (void)slot.Retire(SHOUTMCO_CAST_RELEASE, SHOUTMCO_CAUSE_READY);
        CHECK(!slot.DriverPendingFor(kBehindAttack));
        CHECK(!slot.DriverPendingFor(kBehindShout));

        // A vanilla occupant answers no driver reason -- the input hook's own `g_held` carries
        // the player's press and its reason.
        (void)slot.TakeVanilla();
        CHECK(!slot.DriverPendingFor(kBehindAttack));
        CHECK(!slot.DriverPendingFor(kBehindShout));
    }

    // ---- the vanilla path lets go of the slot ---------------------------------------------

    // The other cold-review finding: nothing cleared a VANILLA occupant, so once the player's
    // press ended the slot kept reporting `kVanilla` with nothing actually held.
    void TestVanillaLeavesTheSlot() {
        std::printf("vanilla occupant clears\n");
        using namespace ShoutMCO;
        ResetLog();

        CastIntentSlot slot;
        (void)slot.TakeVanilla();
        CHECK(slot.Owner() == CastIntentOwner::kVanilla);

        slot.ClearVanilla();
        CHECK(slot.Owner() == CastIntentOwner::kNone);

        // Idempotent -- several paths end one press and each one calls this.
        slot.ClearVanilla();
        CHECK(slot.Owner() == CastIntentOwner::kNone);

        // AND IT MUST NOT TOUCH A DRIVER INTENT. `Api_Request` displaces a vanilla press by
        // abandoning it, and that abandonment runs AFTER the driver intent has taken the slot --
        // so a `ClearVanilla` that cleared indiscriminately would wipe the intent it just took.
        const auto taken = slot.TakeDriver(&Record, nullptr, kBehindAttack);
        slot.ClearVanilla();
        CHECK(slot.DriverPending());
        CHECK(slot.CurrentHandle() == taken.handle);

        // The driver intent still owes exactly one callback after all that.
        const auto notice = slot.Retire(SHOUTMCO_CAST_ABANDON, SHOUTMCO_CAUSE_CANCELLED);
        CHECK(notice.Owed());
        notice.Notify();
        CHECK(g_log.size() == 1);
    }

    // ---- re-entrancy ---------------------------------------------------------------------

    // The public header says calling `Request` from inside the callback is legal and is how a
    // driver chains. That only holds because notices are spent OUTSIDE the slot's mutex. If
    // dispatch ever moves back under the lock, this test deadlocks rather than failing quietly.
    ShoutMCO::CastIntentSlot* g_reentrantSlot = nullptr;
    int                       g_reentrantDepth = 0;

    void ReentrantCallback(ShoutMCO_CastHandle a_handle, ShoutMCO_CastOutcome a_outcome,
                           std::uint32_t a_cause, void* a_context) {
        Record(a_handle, a_outcome, a_cause, a_context);
        if (g_reentrantDepth++ > 0) return;
        const auto chained = g_reentrantSlot->TakeDriver(&Record, nullptr, kBehindAttack);
        Check(chained.handle != SHOUTMCO_CAST_HANDLE_INVALID, "chained take from callback",
              __LINE__);
    }

    void TestReentrantTakeFromCallback() {
        std::printf("re-entrant take from inside a callback\n");
        using namespace ShoutMCO;
        ResetLog();

        CastIntentSlot slot;
        g_reentrantSlot = &slot;
        g_reentrantDepth = 0;

        (void)slot.TakeDriver(&ReentrantCallback, nullptr, kBehindAttack);
        const auto notice = slot.Retire(SHOUTMCO_CAST_ABANDON, SHOUTMCO_CAUSE_WATCHDOG);
        notice.Notify();  // deadlocks if dispatch ever happens under the mutex

        CHECK(g_log.size() == 1);
        CHECK(slot.DriverPending());  // the chained intent is now the live one
        g_reentrantSlot = nullptr;
    }
}

int main() {
    std::printf("ShoutMCO cast-intent slot tests\n\n");
    TestVersionNegotiation();
    TestReplacement();
    TestExactlyOnce();
    TestStaleHandleRejection();
    TestReleaseGatingByReason();
    TestVanillaLeavesTheSlot();
    TestReentrantTakeFromCallback();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
