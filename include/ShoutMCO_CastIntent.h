/* ShoutMCO cast-intent driver API -- the public contract (ADR-0008).
 *
 * WHAT THIS IS FOR. A driver (Spell Hotbar 2 and anything after it) initiates casts through its
 * own code and owns a payload ShoutMCO cannot safely copy, validate or execute. ShoutMCO owns the
 * one global pending cast-intent slot and the state-based decision to release or abandon it. This
 * header is the whole of the boundary between those two facts.
 *
 * The seam on the driver side is wherever its own attempt is refused for a reason ShoutMCO can
 * wait out. In Spell Hotbar 2 that is `MscoCastDriver::begin()`'s mid-swing false return: the
 * graph refuses the notify because the root state machine sits in `AttackState`. Instead of
 * failing, the driver hands the intent here and re-attempts it in its own callback.
 *
 * PLAIN C, ON PURPOSE. No STL types, no exceptions, no allocator ownership and no C++ ABI cross
 * this boundary in either direction, so a driver built with a different toolchain than ShoutMCO
 * still links. Every struct is POD and carries its own size plus the API version it was compiled
 * against; ShoutMCO reads `structSize` before any field beyond it.
 *
 * IT FAILS OPEN, AND THAT IS THE MOST IMPORTANT PROPERTY HERE. ShoutMCO is optional. A driver that
 * cannot find the export, or that asks for a major version this build does not implement, gets a
 * null API and MUST carry on with its own native behaviour -- refusing the cast exactly as it does
 * today. It must never hard-depend on ShoutMCO.dll, and must never popup or log-spam about its
 * absence; one read-only status line is the whole reporting surface.
 *
 * USAGE
 *
 *     typedef const ShoutMCO_CastIntentApi* (*GetApi_t)(uint32_t);
 *     HMODULE dll = GetModuleHandleA("ShoutMCO.dll");
 *     const ShoutMCO_CastIntentApi* api = NULL;
 *     if (dll) {
 *         GetApi_t get = (GetApi_t)GetProcAddress(dll, "ShoutMCO_GetCastIntentApi");
 *         if (get) api = get(SHOUTMCO_CAST_INTENT_VERSION_MAJOR);
 *     }
 *     // api == NULL is normal. Fall back to native behaviour and report status once.
 *
 * Then, at the refusal seam:
 *
 *     ShoutMCO_CastRequest req = {0};
 *     req.structSize       = sizeof(req);
 *     req.versionMajor     = SHOUTMCO_CAST_INTENT_VERSION_MAJOR;
 *     req.versionMinor     = SHOUTMCO_CAST_INTENT_VERSION_MINOR;
 *     req.callback         = &MyDriver_OnCastIntent;
 *     req.context          = self;
 *     ShoutMCO_CastHandle handle = SHOUTMCO_CAST_HANDLE_INVALID;
 *     switch (api->Request(&req, &handle)) {
 *         case SHOUTMCO_CAST_PASS_THROUGH: return DoCastNow();   // nothing to wait for
 *         case SHOUTMCO_CAST_DEFERRED:     return true;          // callback comes later
 *         default:                         return false;         // refused; behave as today
 *     }
 */
#ifndef SHOUTMCO_CAST_INTENT_H
#define SHOUTMCO_CAST_INTENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MAJOR changes when an existing field or call changes meaning, and ShoutMCO refuses a major it
 * does not implement. MINOR changes when fields are APPENDED to the end of a struct; an older
 * driver keeps working because `structSize` says where its knowledge stops. */
#define SHOUTMCO_CAST_INTENT_VERSION_MAJOR 1u
#define SHOUTMCO_CAST_INTENT_VERSION_MINOR 0u

/* The exported entry point's name, for GetProcAddress. */
#define SHOUTMCO_CAST_INTENT_EXPORT "ShoutMCO_GetCastIntentApi"

/* Opaque, monotonic, and never reused within a session. Zero is always invalid, so a zeroed
 * struct cannot accidentally name a live intent. A handle for an intent that has already been
 * released, abandoned or replaced is STALE, and every call that takes one rejects it. */
typedef uint64_t ShoutMCO_CastHandle;
#define SHOUTMCO_CAST_HANDLE_INVALID ((ShoutMCO_CastHandle)0)

