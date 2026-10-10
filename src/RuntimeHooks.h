#pragma once

#include "PCH.h"
#include <Windows.h>

namespace ShoutMCO {
    // 1.7.99 inserted motion-gesture and sixaxis handlers before ProcessButton.
    // Both installation and CommonLib's virtual-call wrappers must select the same slot.
    inline std::size_t InputHandlerSlot(std::size_t a_legacySlot) {
        return REL::VersionShift(a_legacySlot, RE::PlayerInputHandler::kAE1799AddedVFuncCount,
                                 SKSE::RUNTIME_SSE_1_7_99);
    }

    template <class F>
    std::uintptr_t WriteVerifiedVFunc(REL::Relocation<std::uintptr_t>& a_vtable,
                                      std::size_t a_slot, F a_hook, std::string_view a_name) {
        const auto slotAddress = a_vtable.address() + a_slot * sizeof(std::uintptr_t);
        MEMORY_BASIC_INFORMATION slotInfo{};
        if (!VirtualQuery(reinterpret_cast<void*>(slotAddress), &slotInfo, sizeof(slotInfo)) ||
            slotInfo.State != MEM_COMMIT || (slotInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            SKSE::stl::report_and_fail("ShoutMCO hook vtable is not readable");
        }
        const auto original = *reinterpret_cast<const std::uintptr_t*>(slotAddress);
        MEMORY_BASIC_INFORMATION codeInfo{};
        constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!VirtualQuery(reinterpret_cast<void*>(original), &codeInfo, sizeof(codeInfo)) ||
            codeInfo.State != MEM_COMMIT || !(codeInfo.Protect & executable) ||
            (codeInfo.Protect & PAGE_GUARD)) {
            SKSE::stl::report_and_fail("ShoutMCO hook target is not executable");
        }
        // An earlier plugin may own the callable original. Preserve that chain and log its
        // actual module instead of requiring every target to still point into SkyrimSE.exe.
        HMODULE module{};
        wchar_t path[MAX_PATH]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(original), &module)) {
            GetModuleFileNameW(module, path, MAX_PATH);
        }
        SKSE::log::info("[ShoutMCO] hook {}: runtime={} vtableRVA={:X} slot={:X} "
                        "originalRVA={:X} module={}", a_name, REL::Module::get().version().string(),
                        a_vtable.address() - REL::Module::get().base(), a_slot,
                        original - reinterpret_cast<std::uintptr_t>(module),
                        std::filesystem::path(path).filename().string());
        return a_vtable.write_vfunc(a_slot, a_hook);
    }
}
