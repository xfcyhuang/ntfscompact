#include "detect.hpp"

#include <windows.h>

#ifndef FSCTL_GET_COMPRESSION
#define FSCTL_GET_COMPRESSION \
  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 15, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

namespace ntfscompact {
namespace {

struct WofFileCompressionInfoV1 {
  ULONG u4_algorithm{};
  ULONG u4_flags{};
};

using WofIsExternalFilePtr = ULONG(WINAPI*)(LPCWSTR, PBOOL, PULONG, PVOID,
                                            PULONG);

constexpr ULONG kAlgoXpress4k{0};
constexpr ULONG kAlgoLzx{1};
constexpr ULONG kAlgoXpress8k{2};
constexpr ULONG kAlgoXpress16k{3};

constexpr USHORT kFormatNone{0};

Algorithm algorithm_from_wof(ULONG u4_algo) {
  switch (u4_algo) {
    case kAlgoXpress4k: return Algorithm::kXpress4k;
    case kAlgoLzx: return Algorithm::kLzx;
    case kAlgoXpress8k: return Algorithm::kXpress8k;
    case kAlgoXpress16k: return Algorithm::kXpress16k;
    default: return Algorithm::kXpress4k;
  }
}

struct HmoduleReleaser {
  void operator()(HMODULE h_module) const {
    if (h_module != nullptr) {
      FreeLibrary(h_module);
    }
  }
};

using HmoduleGuard =
    std::unique_ptr<std::remove_pointer_t<HMODULE>, HmoduleReleaser>;

struct DetectorContext {
  HmoduleGuard guard_module{};
  std::function<ULONG WINAPI(LPCWSTR, PBOOL, PULONG, PVOID, PULONG)>
      fn_is_external{};
};

DetectionResult detect_with(const DetectorContext& ctx_detector,
                            const std::wstring& str_long_path) {
  DetectionResult dr_result{};
  BOOL b_external{FALSE};
  ULONG u4_provider{};
  WofFileCompressionInfoV1 info_current{};
  ULONG u4_info_len{sizeof(info_current)};
  const ULONG u4_err{ctx_detector.fn_is_external(
      str_long_path.c_str(), &b_external, &u4_provider, &info_current,
      &u4_info_len)};
  if (u4_err != ERROR_SUCCESS) {
    dr_result.str_error =
        std::format(L"现状检测失败（WofIsExternalFile 错误码 {}）", u4_err);
    return dr_result;
  }
  if (b_external) {
    dr_result.cs_state.e_kind = CurrentState::Kind::kWof;
    dr_result.cs_state.i4_provider = static_cast<int>(u4_provider);
    dr_result.cs_state.e_algorithm = algorithm_from_wof(info_current.u4_algorithm);
    return dr_result;
  }
  HANDLE h_file{CreateFileW(str_long_path.c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    dr_result.str_error =
        std::format(L"无法打开文件（错误码 {}）", GetLastError());
    return dr_result;
  }
  USHORT u2_format{kFormatNone};
  DWORD u4_returned{};
  const BOOL b_ok{DeviceIoControl(h_file, FSCTL_GET_COMPRESSION, nullptr, 0,
                                  &u2_format, sizeof(u2_format), &u4_returned,
                                  nullptr)};
  CloseHandle(h_file);
  if (!b_ok) {
    dr_result.str_error =
        std::format(L"现状检测失败（FSCTL_GET_COMPRESSION 错误码 {}）",
                    GetLastError());
    return dr_result;
  }
  if (u2_format != kFormatNone) {
    dr_result.cs_state.e_kind = CurrentState::Kind::kLznt1;
  } else {
    dr_result.cs_state.e_kind = CurrentState::Kind::kNone;
  }
  return dr_result;
}

}

auto make_real_detector() -> std::expected<DetectFn, std::wstring> {
  HMODULE h_module{LoadLibraryW(L"wofutil.dll")};
  if (h_module == nullptr) {
    return std::unexpected(std::format(
        L"无法加载 wofutil.dll（错误码 {}）：WOF 现状检测不可用，工具终止。"
        L"该 DLL 为 Windows 10+ 系统组件，请确认系统完整性。",
        GetLastError()));
  }
  HmoduleGuard guard_module{h_module};
  FARPROC ptr_is{GetProcAddress(h_module, "WofIsExternalFile")};
  if (ptr_is == nullptr) {
    return std::unexpected(std::format(
        L"wofutil.dll 中未找到 WofIsExternalFile 导出（错误码 {}）：无法检测 WOF 现状。",
        GetLastError()));
  }
  auto sp_ctx{std::make_shared<DetectorContext>()};
  sp_ctx->guard_module = std::move(guard_module);
  sp_ctx->fn_is_external = reinterpret_cast<WofIsExternalFilePtr>(ptr_is);
  return DetectFn{[sp_ctx = std::move(sp_ctx)](
                      const std::wstring& str_long_path) {
    return detect_with(*sp_ctx, str_long_path);
  }};
}

}
