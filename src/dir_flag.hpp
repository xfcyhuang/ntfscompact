#pragma once

import std;

namespace ntfscompact {

using DirFlagFn =
    std::function<auto(const std::wstring& str_long_path) -> std::expected<void, std::wstring>>;

[[nodiscard]] auto clear_dir_flag(const std::wstring& str_long_path)
    -> std::expected<void, std::wstring>;

[[nodiscard]] auto make_real_dir_flag_clearer() -> std::expected<DirFlagFn, std::wstring>;

}
