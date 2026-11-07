#include "release.hpp"

#include "handle_guard.hpp"
#include "wof_util.hpp"

#include <windows.h>

namespace ntfscompact {

bool probe_file_occupied(const std::wstring& str_long_path) {
  HANDLE h_probe{CreateFileW(
      str_long_path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL,
      nullptr)};
  if (h_probe != INVALID_HANDLE_VALUE) {
    CloseHandle(h_probe);
    return false;
  }
  return GetLastError() == ERROR_SHARING_VIOLATION;
}

bool process_is_elevated() {
  HANDLE h_token{nullptr};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h_token)) {
    return false;
  }
  TOKEN_ELEVATION te_elevation{};
  DWORD u4_len{0};
  const BOOL b_ok{GetTokenInformation(h_token, TokenElevation, &te_elevation,
                                      sizeof(te_elevation), &u4_len)};
  CloseHandle(h_token);
  return b_ok != FALSE && te_elevation.TokenIsElevated != 0;
}

namespace {

std::wstring lowercase_name(const std::wstring& str_name) {
  std::wstring str_lower{str_name};
  for (wchar_t& ch_wide : str_lower) {
    ch_wide = static_cast<wchar_t>(std::towlower(ch_wide));
  }
  return str_lower;
}

bool in_name_table(std::wstring_view str_name,
                   std::span<const std::wstring_view> vec_table) {
  for (const std::wstring_view str_entry : vec_table) {
    if (str_name == str_entry) {
      return true;
    }
  }
  return false;
}

}

ProcessRisk classify_process_risk(const std::wstring& str_process_name) {
  const std::wstring str_lower{lowercase_name(str_process_name)};
  static constexpr std::wstring_view kLowRiskNames[]{
      L"searchindexer.exe",   L"searchprotocolhost.exe",
      L"searchfilterhost.exe", L"msmpeng.exe",
      L"nissrv.exe",          L"ntfscompact_hold.exe"};
  static constexpr std::wstring_view kHighRiskNames[]{
      L"explorer.exe",        L"winword.exe",        L"excel.exe",
      L"powerpnt.exe",        L"outlook.exe",        L"notepad.exe",
      L"notepad++.exe",       L"code.exe",           L"devenv.exe",
      L"sublime_text.exe",    L"sqlservr.exe",       L"mysqld.exe",
      L"postgres.exe",        L"mongod.exe",         L"redis-server.exe",
      L"vmware-vmx.exe",      L"vmms.exe",           L"vmplayer.exe",
      L"virtualbox.exe",      L"vboxsvc.exe",        L"vboxsdl.exe",
      L"qemu-system-x86_64.exe", L"lsass.exe",       L"winlogon.exe",
      L"wininit.exe",         L"csrss.exe",          L"smss.exe",
      L"services.exe",        L"svchost.exe",        L"dwm.exe",
      L"fontdrvhost.exe",     L"audiodg.exe",        L"conhost.exe",
      L"dllhost.exe",         L"taskhostw.exe",      L"sihost.exe",
      L"chrome.exe",          L"firefox.exe",        L"msedge.exe"};
  if (in_name_table(str_lower, kLowRiskNames)) {
    return ProcessRisk::kLow;
  }
  if (in_name_table(str_lower, kHighRiskNames)) {
    return ProcessRisk::kHigh;
  }
  return ProcessRisk::kUnknown;
}

namespace {

using NtQuerySystemInformationPtr =
    LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);

constexpr ULONG kSystemExtendedHandleInformationClass{64};
constexpr LONG kNtStatusInfoLengthMismatch{static_cast<LONG>(0xC0000004u)};
constexpr std::size_t kHandleSnapshotInitialSlots{1u << 19};
constexpr std::size_t kHandleSnapshotMaxSlots{1u << 27};

struct SystemHandleEntryEx {
  PVOID ptr_object{};
  std::uintptr_t u8_pid{0};
  std::uintptr_t u8_handle{0};
  ULONG u4_granted_access{0};
  USHORT u2_creator_back_trace_index{0};
  USHORT u2_object_type_index{0};
  ULONG u4_handle_attributes{0};
  ULONG u4_reserved{0};
};

