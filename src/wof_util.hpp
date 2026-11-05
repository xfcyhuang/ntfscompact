#pragma once

import std;

#include <windows.h>

#include "cli.hpp"

namespace ntfscompact {

struct WofFileCompressionInfoV1 {
  ULONG u4_algorithm{};
  ULONG u4_flags{};
};

using WofIsExternalFilePtr =
    ULONG(WINAPI*)(LPCWSTR, PBOOL, PULONG, PVOID, PULONG);
using WofSetFileDataLocationPtr = ULONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG);

struct HmoduleReleaser {
  void operator()(HMODULE h_module) const {
    if (h_module != nullptr) {
      FreeLibrary(h_module);
    }
  }
};

using HmoduleGuard =
    std::unique_ptr<std::remove_pointer_t<HMODULE>, HmoduleReleaser>;

constexpr ULONG kWofAlgoXpress4k{0};
constexpr ULONG kWofAlgoLzx{1};
constexpr ULONG kWofAlgoXpress8k{2};
constexpr ULONG kWofAlgoXpress16k{3};

Algorithm from_wof_algorithm(ULONG u4_algorithm);

ULONG to_wof_algorithm(Algorithm e_algorithm);

std::expected<HmoduleGuard, DWORD> load_wofutil_module();

}