/* What `Request` decided. */
typedef enum ShoutMCO_CastDecision {
    /* Nothing to wait for -- cast now, on this call, on the caller's thread. No callback follows,
     * and the handle is left invalid. */
    SHOUTMCO_CAST_PASS_THROUGH = 0,
    /* ShoutMCO owns the intent. EXACTLY ONE callback will follow, on the main thread. */
    SHOUTMCO_CAST_DEFERRED = 1,
    /* ShoutMCO will not take it: the request is malformed, or it names a major version this build
     * does not implement, or ShoutMCO is switched off. Those are the only three causes -- nothing
     * here inspects the player. No callback follows. The driver behaves exactly as it would with
     * no ShoutMCO present. */
    SHOUTMCO_CAST_REJECTED = 2
} ShoutMCO_CastDecision;

/* Why the single callback fired. */
typedef enum ShoutMCO_CastOutcome {
    /* The confirmed graph state the intent was waiting for has arrived. Re-validate the payload
     * and attempt the cast ONCE. ShoutMCO does not retry and never inspects cooldown state. */
    SHOUTMCO_CAST_RELEASE = 0,
    /* The intent will never be released. Do not cast. */
    SHOUTMCO_CAST_ABANDON = 1
} ShoutMCO_CastOutcome;

/* Diagnostic only -- the `cause` argument to the callback. A driver may log it or ignore it, but
 * must not branch cast behaviour on it: RELEASE means cast, ABANDON means do not, and that is the
 * whole contract. Values may be appended in a MINOR version, so treat unknown values as generic. */
typedef enum ShoutMCO_CastCause {
    SHOUTMCO_CAUSE_NONE = 0,
    /* RELEASE: the confirmed state the intent was waiting for arrived. Behind an MCO attack that
     * is the attack itself ending -- the character is no longer attacking -- which lands later
     * than the graph first calling itself ready, because a cut attack reports ready while it is
     * still swinging. Behind a shout it is that shout ending in any way that is not an
     * abandonment: the exhale running out on its own, and equally the graph reporting that it has
     * left the shout state. There the callback follows a short fixed delay rather than the end
     * itself, because a cast attempted in the same frame as the shout's own exit is silently
     * refused. Which confirmed state applies is ShoutMCO's decision, not the driver's. */
    SHOUTMCO_CAUSE_READY = 1,
    /* ABANDON: a newer intent -- from this driver, another driver, or the player's own shout
     * press -- took the single slot. The newest valid intent always wins. */
    SHOUTMCO_CAUSE_REPLACED = 2,
    /* ABANDON: the driver called `Cancel`. */
    SHOUTMCO_CAUSE_CANCELLED = 3,
    /* ABANDON: the context the intent was made in is gone -- a game load, or the state the intent
     * was waiting for ending in a way it can never arrive from (the shout it was deferred behind
     * being cut for an MCO chain, or ending abandoned).
     *
     * This is still not the complete set of ways an intent can lose its world: one stranded by
     * anything ShoutMCO does not observe is retired by the watchdog below instead. Treat the cause
     * as diagnostic, exactly as this enum's own header says.
     *
     * Sites that know an intent's release condition is destroyed must abandon the slot, not only
     * the player's own press: leaving a driver intent for the watchdog waits out the full cap and
     * then tells the driver the wrong reason. No value is added or renumbered, so no driver's
     * build changes; a driver that logs this string sees it in every such case, which is the
     * point. */
    SHOUTMCO_CAUSE_CONTEXT_LOST = 4,
    /* ABANDON: the bounded watchdog expired before any confirmed state arrived. ShoutMCO abandons
     * and logs; it never forces execution. This is the path that keeps an intent from waiting
     * forever behind an attack that raises no swing at all. */
    SHOUTMCO_CAUSE_WATCHDOG = 5
} ShoutMCO_CastCause;

/* `Status` -- what a driver surfaces read-only, without popups or repeated log spam. */
typedef enum ShoutMCO_CastStatus {
    SHOUTMCO_STATUS_UNAVAILABLE = 0, /* no ShoutMCO, or the export is missing */
    SHOUTMCO_STATUS_ACTIVE = 1,      /* negotiated and taking intents */
    SHOUTMCO_STATUS_INCOMPATIBLE = 2 /* present, but no shared major version */
} ShoutMCO_CastStatus;

/* THE SINGLE CALLBACK. Called EXACTLY ONCE per deferred intent, ALWAYS ON THE MAIN THREAD, and
 * never re-entrantly from inside `Request` itself.
 *
 * Keep it short and non-blocking -- it runs inside a task drain, so time spent here is time the
 * frame does not advance. Re-validate the payload before acting on a RELEASE: the intent may have
 * waited hundreds of milliseconds, and the driver, not ShoutMCO, owns what is still true.
 *
 * Calling `Request` again from inside the callback is legal and is the normal way to chain. */