std::expected<std::vector<SystemHandleEntryEx>, std::wstring>
snapshot_system_handles() {
  HMODULE h_ntdll{LoadLibraryW(L"ntdll.dll")};
  if (h_ntdll == nullptr) {
    return std::unexpected(std::format(
        L"无法加载 ntdll.dll（错误码 {}）：句柄枚举不可用", GetLastError()));
  }
  HmoduleGuard guard_module{h_ntdll};
  const auto fn_query{reinterpret_cast<NtQuerySystemInformationPtr>(
      GetProcAddress(h_ntdll, "NtQuerySystemInformation"))};
  if (fn_query == nullptr) {
    return std::unexpected(L"ntdll.dll 缺少 NtQuerySystemInformation 导出");
  }
  std::vector<std::uintptr_t> vec_slots(kHandleSnapshotInitialSlots, 0);
  for (;;) {
    ULONG u4_returned{0};
    const LONG i4_status{fn_query(
        kSystemExtendedHandleInformationClass, vec_slots.data(),
        static_cast<ULONG>(vec_slots.size() * sizeof(std::uintptr_t)),
        &u4_returned)};
    if (i4_status == 0) {
      break;
    }
    if (i4_status == kNtStatusInfoLengthMismatch) {
      if (vec_slots.size() >= kHandleSnapshotMaxSlots) {
        return std::unexpected(L"系统句柄表超出枚举上限，句柄枚举止步");
      }
      vec_slots.resize(vec_slots.size() * 2, 0);
      continue;
    }
    return std::unexpected(std::format(
        L"系统句柄表枚举失败（NTSTATUS 0x{:08X}）",
        static_cast<unsigned int>(i4_status)));
  }
  std::vector<SystemHandleEntryEx> vec_entries{};
  if (vec_slots.size() < 2) {
    return vec_entries;
  }
  const std::size_t u8_count{vec_slots[0]};
  const std::size_t u8_slots_per_entry{sizeof(SystemHandleEntryEx) /
                                       sizeof(std::uintptr_t)};
  const std::size_t u8_capacity{
      (vec_slots.size() - 2) / u8_slots_per_entry};
  vec_entries.reserve(std::min(u8_count, u8_capacity));
  const auto* ptr_first{
      reinterpret_cast<const SystemHandleEntryEx*>(vec_slots.data() + 2)};
  for (std::size_t u8_index{0};
       u8_index < u8_count && u8_index < u8_capacity; ++u8_index) {
    vec_entries.push_back(ptr_first[u8_index]);
  }
  return vec_entries;
}

std::wstring query_final_path(HANDLE h_file) {
  DWORD u4_len{512};
  std::wstring str_path(u4_len, L'\0');
  for (;;) {
    const DWORD u4_got{GetFinalPathNameByHandleW(
        h_file, str_path.data(), u4_len,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)};
    if (u4_got == 0) {
      return {};
    }
    if (u4_got < u4_len) {
      str_path.resize(u4_got);
      return str_path;
    }
    u4_len = u4_got;
    str_path.resize(u4_len);
  }
}

bool same_file_path(const std::wstring& str_left,
                    const std::wstring& str_right) {
  return lowercase_name(str_left) == lowercase_name(str_right);
}

std::wstring query_process_image_name(std::uint32_t u4_pid) {
  HANDLE h_proc{
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, u4_pid)};
  if (h_proc == nullptr) {
    return {};
  }
  HandlePtr up_proc{h_proc};
  DWORD u4_len{1024};
  std::wstring str_path(u4_len, L'\0');
  if (!QueryFullProcessImageNameW(h_proc, 0, str_path.data(), &u4_len)) {
    return {};
  }
  str_path.resize(u4_len);
  const std::size_t u8_slash{str_path.find_last_of(L"\\")};
  return u8_slash == std::wstring::npos ? str_path
                                        : str_path.substr(u8_slash + 1);
}

bool close_remote_file_handle(std::uint32_t u4_pid, std::uintptr_t u8_handle) {
  HANDLE h_proc{OpenProcess(PROCESS_DUP_HANDLE, FALSE, u4_pid)};
  if (h_proc == nullptr) {
    return false;
  }
  HandlePtr up_proc{h_proc};
  HANDLE h_dup{nullptr};
  const BOOL b_ok{DuplicateHandle(
      h_proc, reinterpret_cast<HANDLE>(u8_handle), GetCurrentProcess(),
      &h_dup, 0, FALSE, DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)};
  if (b_ok == FALSE) {
    return false;
  }
  if (h_dup != nullptr) {
    CloseHandle(h_dup);
  }
  return true;
}

