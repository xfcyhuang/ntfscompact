#include "wof_util.hpp"

namespace ntfscompact {

Algorithm from_wof_algorithm(ULONG u4_algorithm) {
  switch (u4_algorithm) {
    case kWofAlgoXpress4k:
      return Algorithm::kXpress4k;
    case kWofAlgoLzx:
      return Algorithm::kLzx;
    case kWofAlgoXpress8k:
      return Algorithm::kXpress8k;
    case kWofAlgoXpress16k:
      return Algorithm::kXpress16k;
  }
  return Algorithm::kXpress8k;
}

ULONG to_wof_algorithm(Algorithm e_algorithm) {
  switch (e_algorithm) {
    case Algorithm::kXpress4k:
      return kWofAlgoXpress4k;
    case Algorithm::kLzx:
      return kWofAlgoLzx;
    case Algorithm::kXpress8k:
      return kWofAlgoXpress8k;
    case Algorithm::kXpress16k:
      return kWofAlgoXpress16k;
  }
  return kWofAlgoXpress8k;
}

std::expected<HmoduleGuard, DWORD> load_wofutil_module() {
  HMODULE h_module{LoadLibraryW(L"wofutil.dll")};
  if (h_module == nullptr) {
    return std::unexpected(GetLastError());
  }
  return HmoduleGuard{h_module};
}

}
