#include "release.hpp"

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

std::expected<ReleaseFn, std::wstring> make_real_releaser() {
  auto xe_graceful{make_rm_graceful_level()};
  if (!xe_graceful.has_value()) {
    return xe_graceful;
  }
  auto xe_force{make_rm_force_level()};
  if (!xe_force.has_value()) {
    return xe_force;
  }
  return make_escalating_releaser(
      {std::move(xe_graceful).value(), std::move(xe_force).value()},
      EscalationBackoff{});
}

}