typedef void (*ShoutMCO_CastCallback)(ShoutMCO_CastHandle a_handle,
                                      ShoutMCO_CastOutcome a_outcome,
                                      uint32_t a_cause,
                                      void* a_context);

/* The request. Zero-initialise it, then set every field: ShoutMCO rejects a request whose
 * `structSize` is smaller than the v1 struct or whose `callback` is null. */
typedef struct ShoutMCO_CastRequest {
    uint32_t structSize;   /* sizeof(ShoutMCO_CastRequest) as the DRIVER compiled it */
    uint32_t versionMajor; /* SHOUTMCO_CAST_INTENT_VERSION_MAJOR the driver compiled against */
    uint32_t versionMinor;
    uint32_t flags;                 /* reserved; must be 0 in v1 */
    ShoutMCO_CastCallback callback; /* required */
    void* context;                  /* opaque; handed back verbatim */
} ShoutMCO_CastRequest;

/* The negotiated API. ShoutMCO owns this object for the life of the process -- do not free it,
 * and do not copy it and expect the copy to stay valid across a version renegotiation. */
typedef struct ShoutMCO_CastIntentApi {
    uint32_t structSize; /* sizeof(ShoutMCO_CastIntentApi) as SHOUTMCO compiled it */
    uint32_t versionMajor;
    uint32_t versionMinor;
    uint32_t reserved;

    /* Offer an intent. Synchronous and safe from any thread -- but not free, and not constant-time.
     * What it costs:
     *
     *   - It takes TWO of ShoutMCO's own locks, the second inside the first: the engine lock, and
     *     the pending-slot lock. Neither is a lock the game holds, so this cannot deadlock against
     *     the game's own work, but a concurrent caller waits for both. Reading the current
     *     settings, just before them, is a synchronized load of its own.
     *   - It allocates when it queues a callback: the displaced owner's notice is copied into a
     *     task handed to the game's main-thread queue.
     *   - WITH TRACING SWITCHED ON IT WRITES TO DISK, and more than once in the replacement case.
     *     ShoutMCO's log flushes every line as it is written, so a traced call synchronously
     *     writes and flushes and can block on that I/O. With tracing off it writes nothing here,
     *     except that a missing main-thread queue is reported as an error whatever the setting.
     *   - It starts no thread.
     *
     * On SHOUTMCO_CAST_DEFERRED, `*a_outHandle` receives the live handle. On any other decision it
     * receives SHOUTMCO_CAST_HANDLE_INVALID. `a_outHandle` may be NULL if the caller does not
     * intend to cancel.
     *
     * A deferred request REPLACES whatever intent was pending, whoever owned it.
     *
     * GUARANTEED on return: the displaced owner is retired, so its handle is stale and `Cancel` on
     * it returns zero; and it will receive exactly one ABANDON / REPLACED callback.
     *
     * NOT GUARANTEED, AND DO NOT ORDER AGAINST IT EITHER WAY: whether that callback has run. It is
     * never invoked inline on your thread -- it is queued for the main thread -- so if you call
     * from the main thread it cannot run before this returns, and if you call from any other
     * thread the main thread may drain it at any moment, including before this returns. Write the
     * callback and the code after this call so that either order is correct. */
    ShoutMCO_CastDecision (*Request)(const ShoutMCO_CastRequest* a_request,
                                     ShoutMCO_CastHandle* a_outHandle);

    /* Withdraw a deferred intent. Returns non-zero if this call is what retired it; zero if the
     * handle was already stale, which is not an error and needs no reporting. The callback fires
     * with ABANDON / CANCELLED exactly once, as for any other retirement. Safe from any thread. */
    int (*Cancel)(ShoutMCO_CastHandle a_handle);

    /* What the driver should display. Never blocks. */
    ShoutMCO_CastStatus (*Status)(void);
} ShoutMCO_CastIntentApi;

/* THE ONLY EXPORT. Returns NULL when `a_requestedMajor` is not this build's major version, which
 * is the incompatible case and is not an error -- the driver falls back to native behaviour. The
 * returned pointer is valid for the life of the process. */
typedef const ShoutMCO_CastIntentApi* (*ShoutMCO_GetCastIntentApi_t)(uint32_t a_requestedMajor);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SHOUTMCO_CAST_INTENT_H */
