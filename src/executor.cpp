#include "executor.hpp"

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

constexpr ULONG kWofProviderFile{2};
constexpr ULONG kWofAlgoXpress4k{0};
constexpr ULONG kWofAlgoLzx{1};
constexpr ULONG kWofAlgoXpress8k{2};
constexpr ULONG kWofAlgoXpress16k{3};
constexpr ULONG kHresultFacilityWin32{0x80070000U};
constexpr int kTransientRetryMax{10};
constexpr DWORD kTransientRetryDelayMs{50};

struct WofFileCompressionInfoV1 {
  ULONG u4_algorithm{};
  ULONG u4_flags{};
};

using WofSetFileDataLocationPtr = ULONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG);
using WofIsExternalFilePtr =
    ULONG(WINAPI*)(LPCWSTR, PBOOL, PULONG, PVOID, PULONG);

struct HmoduleReleaser {
  void operator()(HMODULE h_module) const {
    if (h_module != nullptr) {
      FreeLibrary(h_module);
    }
  }
};

using HmoduleGuard =
    std::unique_ptr<std::remove_pointer_t<HMODULE>, HmoduleReleaser>;

struct HandleReleaser {
  void operator()(HANDLE h_file) const { CloseHandle(h_file); }
};

using HandleGuard = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleReleaser>;

struct ExecutorContext {
  HmoduleGuard guard_module{};
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

HandleGuard open_file_for_compress(const std::wstring& str_long_path) {
  for (int i4_attempt{0};; ++i4_attempt) {
    HandleGuard guard_file{CreateFileW(
        str_long_path.c_str(),
        GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL,
        nullptr)};
    if (guard_file) {
      return guard_file;
    }
    const DWORD u4_error{GetLastError()};
    if (i4_attempt >= kTransientRetryMax ||
        !is_transient_win32_error(u4_error)) {
      return nullptr;
    }
    Sleep(kTransientRetryDelayMs);
  }
}

IoctlOutcome ioctl_with_retry(HANDLE h_file, DWORD u4_control, void* ptr_in,
                              DWORD u4_in_len) {
  IoctlOutcome outcome{};
  for (int i4_attempt{0};; ++i4_attempt) {
    DWORD u4_returned{};
    outcome.b_ok = DeviceIoControl(h_file, u4_control, ptr_in, u4_in_len,
                                   nullptr, 0, &u4_returned, nullptr) != 0;
    if (outcome.b_ok) {
      break;
    }
    outcome.u4_error = GetLastError();
    if (i4_attempt >= kTransientRetryMax ||
        !is_transient_win32_error(outcome.u4_error)) {
      break;
    }
    Sleep(kTransientRetryDelayMs);
  }
  return outcome;
}

ExecuteResult compress_core(const ExecutorContext& ctx_execute,
                            const std::wstring& str_long_path,
                            Plan::Action e_action,
                            const CurrentState& cs_current, Algorithm e_target) {
  HandleGuard guard_file{open_file_for_compress(str_long_path)};
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
  ULONG u4_compress{ERROR_SHARING_VIOLATION};
  for (int i4_attempt{0};; ++i4_attempt) {
    u4_compress = ctx_execute.fn_set_data_location(
        guard_file.get(), kWofProviderFile, &info_request, sizeof(info_request));
    if (i4_attempt >= kTransientRetryMax ||
        !is_transient_win32_error(u4_compress)) {
      break;
    }
    Sleep(kTransientRetryDelayMs);
  }
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
  const std::wstring str_mismatch{check_compression_meets_target(
      b_external != FALSE, static_cast<int>(u4_provider),
      from_wof_algorithm(info_verified.u4_algorithm), e_target)};
  if (!str_mismatch.empty()) {
    return make_error(str_mismatch, 0);
  }
  return ExecuteResult{};
}

ExecuteResult execute_one(const ExecutorContext& ctx_execute,
                          const std::wstring& str_long_path,
                          Plan::Action e_action,
                          const CurrentState& cs_current, Algorithm e_target) {
  if (e_action == Plan::Action::kUncompress || e_action == Plan::Action::kSkip) {
    return make_error(L"解压执行段属工单 T4，未实现", 0);
  }
  const DWORD u4_attrs{GetFileAttributesW(str_long_path.c_str())};
  if (u4_attrs == INVALID_FILE_ATTRIBUTES) {
    return make_error(
        std::format(L"无法读取文件属性（错误码 {}）", GetLastError()),
        GetLastError());
  }
  const bool b_readonly{(u4_attrs & FILE_ATTRIBUTE_READONLY) != 0};
  if (b_readonly &&
      !SetFileAttributesW(str_long_path.c_str(),
                          u4_attrs & ~FILE_ATTRIBUTE_READONLY)) {
    return make_error(
        std::format(L"临时清除只读属性失败（错误码 {}）", GetLastError()),
        GetLastError());
  }
  ExecuteResult er_result{compress_core(ctx_execute, str_long_path, e_action,
                                        cs_current, e_target)};
  if (b_readonly) {
    if (!SetFileAttributesW(str_long_path.c_str(), u4_attrs)) {
      er_result.e_outcome = ExecuteResult::Outcome::kError;
      er_result.str_message += std::format(L"；恢复只读属性失败（错误码 {}）",
                                           GetLastError());
    }
  }
  return er_result;
}

}

std::expected<ExecuteFn, std::wstring> make_real_executor() {
  HMODULE h_module{LoadLibraryW(L"wofutil.dll")};
  if (h_module == nullptr) {
    return std::unexpected(std::format(
        L"无法加载 wofutil.dll（错误码 {}）：WOF 压缩不可用，工具终止",
        GetLastError()));
  }
  HmoduleGuard guard_module{h_module};
  FARPROC ptr_set{GetProcAddress(h_module, "WofSetFileDataLocation")};
  FARPROC ptr_is{GetProcAddress(h_module, "WofIsExternalFile")};
  if (ptr_set == nullptr || ptr_is == nullptr) {
    return std::unexpected(std::format(
        L"wofutil.dll 缺少 WOF 压缩导出（错误码 {}）", GetLastError()));
  }
  auto sp_ctx{std::make_shared<ExecutorContext>()};
  sp_ctx->fn_set_data_location =
      reinterpret_cast<WofSetFileDataLocationPtr>(ptr_set);
  sp_ctx->fn_is_external = reinterpret_cast<WofIsExternalFilePtr>(ptr_is);
  sp_ctx->guard_module = std::move(guard_module);
  return ExecuteFn{[sp_ctx = std::move(sp_ctx)](
                       const std::wstring& str_long_path,
                       Plan::Action e_action, const CurrentState& cs_current,
                       Algorithm e_target) {
    return execute_one(*sp_ctx, str_long_path, e_action, cs_current, e_target);
  }};
}

}
