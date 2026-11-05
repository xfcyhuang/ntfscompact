#pragma once

import std;

#include "detect.hpp"

namespace ntfscompact {

using UncompressFn =
    std::function<auto(const std::wstring& str_long_path) -> std::expected<void, std::wstring>>;

[[nodiscard]] auto uncompress_file(const std::wstring& str_long_path,
                                   IDetector& ref_detector) -> std::expected<void, std::wstring>;

[[nodiscard]] auto make_real_uncompressor() -> std::expected<UncompressFn, std::wstring>;

}

