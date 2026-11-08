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

constexpr std::array kWofExports{"WofIsExternalFile",
                                 "WofSetFileDataLocation"};
constexpr auto kWofMembers{
    std::tuple{&WofApi::fn_is_external, &WofApi::fn_set_data_location}};

constexpr std::array kNtdllExports{"NtQuerySystemInformation"};
constexpr auto kNtdllMembers{
    std::tuple{&NtdllApi::fn_query_system_information}};

constexpr std::array kRstrtmgrExports{
    "RmStartSession", "RmRegisterResources", "RmGetList", "RmShutdown",
    "RmEndSession"};
constexpr auto kRstrtmgrMembers{std::tuple{
    &RstrtmgrApi::fn_start_session, &RstrtmgrApi::fn_register_resources,
    &RstrtmgrApi::fn_get_list, &RstrtmgrApi::fn_shutdown,
    &RstrtmgrApi::fn_end_session}};

template <typename TApi, std::size_t TNameCount, typename... TMemberPtr>
std::expected<TApi, std::wstring> load_cached_api(
    ApiSlot<TApi>& slot, const wchar_t* ptr_dll_name,
    const std::array<const char*, TNameCount>& arr_names,
    const std::tuple<TMemberPtr...>& tup_members) {
  std::call_once(slot.flag_once, [&] {
    slot.xe_cached = load_dll_api<TApi>(ptr_dll_name, arr_names, tup_members);
  });
  return slot.xe_cached;
}

}

std::expected<WofApi, std::wstring> load_wof_api() {
  return load_cached_api(slot_wof, L"wofutil.dll", kWofExports, kWofMembers);
}

std::expected<NtdllApi, std::wstring> load_ntdll_api() {
  return load_cached_api(slot_ntdll, L"ntdll.dll", kNtdllExports,
                         kNtdllMembers);
}

std::expected<RstrtmgrApi, std::wstring> load_rstrtmgr_api() {
  return load_cached_api(slot_rstrtmgr, L"rstrtmgr.dll", kRstrtmgrExports,
                         kRstrtmgrMembers);
}

}
