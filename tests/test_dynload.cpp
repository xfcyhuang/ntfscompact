#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dynload.hpp"

#include <windows.h>

namespace {

bool has(const std::wstring& str_haystack, const wchar_t* ptr_needle) {
  return str_haystack.find(ptr_needle) != std::wstring::npos;
}

struct FakeApi {
  DWORD(WINAPI* fn_probe)(){nullptr};
  DWORD(WINAPI* fn_pair)(DWORD, DWORD){nullptr};
};

std::wstring own_exe_path() {
  wchar_t arr_module[MAX_PATH]{};
  const DWORD u4_len{GetModuleFileNameW(nullptr, arr_module, MAX_PATH)};
  return std::wstring{arr_module, u4_len};
}

}

TEST_CASE("load_dll_api：DLL 不存在时报含 DLL 名的加载失败") {
  constexpr std::array arr_names{"FakeExportT3"};
  const auto tup_members{std::tuple{&FakeApi::fn_probe}};
  const auto xe_api{ntfscompact::load_dll_api<FakeApi>(
      L"no-such-dll-t3.dll", arr_names, tup_members)};
  REQUIRE_FALSE(xe_api.has_value());
  CHECK(has(xe_api.error(), L"no-such-dll-t3.dll"));
}

TEST_CASE("load_dll_api：导出缺失时报含 DLL 名与缺失函数名") {
  const std::wstring str_exe{own_exe_path()};
  constexpr std::array arr_names{"NoSuchExportT3"};
  const auto tup_members{std::tuple{&FakeApi::fn_probe}};
  const auto xe_api{ntfscompact::load_dll_api<FakeApi>(
      str_exe.c_str(), arr_names, tup_members)};
  REQUIRE_FALSE(xe_api.has_value());
  CHECK(has(xe_api.error(), str_exe.c_str()));
  CHECK(has(xe_api.error(), L"NoSuchExportT3"));
}

TEST_CASE("load_dll_api：导出齐备时函数表填充成功") {
  constexpr std::array arr_names{"GetCurrentProcessId"};
  const auto tup_members{std::tuple{&FakeApi::fn_probe}};
  const auto xe_api{ntfscompact::load_dll_api<FakeApi>(
      L"kernel32.dll", arr_names, tup_members)};
  REQUIRE(xe_api.has_value());
  CHECK(xe_api.value().fn_probe != nullptr);
  CHECK(xe_api.value().fn_probe() == GetCurrentProcessId());
}

TEST_CASE("load_wof_api：真实 wofutil 加载成功且两函数非空") {
  const auto xe_first{ntfscompact::load_wof_api()};
  REQUIRE(xe_first.has_value());
  CHECK(xe_first.value().fn_is_external != nullptr);
  CHECK(xe_first.value().fn_set_data_location != nullptr);
  const auto xe_second{ntfscompact::load_wof_api()};
  REQUIRE(xe_second.has_value());
  CHECK(xe_second.value().fn_is_external == xe_first.value().fn_is_external);
}

TEST_CASE("load_ntdll_api：真实 ntdll 加载成功且函数非空") {
  const auto xe_api{ntfscompact::load_ntdll_api()};
  REQUIRE(xe_api.has_value());
  CHECK(xe_api.value().fn_query_system_information != nullptr);
}

TEST_CASE("load_rstrtmgr_api：真实 rstrtmgr 加载成功且五函数非空") {
  const auto xe_api{ntfscompact::load_rstrtmgr_api()};
  REQUIRE(xe_api.has_value());
  CHECK(xe_api.value().fn_start_session != nullptr);
  CHECK(xe_api.value().fn_register_resources != nullptr);
  CHECK(xe_api.value().fn_get_list != nullptr);
  CHECK(xe_api.value().fn_shutdown != nullptr);
  CHECK(xe_api.value().fn_end_session != nullptr);
}
