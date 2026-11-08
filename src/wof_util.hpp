#pragma once

import std;

#include <windows.h>

#include "cli.hpp"

namespace ntfscompact {

struct WofFileCompressionInfoV1 {
  ULONG u4_algorithm{};
  ULONG u4_flags{};
};

constexpr ULONG kWofAlgoXpress4k{0};
constexpr ULONG kWofAlgoLzx{1};
constexpr ULONG kWofAlgoXpress8k{2};
constexpr ULONG kWofAlgoXpress16k{3};

Algorithm from_wof_algorithm(ULONG u4_algorithm);

ULONG to_wof_algorithm(Algorithm e_algorithm);

}
