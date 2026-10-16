// Test-only SE 1.5.97 probe: feeds a synthetic completed MCO attack through the
// player's actual hooked animation-event sink. This is not a gameplay reproduction.
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <atomic>
#include <spdlog/sinks/basic_file_sink.h>

namespace {
    std::atomic<std::int32_t> g_probeResult{0};

    void SeedCompletedAttack(RE::StaticFunctionTag*, bool power) {
        g_probeResult.store(0);
        SKSE::GetTaskInterface()->AddTask([power] {
            auto* player = RE::PlayerCharacter::GetSingleton();
            bool attacking = true;
            if (!player || !player->Is3DLoaded() ||
                !player->GetGraphVariableBool("IsAttacking", attacking) || attacking ||
                player->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone) {
                g_probeResult.store(-1);
                SKSE::log::info("seed refused: player must be loaded and idle");
                return;
            }
            auto* sink = static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(player);
            const RE::BSAnimationGraphEvent initiate{
                power ? "MCO_PowerAttackInitiate" : "MCO_AttackInitiate", player, ""};
            const RE::BSAnimationGraphEvent stop{"attackStop", player, ""};
            SKSE::log::info("seed begin: power={}, IsAttacking=false, attackState=0", power);
            sink->ProcessEvent(&initiate, nullptr);
            sink->ProcessEvent(&stop, nullptr);
            SKSE::log::info("seed completed through player animation sink");
            g_probeResult.store(1);
        });
    }

    std::int32_t GetResult(RE::StaticFunctionTag*) { return g_probeResult.load(); }

    bool Register(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("SeedCompletedAttack", "ShoutNeutralProbe", SeedCompletedAttack);
        vm->RegisterFunction("GetResult", "ShoutNeutralProbe", GetResult);
        return true;
    }
}

SKSEPluginInfo(.Version = {1, 0, 0, 0}, .Name = "ShoutNeutralProbe",
               .Author = "Thu'um QA");

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    // The test uses the SE base-class layout. Refuse every other runtime explicitly.
    if (skse->IsEditor() || skse->RuntimeVersion() != SKSE::RUNTIME_SSE_1_5_97) return false;
    auto path = SKSE::log::log_directory();
    if (!path) return false;
    *path /= "ShoutNeutralProbe.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto logger = std::make_shared<spdlog::logger>("neutral-qa", std::move(sink));
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
    SKSE::Init(skse, false);
    SKSE::log::info("test-only neutral shout probe loaded on SE 1.5.97");
    return SKSE::GetPapyrusInterface()->Register(Register);
}