std::vector<HandleOwner> find_target_file_handles(
    const std::wstring& str_long_path) {
  auto xe_entries{snapshot_system_handles()};
  if (!xe_entries.has_value()) {
    return {};
  }
  const std::vector<SystemHandleEntryEx>& vec_entries{xe_entries.value()};
  const std::uint32_t u4_self{GetCurrentProcessId()};
  std::map<std::uint32_t, HANDLE> map_proc{};
  std::vector<HandleOwner> vec_owners{};
  for (const SystemHandleEntryEx& he_entry : vec_entries) {
    if (he_entry.u8_pid == 0 || he_entry.u8_pid == u4_self) {
      continue;
    }
    if (he_entry.u8_handle == 0) {
      continue;
    }
    const auto u4_pid{static_cast<std::uint32_t>(he_entry.u8_pid)};
    const auto it_proc{map_proc.find(u4_pid)};
    HANDLE h_proc{nullptr};
    if (it_proc != map_proc.end()) {
      h_proc = it_proc->second;
    } else {
      h_proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, u4_pid);
      map_proc.emplace(u4_pid, h_proc);
    }
    if (h_proc == nullptr) {
      continue;
    }
    HANDLE h_dup{nullptr};
    if (DuplicateHandle(h_proc, reinterpret_cast<HANDLE>(he_entry.u8_handle),
                        GetCurrentProcess(), &h_dup, 0, FALSE,
                        DUPLICATE_SAME_ACCESS) == FALSE) {
      continue;
    }
    if (h_dup == nullptr) {
      continue;
    }
    HandlePtr up_dup{h_dup};
    if (GetFileType(h_dup) != FILE_TYPE_DISK) {
      continue;
    }
    const std::wstring str_final{query_final_path(h_dup)};
    if (str_final.empty() || !same_file_path(str_final, str_long_path)) {
      continue;
    }
    vec_owners.push_back(HandleOwner{u4_pid, he_entry.u8_handle});
  }
  for (auto& [u4_pid, h_proc] : map_proc) {
    if (h_proc != nullptr) {
      CloseHandle(h_proc);
    }
  }
  return vec_owners;
}

std::vector<std::uint32_t> ordered_owner_pids(
    const std::vector<HandleOwner>& vec_owners) {
  std::vector<std::uint32_t> vec_pids{};
  for (const HandleOwner& ho_owner : vec_owners) {
    if (std::find(vec_pids.begin(), vec_pids.end(), ho_owner.u4_pid) ==
        vec_pids.end()) {
      vec_pids.push_back(ho_owner.u4_pid);
    }
  }
  return vec_pids;
}

std::wstring join_gate_skips(const std::vector<std::wstring>& vec_names) {
  std::wstring str_joined{};
  for (std::size_t u8_index{0}; u8_index < vec_names.size(); ++u8_index) {
    if (u8_index > 0) {
      str_joined += L"、";
    }
    str_joined += vec_names[u8_index];
  }
  return str_joined;
}

}

