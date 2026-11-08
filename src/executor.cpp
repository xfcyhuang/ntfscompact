#include "executor.hpp"

#include "attr_guard.hpp"
#include "dynload.hpp"
#include "handle_guard.hpp"
#include "wof_util.hpp"

#include <windows.h>

#ifndef FSCTL_SET_COMPRESSION
#define FSCTL_SET_COMPRESSION \
  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 16, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef FSCTL_DELETE_EXTERNAL_BACKING
#define FSCTL_DELETE_EXTERNAL_BACKING \
  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 197, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif

namespace ntfscompact {
namespace {

constexpr ULONG kHresultFacilityWin32{0x80070000U};
constexpr int kTransientRetryMax{10};
constexpr DWORD kTransientRetryDelayMs{50};

struct ExecutorContext {
  std::function<ULONG WINAPI(HANDLE, ULONG, PVOID, ULONG)>
      fn_set_data_location{};
  std::function<ULONG WINAPI(LPCWSTR, PBOOL, PULONG, PVOID, PULONG)>
      fn_is_external{};
};

struct IoctlOutcome {
  bool b_ok{false};
  DWORD u4_error{0};
};

ULONG unpack_win32_error(ULONG u4_error) {
  if ((u4_error & 0xFFFF0000U) == kHresultFacilityWin32) {
    return u4_error & 0xFFFFU;
  }
  return u4_error;
}

bool is_transient_win32_error(ULONG u4_error) {
  switch (unpack_win32_error(u4_error)) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_OPLOCK_NOT_GRANTED:
    case ERROR_OPLOCK_BREAK_IN_PROGRESS:
      return true;
  }
  return false;
}

ULONG retry_on_transient(const std::function<ULONG()>& fn_step) {
  for (int i4_attempt{0};; ++i4_attempt) {
    const ULONG u4_status{fn_step()};
    if (i4_attempt >= kTransientRetryMax ||
        !is_transient_win32_error(u4_status)) {
      return u4_status;
    }
    Sleep(kTransientRetryDelayMs);
  }
}

ExecuteResult make_error(std::wstring str_message, ULONG u4_error) {
  ExecuteResult er_result{};
  er_result.e_outcome = ExecuteResult::Outcome::kError;
  er_result.str_message = std::move(str_message);
  er_result.u4_error = static_cast<unsigned int>(u4_error);
  return er_result;
}

ExecuteResult make_skipped(std::wstring str_message) {
  ExecuteResult er_result{};
  er_result.e_outcome = ExecuteResult::Outcome::kSkipped;
  er_result.str_message = std::move(str_message);
  return er_result;
}

HandlePtr open_file_for_compress(const std::wstring& str_long_path) {
  HANDLE h_open{INVALID_HANDLE_VALUE};
  retry_on_transient([&] {
    h_open = CreateFileW(
        str_long_path.c_str(),
        GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL,
        nullptr);
    return h_open != INVALID_HANDLE_VALUE ? 0 : GetLastError();
  });
  if (h_open == INVALID_HANDLE_VALUE) {
    return HandlePtr{};
  }
  return HandlePtr{h_open};
}

IoctlOutcome ioctl_with_retry(HANDLE h_file, DWORD u4_control, void* ptr_in,
                              DWORD u4_in_len) {
  const ULONG u4_status{retry_on_transient([&] {
    DWORD u4_returned{};
    return DeviceIoControl(h_file, u4_control, ptr_in, u4_in_len, nullptr, 0,
                           &u4_returned, nullptr) != 0
               ? 0
               : GetLastError();
  })};
  return IoctlOutcome{u4_status == 0, u4_status};
}

ExecuteResult compress_core(const ExecutorContext& ctx_execute,
                            const std::wstring& str_long_path,
                            Plan::Action e_action,
                            const CurrentState& cs_current, Algorithm e_target) {
  HandlePtr guard_file{open_file_for_compress(str_long_path)};
  if (!guard_file) {
    return make_error(
        std::format(L"无法打开文件（错误码 {}）", GetLastError()),
        GetLastError());
  }
  if (e_action == Plan::Action::kRecompress) {
    if (cs_current.e_kind == CurrentState::Kind::kLznt1) {
      USHORT u2_format_none{0};
      const IoctlOutcome io_unback{ioctl_with_retry(
          guard_file.get(), FSCTL_SET_COMPRESSION, &u2_format_none,
          sizeof(u2_format_none))};
      if (!io_unback.b_ok) {
        return make_error(std::format(
            L"解除 LZNT1 压缩失败（FSCTL_SET_COMPRESSION 错误码 {}）",
            io_unback.u4_error),
            io_unback.u4_error);
      }
    } else if (cs_current.e_kind == CurrentState::Kind::kWof) {
      const IoctlOutcome io_unback{ioctl_with_retry(
          guard_file.get(), FSCTL_DELETE_EXTERNAL_BACKING, nullptr, 0)};
      if (!io_unback.b_ok &&
          io_unback.u4_error != ERROR_OBJECT_NOT_EXTERNALLY_BACKED) {
        return make_error(std::format(
            L"解除 WOF 压缩失败（FSCTL_DELETE_EXTERNAL_BACKING 错误码 {}）",
            io_unback.u4_error),
            io_unback.u4_error);
      }
    }
  }
  WofFileCompressionInfoV1 info_request{};
  info_request.u4_algorithm = to_wof_algorithm(e_target);
  const ULONG u4_compress{retry_on_transient([&] {
    return ctx_execute.fn_set_data_location(
        guard_file.get(), static_cast<ULONG>(CurrentState::kProviderFile),
        &info_request, sizeof(info_request));
  })};
  if (unpack_win32_error(u4_compress) ==
      static_cast<ULONG>(ERROR_COMPRESSION_NOT_BENEFICIAL)) {
    return make_skipped(L"无收益（内核判定压缩无容量收益），文件保持原样");
  }
  if (unpack_win32_error(u4_compress) != ERROR_SUCCESS) {
    return make_error(std::format(L"压缩失败（WofSetFileDataLocation 错误码 {}）",
                                  u4_compress),
                      unpack_win32_error(u4_compress));
  }
  BOOL b_external{FALSE};
  ULONG u4_provider{};
  WofFileCompressionInfoV1 info_verified{};
  ULONG u4_info_len{sizeof(info_verified)};
  const ULONG u4_verify{ctx_execute.fn_is_external(
      str_long_path.c_str(), &b_external, &u4_provider, &info_verified,
      &u4_info_len)};
  if (u4_verify != ERROR_SUCCESS) {
    return make_error(
        std::format(L"复核失败（WofIsExternalFile 错误码 {}）", u4_verify),
        u4_verify);
  }
  CurrentState cs_verified{};
  cs_verified.e_kind = b_external != FALSE ? CurrentState::Kind::kWof
                                           : CurrentState::Kind::kNone;
  cs_verified.i4_provider = static_cast<int>(u4_provider);
  cs_verified.e_algorithm = from_wof_algorithm(info_verified.u4_algorithm);
  const std::wstring str_mismatch{
      check_compression_meets_target(cs_verified, e_target)};
  if (!str_mismatch.empty()) {
    return make_error(str_mismatch, 0);
  }
  return ExecuteResult{};
}

ExecuteResult execute_one(const ExecutorContext& ctx_execute,
                          const std::wstring& str_long_path,
                          Plan::Action e_action,
                          const CurrentState& cs_current, Algorithm e_target) {
  if (e_action != Plan::Action::kCompress &&
      e_action != Plan::Action::kRecompress) {
    return make_error(L"执行器仅受理压缩/重压动作（Skip/Uncompress 不经压缩执行器）", 0);
  }
  auto xe_attr{begin_readonly_guard(str_long_path)};
  if (!xe_attr.has_value()) {
    const AttrFailure af_failure{std::move(xe_attr).error()};
    return make_error(attr_failure_message(af_failure.e_stage,
                                           af_failure.u4_error),
                      af_failure.u4_error);
  }
  const ReadonlyAttrState ra_attr{std::move(xe_attr).value()};
  ExecuteResult er_result{compress_core(ctx_execute, str_long_path, e_action,
                                        cs_current, e_target)};
  AttrFailure af_restore{};
  if (!end_readonly_guard(ra_attr, str_long_path, &af_restore)) {
    er_result.e_outcome = ExecuteResult::Outcome::kError;
    er_result.str_message +=
        L"；" + attr_failure_message(af_restore.e_stage, af_restore.u4_error);
  }
  return er_result;
}

}

std::expected<ExecuteFn, std::wstring> make_real_executor(
    std::expected<WofApi, std::wstring> xe_api) {
  if (!xe_api.has_value()) {
    return std::unexpected(std::format(
        L"{}：WOF 压缩不可用，工具终止", std::move(xe_api).error()));
  }
  WofApi api_wof{std::move(xe_api).value()};
  auto sp_ctx{std::make_shared<ExecutorContext>()};
  sp_ctx->fn_set_data_location = api_wof.fn_set_data_location;
  sp_ctx->fn_is_external = api_wof.fn_is_external;
  return ExecuteFn{[sp_ctx = std::move(sp_ctx)](
                       const std::wstring& str_long_path,
                       Plan::Action e_action, const CurrentState& cs_current,
                       Algorithm e_target) {
    return execute_one(*sp_ctx, str_long_path, e_action, cs_current, e_target);
  }};
}

std::expected<ExecuteFn, std::wstring> make_real_executor() {
  return make_real_executor(load_wof_api());
}

}
