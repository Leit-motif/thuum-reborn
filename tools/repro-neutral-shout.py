"""Compile the production attack bookkeeping and shout verdict with host-only IO shims.

No copied policy: the three function bodies are extracted from ShoutChainEngine.cpp
at each run. Graph readings and input ownership are supplied by each scenario. This
checks the state/decision seam, not Skyrim's input dispatch or behavior graph.

Run: python tools/repro-neutral-shout.py
Exit 1 means at least one finished attack still holds a subsequent neutral shout.
"""
from pathlib import Path
import hashlib
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build-tests" / "neutral-shout-repro"


def extract(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    # Skip comments and literals while counting braces. Match includes original text.
    token = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    depth = 0
    for match in token.finditer(source, opening):
        if match.group() == "{":
            depth += 1
        elif match.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:match.end()]
    raise ValueError(f"Unclosed function: {signature}")


def run(*args: str) -> None:
    subprocess.run(args, cwd=ROOT, check=True)


def main() -> int:
    source_bytes = (ROOT / "src/ShoutChainEngine.cpp").read_bytes()
    source = source_bytes.decode("utf-8")
    signatures = [
        "void ApplyAttackQueuedReleaseLocked(",
        "void TrackMcoAttackLocked(",
        "ShoutChainEngine::ShoutVerdict ShoutChainEngine::ShouldHoldShoutLocked(",
    ]
    functions = [extract(source, s) for s in signatures]
    functions[-1] = functions[-1].replace(
        "ShoutChainEngine::ShoutVerdict ShoutChainEngine::ShouldHoldShoutLocked(",
        "ShoutVerdict ShouldHoldShoutLocked(", 1)
    OUT.mkdir(parents=True, exist_ok=True)
    print("Production source SHA256:", hashlib.sha256(source_bytes).hexdigest(), flush=True)
    (OUT / "repro.cpp").write_text(SHIMS + "\n".join(functions) + CASES, encoding="utf-8")
    (OUT / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.21)\nproject(NeutralShoutRepro LANGUAGES CXX)\n'
        'add_executable(neutral_shout_repro repro.cpp)\n'
        'target_compile_features(neutral_shout_repro PRIVATE cxx_std_23)\n'
        f'target_include_directories(neutral_shout_repro PRIVATE "{ROOT.as_posix()}/src")\n',
        encoding="utf-8")
    run("cmake", "-S", str(OUT), "-B", str(OUT / "build"), "-A", "x64")
    run("cmake", "--build", str(OUT / "build"), "--config", "Release")
    exe = OUT / "build/Release/neutral_shout_repro.exe"
    return subprocess.run([str(exe)], cwd=ROOT).returncode


SHIMS = r'''
#include <atomic>
#include <cstdio>
#include <string_view>
#include "AttackQueuedRelease.h"
#include "SwingAdoption.h"
using namespace std::literals;
using namespace ShoutMCO;
#define SHOUTMCO_TRACE(...) ((void)0)
struct Settings { int shoutWaitCapMs = 4000; };
struct Combo { int nextPowerAttack = 1; };
struct SampledCombo { bool have = true; Combo v; unsigned frame = 1; };
struct Emit { bool releaseHeld = false; std::string_view releaseReason; };
bool held = false;
struct ShoutInputHook {
    static bool IsVanillaHoldingBehindAttackLocked() { return held; }
    static bool IsHoldingBehindAttackLocked() { return held; }
    static bool IsHoldingBehindShoutLocked() { return false; }
    static double HeldSinceMsLocked() { return 0; }
    static void AbandonSlotLocked(std::string_view) {}
};
double ElapsedMs() { return 100; }
bool g_mcoAttackLive = false, g_mcoAttackEnding = false, g_mcoSwingLanded = false;
bool g_swingAdopted = false, g_advanceWaitArmed = false, g_cancelSent = false;
bool g_mcoPowerAttack = false, g_releaseShoutChainOnReady = false;
double g_releaseShoutChainArmedAtMs = 0;
int g_powerAtInitiate = 0;
std::atomic<bool> g_comboSampleWanted{false};
std::atomic<unsigned> g_attackGeneration{0};
struct { bool shoutActive = false, windowOpen = false; } g_state;
enum class ShoutHoldReason { kNone, kBehindAttack, kBehindShout };
struct ShoutVerdict {
    bool hold = false; ShoutHoldReason reason = ShoutHoldReason::kNone;
    bool attackLive = false, swingLanded = false;
};
void SettleResetHoldLocked(std::string_view, unsigned) {}
void RecordRollingComboLocked(std::string_view, const SampledCombo&) {}
void RefreshQueuedResumeLocked(const SampledCombo&) {}
bool ArmAdvanceWaitLocked(const Settings&, const SampledCombo&, Emit&) { return false; }
void TryCancelRecoveryLocked(Emit&) {}
'''

CASES = r'''
int main() {
    int failed = 0, checks = 0;
    Settings settings; SampledCombo combo; Emit emit;
    auto reset = [&] {
        held = false; emit = {}; g_state = {};
        g_mcoAttackLive = g_mcoAttackEnding = g_mcoSwingLanded = g_swingAdopted = false;
        g_advanceWaitArmed = g_cancelSent = g_mcoPowerAttack = false;
    };
    auto event = [&](std::string_view tag, bool attacking) {
        TrackMcoAttackLocked(tag, combo, settings, emit, attacking);
    };
    auto check = [&](bool ok, const char* label) {
        ++checks; failed += !ok;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    };
    reset();
    event("MCO_AttackInitiate", true); event("HitFrame", true);
    event("attackStop", true); event("attackStop", false);
    check(!emit.releaseHeld && !ShouldHoldShoutLocked({false, 0}).hold,
          "report sequence: completed unqueued attack -> neutral shout passes through");
    reset();
    event("MCO_AttackInitiate", true); event("attackStop", false);
    check(!emit.releaseHeld && !ShouldHoldShoutLocked({false, 0}).hold,
          "minimal sequence: initiate -> idle stop -> neutral shout passes through");
    reset();
    event("MCO_PowerAttackInitiate", true); event("attackStop", false);
    check(!emit.releaseHeld && !ShouldHoldShoutLocked({false, 0}).hold,
          "completed unqueued power attack -> neutral shout passes through");
    reset();
    event("MCO_AttackInitiate", true); held = true; event("attackStop", false);
    held = false;
    check(emit.releaseHeld && !ShouldHoldShoutLocked({false, 0}).hold,
          "control: queued shout gets released and clears tracker");
    reset();
    event("MCO_AttackInitiate", true); event("inRdy", false);
    check(!ShouldHoldShoutLocked({false, 0}).hold,
          "control: graph emitting inRdy clears tracker");
    reset();
    auto before = ShouldHoldShoutLocked({true, 1});
    check(before.hold && !ShouldHoldShoutLocked({false, 0}).hold,
          "control: adopted BFCO swing self-clears at idle");
    reset();
    event("MCO_AttackInitiate", true);
    check(ShouldHoldShoutLocked({true, 1}).hold,
          "control: actual pre-hit swing still holds the shout");
    event("attackStop", true);
    check(ShouldHoldShoutLocked({true, 3}).hold,
          "control: teardown with IsAttacking=true still holds the shout");
    std::printf("%d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
'''

if __name__ == "__main__":
    sys.exit(main())