ReleaseFn make_handle_force_level(bool b_allow_all,
                                  const HandleForceDeps& deps) {
  return [b_allow_all, deps](const std::wstring& str_long_path)
             -> ReleaseOutcome {
    const std::function<bool()> fn_is_elevated{
        deps.fn_is_elevated
            ? deps.fn_is_elevated
            : std::function<bool()>{[] { return process_is_elevated(); }}};
    const std::function<std::vector<HandleOwner>(const std::wstring&)>
        fn_find_handles{
            deps.fn_find_handles
                ? deps.fn_find_handles
                : std::function<std::vector<HandleOwner>(const std::wstring&)>{
                      [](const std::wstring& str_path) {
                        return find_target_file_handles(str_path);
                      }}};
    const std::function<std::wstring(std::uint32_t)> fn_process_name{
        deps.fn_process_name
            ? deps.fn_process_name
            : std::function<std::wstring(std::uint32_t)>{
                  [](std::uint32_t u4_pid) {
                    return query_process_image_name(u4_pid);
                  }}};
    const std::function<bool(std::uint32_t, std::uintptr_t)> fn_close_handle{
        deps.fn_close_handle
            ? deps.fn_close_handle
            : std::function<bool(std::uint32_t, std::uintptr_t)>{
                  [](std::uint32_t u4_pid, std::uintptr_t u8_handle) {
                    return close_remote_file_handle(u4_pid, u8_handle);
                  }}};

    ReleaseOutcome ro_busy{};
    ro_busy.e_status = ReleaseOutcome::Status::kBusy;
    ro_busy.e_level = ReleaseOutcome::Level::kHandle;
    if (!fn_is_elevated()) {
      ro_busy.str_detail = L"句柄强关需管理员权限，当前非提权，本级止步";
      return ro_busy;
    }
    const std::vector<HandleOwner> vec_owners{fn_find_handles(str_long_path)};
    const std::vector<std::uint32_t> vec_pids{ordered_owner_pids(vec_owners)};
    std::vector<ClosedProcess> vec_closed{};
    std::vector<std::wstring> vec_gate_skipped{};
    for (const std::uint32_t u4_pid : vec_pids) {
      if (u4_pid == GetCurrentProcessId()) {
        continue;
      }
      const std::wstring str_name{fn_process_name(u4_pid)};
      if (str_name.empty()) {
        continue;
      }
      if (!b_allow_all &&
          classify_process_risk(str_name) != ProcessRisk::kLow) {
        vec_gate_skipped.push_back(str_name);
        continue;
      }
      bool b_closed_any{false};
      for (const HandleOwner& ho_owner : vec_owners) {
        if (ho_owner.u4_pid != u4_pid) {
          continue;
        }
        if (fn_close_handle(u4_pid, ho_owner.u8_handle)) {
          b_closed_any = true;
        }
      }
      if (b_closed_any) {
        vec_closed.push_back(ClosedProcess{u4_pid, str_name});
      }
    }
    const bool b_still_occupied{probe_file_occupied(str_long_path)};
    if (!vec_closed.empty() && !b_still_occupied) {
      ReleaseOutcome ro_released{};
      ro_released.e_status = ReleaseOutcome::Status::kReleased;
      ro_released.e_level = ReleaseOutcome::Level::kHandle;
      ro_released.vec_closed = vec_closed;
      ro_released.str_detail = L"经句柄强关释放";
      return ro_released;
    }
    std::wstring str_detail{b_still_occupied
                                ? std::wstring{L"句柄强关后文件仍被占用"}
                                : std::wstring{L"句柄强关未关到目标句柄"}};
    if (!vec_gate_skipped.empty()) {
      str_detail += L"（闸门跳过：" + join_gate_skips(vec_gate_skipped) +
                    L"；可用 --force-close-all 放行）";
    }
    ro_busy.str_detail = str_detail;
    ro_busy.vec_closed = vec_closed;
    return ro_busy;
  };
}

