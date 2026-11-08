#pragma once

import std;

#include <windows.h>

namespace ntfscompact {

struct WofApi {
  ULONG(WINAPI* fn_is_external)(LPCWSTR, PBOOL, PULONG, PVOID, PULONG){
      nullptr};
  ULONG(WINAPI* fn_set_data_location)(HANDLE, ULONG, PVOID, ULONG){nullptr};
};

struct NtdllApi {
  LONG(WINAPI* fn_query_system_information)(ULONG, PVOID, ULONG, PULONG){
      nullptr};
};

struct RmUniqueProcess {
  DWORD u4_pid{0};
  FILETIME ft_start{};
};

constexpr DWORD kRmAppNameChars{256};
constexpr DWORD kRmServiceNameChars{64};

struct RmProcessInfo {
  RmUniqueProcess up_process{};
  wchar_t arr_app_name[kRmAppNameChars]{};
  wchar_t arr_service_name[kRmServiceNameChars]{};
  DWORD u4_app_type{0};
  ULONG u4_app_status{0};
  DWORD u4_ts_session_id{0};
  BOOL b_restartable{FALSE};
};

struct RstrtmgrApi {
  DWORD(WINAPI* fn_start_session)(DWORD*, DWORD, wchar_t*){nullptr};
  DWORD(WINAPI* fn_register_resources)(
      DWORD, UINT, const wchar_t**, UINT, const RmUniqueProcess*, UINT,
      const wchar_t**){nullptr};
  DWORD(WINAPI* fn_get_list)(DWORD, UINT*, UINT*, RmProcessInfo*,
                             DWORD*){nullptr};
  DWORD(WINAPI* fn_shutdown)(DWORD, ULONG, DWORD(WINAPI*)(DWORD)){nullptr};
  DWORD(WINAPI* fn_end_session)(DWORD){nullptr};
};

inline std::wstring widen_export_name(std::string_view str_name) {
  std::wstring str_wide{};
  str_wide.reserve(str_name.size());
  for (const char ch_name : str_name) {
    str_wide.push_back(static_cast<wchar_t>(ch_name));
  }
  return str_wide;
}

template <typename TMemberPtr>
struct ExportBinding {
  const char* ptr_export;
  TMemberPtr ptr_member;
};

template <typename TApi, typename TBinding>
bool bind_export(TApi& api, HMODULE h_module, const wchar_t* ptr_dll_name,
                 const TBinding& binding, std::wstring& str_error) {
  FARPROC ptr_proc{GetProcAddress(h_module, binding.ptr_export)};
  if (ptr_proc == nullptr) {
    str_error = std::format(L"{} 缺少导出 {}", ptr_dll_name,
                            widen_export_name(binding.ptr_export));
    return false;
  }
  api.*binding.ptr_member = reinterpret_cast<
      std::remove_reference_t<decltype(api.*binding.ptr_member)>>(ptr_proc);
  return true;
}

template <typename TApi, typename... TBindings>
std::expected<TApi, std::wstring> load_dll_api(
    const wchar_t* ptr_dll_name,
    const std::tuple<TBindings...>& tup_bindings) {
  static_assert(
      (std::is_same_v<TBindings,
                      ExportBinding<decltype(TBindings::ptr_member)>> && ...));
  static_assert(
      (std::is_member_pointer_v<decltype(TBindings::ptr_member)> && ...));
  HMODULE h_module{LoadLibraryW(ptr_dll_name)};
  if (h_module == nullptr) {
    return std::unexpected(
        std::format(L"无法加载 {}（错误码 {}）", ptr_dll_name, GetLastError()));
  }
  TApi api{};
  std::wstring str_error{};
  const bool b_bound{[&]<std::size_t... TIs>(std::index_sequence<TIs...>) {
    return (bind_export(api, h_module, ptr_dll_name,
                        std::get<TIs>(tup_bindings), str_error) &&
            ...);
  }(std::index_sequence_for<TBindings...>{})};
  if (!b_bound) {
    return std::unexpected(std::move(str_error));
  }
  return api;
}

std::expected<WofApi, std::wstring> load_wof_api();

std::expected<NtdllApi, std::wstring> load_ntdll_api();

std::expected<RstrtmgrApi, std::wstring> load_rstrtmgr_api();

}
