#include "dynload.hpp"

namespace ntfscompact {
namespace {

template <typename TApi>
struct ApiSlot {
  std::once_flag flag_once{};
  std::expected<TApi, std::wstring> xe_cached{std::unexpected(std::wstring{})};
};

ApiSlot<WofApi> slot_wof{};
ApiSlot<NtdllApi> slot_ntdll{};
ApiSlot<RstrtmgrApi> slot_rstrtmgr{};

constexpr auto kWofBindings{std::tuple{
    ExportBinding{"WofIsExternalFile", &WofApi::fn_is_external},
    ExportBinding{"WofSetFileDataLocation",
                  &WofApi::fn_set_data_location}}};

constexpr auto kNtdllBindings{std::tuple{
    ExportBinding{"NtQuerySystemInformation",
                  &NtdllApi::fn_query_system_information}}};

constexpr auto kRstrtmgrBindings{std::tuple{
    ExportBinding{"RmStartSession", &RstrtmgrApi::fn_start_session},
    ExportBinding{"RmRegisterResources", &RstrtmgrApi::fn_register_resources},
    ExportBinding{"RmGetList", &RstrtmgrApi::fn_get_list},
    ExportBinding{"RmShutdown", &RstrtmgrApi::fn_shutdown},
    ExportBinding{"RmEndSession", &RstrtmgrApi::fn_end_session}}};

template <typename TApi, typename... TBindings>
std::expected<TApi, std::wstring> load_cached_api(
    ApiSlot<TApi>& slot, const wchar_t* ptr_dll_name,
    const std::tuple<TBindings...>& tup_bindings) {
  std::call_once(slot.flag_once, [&] {
    slot.xe_cached = load_dll_api<TApi>(ptr_dll_name, tup_bindings);
  });
  return slot.xe_cached;
}

}

std::expected<WofApi, std::wstring> load_wof_api() {
  return load_cached_api(slot_wof, L"wofutil.dll", kWofBindings);
}

std::expected<NtdllApi, std::wstring> load_ntdll_api() {
  return load_cached_api(slot_ntdll, L"ntdll.dll", kNtdllBindings);
}

std::expected<RstrtmgrApi, std::wstring> load_rstrtmgr_api() {
  return load_cached_api(slot_rstrtmgr, L"rstrtmgr.dll", kRstrtmgrBindings);
}

}