namespace {

std::uint32_t escalation_delay_ms(const EscalationBackoff& eb_backoff,
                                  std::size_t u8_escalation,
                                  std::uint32_t u4_spent_ms) {
  if (eb_backoff.vec_delays_ms.empty()) {
    return 0;
  }
  const std::size_t u8_index{
      u8_escalation < eb_backoff.vec_delays_ms.size()
          ? u8_escalation
          : eb_backoff.vec_delays_ms.size() - 1};
  if (u4_spent_ms >= eb_backoff.u4_budget_ms) {
    return 0;
  }
  const std::uint32_t u4_remaining{eb_backoff.u4_budget_ms - u4_spent_ms};
  const std::uint32_t u4_delay{eb_backoff.vec_delays_ms[u8_index]};
  return u4_delay > u4_remaining ? u4_remaining : u4_delay;
}

ReleaseOutcome run_release_chain(const std::vector<ReleaseFn>& vec_levels,
                                 const std::wstring& str_long_path,
                                 const EscalationBackoff& eb_backoff) {
  const std::function<void(std::uint32_t)> fn_sleep_default{
      [](std::uint32_t u4_ms) { Sleep(u4_ms); }};
  const std::function<void(std::uint32_t)>& fn_sleep{
      eb_backoff.fn_sleep ? eb_backoff.fn_sleep : fn_sleep_default};
  ReleaseOutcome ro_merged{};
  ReleaseOutcome::Level e_last_level{ReleaseOutcome::Level::kNone};
  std::wstring str_last_detail{};
  std::uint32_t u4_spent{0};
  for (std::size_t u8_index{0}; u8_index < vec_levels.size(); ++u8_index) {
    if (u8_index > 0) {
      const std::uint32_t u4_delay{
          escalation_delay_ms(eb_backoff, u8_index - 1, u4_spent)};
      if (u4_delay > 0) {
        fn_sleep(u4_delay);
        u4_spent += u4_delay;
      }
      if (!probe_file_occupied(str_long_path)) {
        ReleaseOutcome ro_landed{std::move(ro_merged)};
        ro_landed.e_status = ReleaseOutcome::Status::kReleased;
        ro_landed.e_level = e_last_level;
        ro_landed.str_detail = L"经退避探测确认释放";
        return ro_landed;
      }
    }
    const ReleaseOutcome ro_step{vec_levels[u8_index](str_long_path)};
    ro_merged.vec_closed.insert(ro_merged.vec_closed.end(),
                                ro_step.vec_closed.begin(),
                                ro_step.vec_closed.end());
    if (ro_step.e_level != ReleaseOutcome::Level::kNone) {
      e_last_level = ro_step.e_level;
    }
    if (!ro_step.str_detail.empty()) {
      str_last_detail = ro_step.str_detail;
    }
    if (ro_step.e_status != ReleaseOutcome::Status::kBusy) {
      ReleaseOutcome ro_done{std::move(ro_merged)};
      ro_done.e_status = ReleaseOutcome::Status::kReleased;
      ro_done.e_level = e_last_level;
      ro_done.str_detail = ro_step.str_detail;
      return ro_done;
    }
  }
  ReleaseOutcome ro_busy{std::move(ro_merged)};
  ro_busy.e_status = ReleaseOutcome::Status::kBusy;
  ro_busy.e_level = e_last_level;
  ro_busy.str_detail = std::wstring{L"占用不可释放：升级链尽后仍被占用"};
  if (!str_last_detail.empty()) {
    ro_busy.str_detail += L"（" + str_last_detail + L"）";
  }
  return ro_busy;
}

constexpr DWORD kRmForceShutdownFlag{0x1};
constexpr DWORD kRmSessionKeyChars{32};

struct RmUniqueProcess {
  DWORD u4_pid{0};
  FILETIME ft_start{};
};

struct RmProcessInfo {
  RmUniqueProcess up_process{};
  wchar_t arr_app_name[256]{};
  wchar_t arr_service_name[64]{};
  DWORD u4_app_type{0};
  ULONG u4_app_status{0};
  DWORD u4_ts_session_id{0};
  BOOL b_restartable{FALSE};
};

using RmStartSessionPtr = DWORD(WINAPI*)(DWORD*, DWORD, wchar_t*);
using RmRegisterResourcesPtr =
    DWORD(WINAPI*)(DWORD, UINT, const wchar_t**, UINT, const RmUniqueProcess*,
                   UINT, const wchar_t**);
using RmGetListPtr = DWORD(WINAPI*)(DWORD, UINT*, UINT*, RmProcessInfo*, DWORD*);
using RmStatusCallbackPtr = DWORD(WINAPI*)(DWORD);
using RmShutdownPtr = DWORD(WINAPI*)(DWORD, ULONG, RmStatusCallbackPtr);
using RmEndSessionPtr = DWORD(WINAPI*)(DWORD);

struct RmContext {
  HmoduleGuard guard_module{};
  RmStartSessionPtr fn_start{nullptr};
  RmRegisterResourcesPtr fn_register{nullptr};
  RmGetListPtr fn_get_list{nullptr};
  RmShutdownPtr fn_shutdown{nullptr};
  RmEndSessionPtr fn_end{nullptr};
};

std::expected<RmContext, std::wstring> make_rm_context() {
  HMODULE h_module{LoadLibraryW(L"rstrtmgr.dll")};
  if (h_module == nullptr) {
    return std::unexpected(std::format(
        L"无法加载 rstrtmgr.dll（错误码 {}）：占用释放不可用，工具终止",
        GetLastError()));
  }
  RmContext ctx_rm{};
  ctx_rm.guard_module = HmoduleGuard{h_module};
  ctx_rm.fn_start =
      reinterpret_cast<RmStartSessionPtr>(GetProcAddress(h_module, "RmStartSession"));
  ctx_rm.fn_register = reinterpret_cast<RmRegisterResourcesPtr>(
      GetProcAddress(h_module, "RmRegisterResources"));
  ctx_rm.fn_get_list =
      reinterpret_cast<RmGetListPtr>(GetProcAddress(h_module, "RmGetList"));
  ctx_rm.fn_shutdown =
      reinterpret_cast<RmShutdownPtr>(GetProcAddress(h_module, "RmShutdown"));
  ctx_rm.fn_end =
      reinterpret_cast<RmEndSessionPtr>(GetProcAddress(h_module, "RmEndSession"));
  if (ctx_rm.fn_start == nullptr || ctx_rm.fn_register == nullptr ||
      ctx_rm.fn_get_list == nullptr || ctx_rm.fn_shutdown == nullptr ||
      ctx_rm.fn_end == nullptr) {
    return std::unexpected(std::format(
        L"rstrtmgr.dll 缺少 Restart Manager 导出（错误码 {}）", GetLastError()));
  }
  return ctx_rm;
}

std::vector<ClosedProcess> listed_processes(
    const std::vector<RmProcessInfo>& vec_apps, UINT u4_count) {
  std::vector<ClosedProcess> vec_closed{};
  for (UINT u4_index{0}; u4_index < u4_count && u4_index < vec_apps.size();
       ++u4_index) {
    ClosedProcess cp_item{};
    cp_item.u4_pid = vec_apps[u4_index].up_process.u4_pid;
    cp_item.str_name = vec_apps[u4_index].arr_app_name;
    if (cp_item.str_name.empty()) {
      cp_item.str_name = L"未知进程";
    }
    vec_closed.push_back(std::move(cp_item));
  }
  return vec_closed;
}

ReleaseOutcome rm_shutdown_level(const RmContext& ctx_rm, bool b_force,
                                 const std::wstring& str_long_path) {
  const wchar_t* ptr_level_name{b_force ? L"RM 强制关" : L"RM 优雅关"};
  const ReleaseOutcome::Level e_level{b_force
                                          ? ReleaseOutcome::Level::kRmForce
                                          : ReleaseOutcome::Level::kRmGraceful};
  ReleaseOutcome ro_busy{};
  ro_busy.e_status = ReleaseOutcome::Status::kBusy;
  ro_busy.e_level = e_level;
  DWORD u4_session{0};
  wchar_t arr_key[kRmSessionKeyChars + 1]{};
  DWORD u4_err{ctx_rm.fn_start(&u4_session, 0, arr_key)};
  if (u4_err != ERROR_SUCCESS) {
    ro_busy.str_detail =
        std::format(L"{}会话启动失败（错误码 {}）", ptr_level_name, u4_err);
    return ro_busy;
  }
  const wchar_t* arr_paths[1]{str_long_path.c_str()};
  u4_err = ctx_rm.fn_register(u4_session, 1, arr_paths, 0, nullptr, 0, nullptr);
  if (u4_err != ERROR_SUCCESS) {
    ctx_rm.fn_end(u4_session);
    ro_busy.str_detail =
        std::format(L"{}资源注册失败（错误码 {}）", ptr_level_name, u4_err);
    return ro_busy;
  }
  UINT u4_needed{0};
  UINT u4_count{0};
  DWORD u4_reboot_reasons{0};
  u4_err = ctx_rm.fn_get_list(u4_session, &u4_needed, &u4_count, nullptr,
                              &u4_reboot_reasons);
  std::vector<RmProcessInfo> vec_apps{};
  if (u4_err == ERROR_MORE_DATA && u4_needed > 0) {
    vec_apps.resize(u4_needed);
    std::vector<DWORD> vec_reasons(u4_needed, 0);
    u4_count = u4_needed;
    u4_err = ctx_rm.fn_get_list(u4_session, &u4_needed, &u4_count,
                                vec_apps.data(), vec_reasons.data());
  }
  if (u4_err != ERROR_SUCCESS) {
    ctx_rm.fn_end(u4_session);
    ro_busy.str_detail =
        std::format(L"{}占用进程列举失败（错误码 {}）", ptr_level_name, u4_err);
    return ro_busy;
  }
  if (u4_count == 0) {
    ctx_rm.fn_end(u4_session);
    ro_busy.str_detail = std::format(L"{}未列举到占用进程", ptr_level_name);
    return ro_busy;
  }
  u4_err = ctx_rm.fn_shutdown(
      u4_session, b_force ? kRmForceShutdownFlag : 0,
      static_cast<RmStatusCallbackPtr>(nullptr));
  const std::vector<ClosedProcess> vec_closed{listed_processes(vec_apps, u4_count)};
  ctx_rm.fn_end(u4_session);
  if (u4_err != ERROR_SUCCESS) {
    ro_busy.str_detail =
        std::format(L"{}失败（错误码 {}）", ptr_level_name, u4_err);
    return ro_busy;
  }
  if (probe_file_occupied(str_long_path)) {
    ro_busy.str_detail = std::format(L"{}未生效，文件仍被占用", ptr_level_name);
    return ro_busy;
  }
  ReleaseOutcome ro_released{};
  ro_released.e_status = ReleaseOutcome::Status::kReleased;
  ro_released.e_level = e_level;
  ro_released.vec_closed = vec_closed;
  ro_released.str_detail = std::format(L"经{}释放", ptr_level_name);
  return ro_released;
}

ReleaseFn make_rm_shutdown_level(std::shared_ptr<const RmContext> sp_ctx,
                                 bool b_force) {
  return [sp_ctx, b_force](const std::wstring& str_long_path) {
    return rm_shutdown_level(*sp_ctx, b_force, str_long_path);
  };
}

}

