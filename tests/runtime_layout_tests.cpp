#include "RuntimeHooks.h"
#include <array>
#include <iostream>

// Calls the linked dependency without Skyrim. Static compatibility evidence only.
int main(int argc, char** argv) {
    for (const auto version : {REL::Version{1, 5, 97, 0}, REL::Version{1, 6, 1170, 0},
                               REL::Version{1, 7, 99, 0}, REL::Version{1, 7, 104, 0}}) {
        if (!REL::Module::mock(version)) return 1;
        const bool modern = version >= SKSE::RUNTIME_SSE_1_7_99;
        if (REL::Module::IsAE() != (version[1] >= 6) ||
            ShoutMCO::InputHandlerSlot(4) != (modern ? 6 : 4) ||
            ShoutMCO::InputHandlerSlot(5) != (modern ? 7 : 5)) return 2;
        alignas(16) std::array<std::byte, 0x300> graphics{};
        auto* state = reinterpret_cast<RE::BSGraphics::State*>(graphics.data());
        if (reinterpret_cast<std::byte*>(&state->GetFrameCount()) - graphics.data() !=
            (modern ? 0x54 : 0x4C)) return 3;
        alignas(16) std::array<std::byte, 0x500> actor{};
        auto* playerActor = reinterpret_cast<RE::Actor*>(actor.data());
        if (reinterpret_cast<std::byte*>(playerActor->AsActorState()) - actor.data() !=
            (version >= SKSE::RUNTIME_SSE_1_6_629 ? 0xC0 : 0xB8)) return 4;
        if (reinterpret_cast<std::byte*>(&playerActor->GetActorRuntimeData()) - actor.data() !=
            (version >= SKSE::RUNTIME_SSE_1_6_629 ? 0xE8 : 0xE0)) return 5;
        std::cout << "PASS runtime/layout " << version.string() << '\n';
    }
    if (argc != 2) return 6;
    for (const auto version : {REL::Version{1, 5, 97, 0}, REL::Version{1, 6, 1170, 0},
                               REL::Version{1, 7, 104, 0}}) {
        if (!REL::Module::mock(version)) return 7;
        const auto name = std::format("{}-{}.bin", REL::Module::IsAE() ? "versionlib" : "version",
                                       version.string());
        const auto path = std::filesystem::path(argv[1]) / name;
        if (!REL::IDDB::inject(path.wstring(), version)) return 8;
        for (const auto& id : {RE::VTABLE_AttackBlockHandler[0], RE::VTABLE_ShoutHandler[0],
                              RE::VTABLE_PlayerCharacter[2], RE::VTABLE_PlayerCharacter[3]}) {
            if (!id.offset()) return 9;
        }
        std::cout << "PASS database " << name << '\n';
    }
}
