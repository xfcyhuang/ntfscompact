// 生产检测器实现（工单 #3）
// wofutil.dll 动态加载（本机 MinGW 工具链无 wofutil import lib，见研究笔记）；
// WOF 算法经 WofIsExternalFile，LZNT1 经 FSCTL_GET_COMPRESSION。
#include "detect.hpp"

#include <windows.h>

#ifndef FSCTL_GET_COMPRESSION
#define FSCTL_GET_COMPRESSION \
    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 15, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

namespace ntfscompact {
namespace {

// wofapi.h 结构本地声明（MinGW 头文件不含 wofapi.h）
struct WofFileCompressionInfoV1 {
    ULONG Algorithm;
    ULONG Flags;
};

using WofIsExternalFilePtr = ULONG(WINAPI*)(LPCWSTR FilePath, PBOOL IsExternalFile,
                                           PULONG Provider,
                                           PVOID ExternalFileInfo,
                                           PULONG BufferLength);

// WOF 算法常量（winioctl.h/wofapi.h）
constexpr ULONG kAlgoXpress4k = 0;
constexpr ULONG kAlgoLzx = 1;
constexpr ULONG kAlgoXpress8k = 2;
constexpr ULONG kAlgoXpress16k = 3;

// FSCTL_GET_COMPRESSION 返回的格式常量（winioctl.h）
// COMPRESSION_FORMAT_LZNT1 = 2：非零格式一律按传统压缩处理（ADR 0001 重压路径）
constexpr USHORT kFormatNone = 0;  // COMPRESSION_FORMAT_NONE

Algorithm AlgorithmFromWof(ULONG algo) {
    switch (algo) {
        case kAlgoXpress4k: return Algorithm::Xpress4k;
        case kAlgoLzx: return Algorithm::Lzx;
        case kAlgoXpress8k: return Algorithm::Xpress8k;
        case kAlgoXpress16k: return Algorithm::Xpress16k;
        default: return Algorithm::Xpress4k;  // 未知值：仅 WOF 形态判定用，不参与决策
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

// 检测一文件：先查 WOF backing（一步拿到 provider 与算法），非 WOF 再查传统压缩
DetectionResult detect_with(const DetectorContext& ctx,
                            const std::wstring& long_path) {
    DetectionResult result;
    BOOL is_external = FALSE;
    ULONG provider = 0;
    WofFileCompressionInfoV1 info{};
    ULONG info_len = sizeof(info);
    const ULONG err = ctx.fn_is_external(long_path.c_str(), &is_external,
                                         &provider, &info, &info_len);
    if (err != ERROR_SUCCESS) {
        // 目录/特殊路径会拒绝访问（探针实测 0x80070005）——按检测失败处理
        result.error = std::format(L"现状检测失败（WofIsExternalFile 错误码 {}）", err);
        return result;
    }
    if (is_external) {
        result.state.kind = CurrentState::Kind::Wof;
        result.state.provider = static_cast<int>(provider);
        result.state.algorithm = AlgorithmFromWof(info.Algorithm);
        return result;
    }
    HANDLE handle = CreateFileW(long_path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = std::format(L"无法打开文件（错误码 {}）", GetLastError());
        return result;
    }
    USHORT format = kFormatNone;
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_GET_COMPRESSION, nullptr, 0,
                                    &format, sizeof(format), &returned, nullptr);
    CloseHandle(handle);
    if (!ok) {
        result.error =
            std::format(L"现状检测失败（FSCTL_GET_COMPRESSION 错误码 {}）",
                        GetLastError());
        return result;
    }
    if (format != kFormatNone) {
        result.state.kind = CurrentState::Kind::Lznt1;  // 含 LZNT1 及其他传统 NTFS 压缩
    } else {
        result.state.kind = CurrentState::Kind::None;
    }
    return result;
}

}  // namespace

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

}  // namespace ntfscompact
