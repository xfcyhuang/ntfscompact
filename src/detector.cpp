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

using WofIsExternalFileFn = ULONG(WINAPI*)(LPCWSTR FilePath, PBOOL IsExternalFile,
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

}  // namespace

struct RealDetector::Impl {
    HMODULE wofutil = nullptr;
    WofIsExternalFileFn is_external = nullptr;
};

RealDetector::RealDetector(std::wstring* load_error) {
    impl_ = new Impl;
    impl_->wofutil = LoadLibraryW(L"wofutil.dll");
    if (impl_->wofutil == nullptr) {
        if (load_error != nullptr) {
            *load_error = L"无法加载 wofutil.dll（错误码 " +
                          std::to_wstring(GetLastError()) +
                          L"）：WOF 现状检测不可用，工具终止。该 DLL 为 Windows "
                          L"10+ 系统组件，请确认系统完整性。";
        }
        return;
    }
    impl_->is_external = reinterpret_cast<WofIsExternalFileFn>(
        GetProcAddress(impl_->wofutil, "WofIsExternalFile"));
    if (impl_->is_external == nullptr && load_error != nullptr) {
        *load_error = L"wofutil.dll 中未找到 WofIsExternalFile 导出（错误码 " +
                      std::to_wstring(GetLastError()) + L"）：无法检测 WOF 现状。";
    }
}

RealDetector::~RealDetector() {
    if (impl_ != nullptr) {
        if (impl_->wofutil != nullptr) {
            FreeLibrary(impl_->wofutil);
        }
        delete impl_;
    }
}

DetectionResult RealDetector::Detect(const std::wstring& long_path) {
    DetectionResult result;
    if (impl_ == nullptr || impl_->wofutil == nullptr ||
        impl_->is_external == nullptr) {
        result.error = L"wofutil.dll 未加载，无法检测";
        return result;
    }

    // 先查 WOF backing：一步拿到 provider 与算法
    BOOL is_external = FALSE;
    ULONG provider = 0;
    WofFileCompressionInfoV1 info{};
    ULONG info_len = sizeof(info);
    const ULONG err =
        impl_->is_external(long_path.c_str(), &is_external, &provider, &info,
                           &info_len);
    if (err != ERROR_SUCCESS) {
        // 目录/特殊路径会拒绝访问（探针实测 0x80070005）——按检测失败处理
        result.error = L"现状检测失败（WofIsExternalFile 错误码 " +
                       std::to_wstring(err) + L"）";
        return result;
    }
    if (is_external) {
        result.state.kind = CurrentState::Kind::Wof;
        result.state.provider = static_cast<int>(provider);
        result.state.algorithm = AlgorithmFromWof(info.Algorithm);
        return result;
    }

    // 非 WOF：查传统 NTFS 压缩（LZNT1）
    HANDLE handle = CreateFileW(long_path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = L"无法打开文件（错误码 " +
                       std::to_wstring(GetLastError()) + L"）";
        return result;
    }
    USHORT format = kFormatNone;
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_GET_COMPRESSION, nullptr, 0,
                                    &format, sizeof(format), &returned, nullptr);
    CloseHandle(handle);
    if (!ok) {
        result.error = L"现状检测失败（FSCTL_GET_COMPRESSION 错误码 " +
                       std::to_wstring(GetLastError()) + L"）";
        return result;
    }
    if (format != kFormatNone) {
        result.state.kind = CurrentState::Kind::Lznt1;  // 含 LZNT1 及其他传统 NTFS 压缩
    } else {
        result.state.kind = CurrentState::Kind::None;
    }
    return result;
}

}  // namespace ntfscompact