ReleaseFn make_oplock_level(
    std::uint32_t u4_wait_timeout_ms,
    std::function<bool(const std::wstring&)> fn_probe) {
  return [u4_wait_timeout_ms,
          fn_probe = std::move(fn_probe)](const std::wstring& str_long_path)
             -> ReleaseOutcome {
    const std::function<bool(const std::wstring&)> fn_probe_resolved{
        fn_probe ? fn_probe
                 : std::function<bool(const std::wstring&)>{
                       [](const std::wstring& str_path) {
                         return probe_file_occupied(str_path);
                       }}};
    const auto fn_released{[](const wchar_t* ptr_detail) {
      ReleaseOutcome ro_released{};
      ro_released.e_status = ReleaseOutcome::Status::kReleased;
      ro_released.e_level = ReleaseOutcome::Level::kOplock;
      ro_released.str_detail = ptr_detail;
      return ro_released;
    }};
    ReleaseOutcome ro_busy{};
    ro_busy.e_status = ReleaseOutcome::Status::kBusy;
    ro_busy.e_level = ReleaseOutcome::Level::kOplock;
    HANDLE h_file{CreateFileW(
        str_long_path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
    if (h_file == INVALID_HANDLE_VALUE) {
      ro_busy.str_detail = std::format(
          L"oplock 协商打不开目标文件（错误码 {}），立即降级", GetLastError());
      return ro_busy;
    }
    HandlePtr up_file{h_file};
    REQUEST_OPLOCK_INPUT_BUFFER roi_input{};
    roi_input.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
    roi_input.StructureLength = sizeof(REQUEST_OPLOCK_INPUT_BUFFER);
    roi_input.RequestedOplockLevel = OPLOCK_LEVEL_CACHE_READ |
                                     OPLOCK_LEVEL_CACHE_WRITE |
                                     OPLOCK_LEVEL_CACHE_HANDLE;
    roi_input.Flags = REQUEST_OPLOCK_INPUT_FLAG_REQUEST;
    REQUEST_OPLOCK_OUTPUT_BUFFER roo_output{};
    OVERLAPPED ov_wait{};
    ov_wait.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov_wait.hEvent == nullptr) {
      ro_busy.str_detail = std::format(
          L"oplock 协商创建等待事件失败（错误码 {}）", GetLastError());
      return ro_busy;
    }
    HandlePtr up_event{ov_wait.hEvent};
    const BOOL b_posted{
        DeviceIoControl(h_file, FSCTL_REQUEST_OPLOCK, &roi_input,
                        sizeof(roi_input), &roo_output, sizeof(roo_output),
                        nullptr, &ov_wait)};
    if (b_posted == FALSE && GetLastError() != ERROR_IO_PENDING) {
      const DWORD u4_request_error{GetLastError()};
      up_file.reset();
      if (!fn_probe_resolved(str_long_path)) {
        return fn_released(L"经 Oplock 协商释放");
      }
      ro_busy.str_detail = std::format(L"oplock 请求被拒（错误码 {}），降级",
                                       u4_request_error);
      return ro_busy;
    }
    const DWORD u4_wait{
        WaitForSingleObject(ov_wait.hEvent, u4_wait_timeout_ms)};
    if (u4_wait == WAIT_OBJECT_0) {
      up_file.reset();
      if (!fn_probe_resolved(str_long_path)) {
        return fn_released(L"经 Oplock 协商释放");
      }
      ro_busy.str_detail = L"oplock 锁中断完成但文件仍被占用，降级";
      return ro_busy;
    }
    if (u4_wait != WAIT_TIMEOUT) {
      const DWORD u4_wait_error{GetLastError()};
      CancelIoEx(h_file, &ov_wait);
      DWORD u4_aborted_bytes{0};
      GetOverlappedResult(h_file, &ov_wait, &u4_aborted_bytes, TRUE);
      ro_busy.str_detail = std::format(
          L"oplock 协商等待失败（错误码 {}），降级", u4_wait_error);
      return ro_busy;
    }
    CancelIoEx(h_file, &ov_wait);
    DWORD u4_aborted_bytes{0};
    GetOverlappedResult(h_file, &ov_wait, &u4_aborted_bytes, TRUE);
    up_file.reset();
    if (!fn_probe_resolved(str_long_path)) {
      return fn_released(L"经 Oplock 协商释放");
    }
    ro_busy.str_detail = std::format(L"oplock 协商等待超时（{}ms），降级",
                                     u4_wait_timeout_ms);
    return ro_busy;
  };
}

std::wstring level_display_name(ReleaseOutcome::Level e_level) {
  switch (e_level) {
    case ReleaseOutcome::Level::kOplock:
      return L"Oplock 协商";
    case ReleaseOutcome::Level::kRmGraceful:
      return L"RM 优雅关";
    case ReleaseOutcome::Level::kRmForce:
      return L"RM 强制关";
    case ReleaseOutcome::Level::kHandle:
      return L"句柄强关";
    case ReleaseOutcome::Level::kNone:
      return L"探测释放";
  }
  return L"探测释放";
}

std::expected<ReleaseFn, std::wstring> make_escalating_releaser(
    std::vector<ReleaseFn> vec_levels, const EscalationBackoff& eb_backoff) {
  return ReleaseFn{
      [vec_levels = std::move(vec_levels), eb_backoff](
          const std::wstring& str_long_path) -> ReleaseOutcome {
        if (!probe_file_occupied(str_long_path)) {
          return ReleaseOutcome{};
        }
        return run_release_chain(vec_levels, str_long_path, eb_backoff);
      }};
}

std::wstring release_audit_entry(const ReleaseOutcome& ro_outcome) {
  std::wstring str_entry{level_display_name(ro_outcome.e_level)};
  if (!ro_outcome.vec_closed.empty()) {
    str_entry += L"（";
    for (std::size_t u8_index{0}; u8_index < ro_outcome.vec_closed.size();
         ++u8_index) {
      if (u8_index > 0) {
        str_entry += L"、";
      }
      str_entry += std::format(L"{} PID {}", ro_outcome.vec_closed[u8_index].str_name,
                               ro_outcome.vec_closed[u8_index].u4_pid);
    }
    str_entry += L"）";
  }
  return str_entry;
}

std::expected<ReleaseFn, std::wstring> make_rm_shutdown_level(bool b_force) {
  auto xe_ctx{make_rm_context()};
  if (!xe_ctx.has_value()) {
    return std::unexpected(std::move(xe_ctx).error());
  }
  const auto sp_ctx{std::make_shared<const RmContext>(std::move(xe_ctx).value())};
  return make_rm_shutdown_level(sp_ctx, b_force);
}

std::expected<ReleaseFn, std::wstring> make_rm_graceful_level() {
  return make_rm_shutdown_level(false);
}

std::expected<ReleaseFn, std::wstring> make_rm_force_level() {
  return make_rm_shutdown_level(true);
}

std::expected<ReleaseFn, std::wstring> make_real_releaser(
    bool b_force_close_all) {
  auto xe_graceful{make_rm_graceful_level()};
  if (!xe_graceful.has_value()) {
    return xe_graceful;
  }
  auto xe_force{make_rm_force_level()};
  if (!xe_force.has_value()) {
    return xe_force;
  }
  return make_escalating_releaser(
      {make_oplock_level(), std::move(xe_graceful).value(),
       std::move(xe_force).value(),
       make_handle_force_level(b_force_close_all)},
      EscalationBackoff{});
}

}
