// 路径收集与递归遍历实现（工单 #3）
#include "scan.hpp"

namespace ntfscompact {
namespace {

constexpr std::uint64_t kMinCompressibleSize = 4096;  // ≤4KB 跳过

#ifndef IO_REPARSE_TAG_WOF
#define IO_REPARSE_TAG_WOF (0x80000017u)
#endif

// 系统关键文件黑名单（文件名级，大小写不敏感）
bool IsCriticalSystemFile(const std::wstring& display_path) {
    const std::size_t sep = display_path.find_last_of(L"\\/");
    const std::wstring name =
        sep == std::wstring::npos ? display_path : display_path.substr(sep + 1);
    return lstrcmpiW(name.c_str(), L"pagefile.sys") == 0 ||
           lstrcmpiW(name.c_str(), L"hiberfil.sys") == 0 ||
           lstrcmpiW(name.c_str(), L"swapfile.sys") == 0;
}

// 路径任一组件为 WinSxS（大小写不敏感，按组件精确匹配，前缀相似不误伤）
bool HasWinSxSComponent(const std::wstring& display_path) {
    std::size_t begin = 0;
    while (begin <= display_path.size()) {
        const std::size_t end = display_path.find_first_of(L"\\/", begin);
        const std::wstring comp =
            display_path.substr(begin, end == std::wstring::npos
                                           ? std::wstring::npos
                                           : end - begin);
        if (!comp.empty() && lstrcmpiW(comp.c_str(), L"WinSxS") == 0) {
            return true;
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return false;
}

// 规范化为绝对 \\?\ 长路径（>260 字符亦可）。已带前缀的原样返回（UNC 补 UNC 形式）。
std::wstring ToLongPath(const std::wstring& raw) {
    if (raw.rfind(L"\\\\?\\", 0) == 0) return raw;
    wchar_t buffer[32768];
    const DWORD n = GetFullPathNameW(raw.c_str(), 32768, buffer, nullptr);
    if (n == 0 || n >= 32768) return raw;  // 规范化失败：按原样交给文件系统报错
    std::wstring full(buffer, n);
    if (full.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC" + full.substr(1);  // \\server\share → \\?\UNC\server\share
    }
    return L"\\\\?\\" + full;
}

// \\?\ 长路径 → 显示路径
std::wstring ToDisplayPath(const std::wstring& long_path) {
    if (long_path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        return L"\\" + long_path.substr(7);
    }
    if (long_path.rfind(L"\\\\?\\", 0) == 0) {
        return long_path.substr(4);
    }
    return long_path;
}

bool HasWildcard(const std::wstring& path) {
    return path.find_first_of(L"*?") != std::wstring::npos;
}

std::uint64_t FindDataSize(const WIN32_FIND_DATAW& fd) {
    ULARGE_INTEGER n{};
    n.LowPart = fd.nFileSizeLow;
    n.HighPart = fd.nFileSizeHigh;
    return n.QuadPart;
}

bool IsDirectory(const WIN32_FIND_DATAW& fd) {
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsReparse(const WIN32_FIND_DATAW& fd) {
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// 遍历目录（不含 reparse 防环原则见头注释）
void WalkDirectory(const std::wstring& long_dir, const std::wstring& display_dir,
                   ScanOutcome& out) {
    const std::wstring pattern = long_dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        out.errors.push_back({display_dir, L"无法访问（不存在或权限不足）"});
        return;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring child_long = long_dir + L"\\" + fd.cFileName;
        const std::wstring child_disp = display_dir + L"\\" + fd.cFileName;
        if (IsDirectory(fd)) {
            if (IsReparse(fd)) {
                // 不进符号链接/junction：防环核心
                out.skipped.push_back(
                    {child_disp, L"目录为符号链接/junction，不进入"});
            } else if (HasWinSxSComponent(child_disp)) {
                // 黑名单子树整树剪枝：避免遍历 WinSxS 海量文件
                out.skipped.push_back({child_disp, L"位于 WinSxS 目录"});
            } else {
                WalkDirectory(child_long, child_disp, out);
            }
        } else {
            FileMetadata meta;
            meta.display_path = child_disp;
            meta.attributes = fd.dwFileAttributes;
            meta.reparse_tag = fd.dwReserved0;
            meta.size = FindDataSize(fd);
            std::wstring reason;
            if (CheckFileCandidate(meta, &reason)) {
                out.items.push_back({child_long, child_disp, meta.size});
            } else {
                out.skipped.push_back({child_disp, reason});
            }
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

// 显式路径参数（无通配符）：文件 → 过滤收录；目录 → 递归；reparse 目录 → 跳过
void HandleExplicitPath(const std::wstring& raw, ScanOutcome& out) {
    const std::wstring long_path = ToLongPath(raw);
    const std::wstring display = ToDisplayPath(long_path);
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(long_path.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        out.errors.push_back({display, L"无法访问（不存在或权限不足）"});
        return;
    }
    FindClose(find);
    if (IsDirectory(fd)) {
        if (IsReparse(fd)) {
            out.skipped.push_back({display, L"目录为符号链接/junction，不进入"});
        } else {
            WalkDirectory(long_path, display, out);
        }
        return;
    }
    FileMetadata meta;
    meta.display_path = display;
    meta.attributes = fd.dwFileAttributes;
    meta.reparse_tag = fd.dwReserved0;
    meta.size = FindDataSize(fd);
    std::wstring reason;
    if (CheckFileCandidate(meta, &reason)) {
        out.items.push_back({long_path, display, meta.size});
    } else {
        out.skipped.push_back({display, reason});
    }
}

// 通配符参数：仅匹配该层文件（目录不递归，对齐 compact.exe 语义）
void HandleWildcard(const std::wstring& raw, ScanOutcome& out) {
    const std::size_t sep = raw.find_last_of(L"\\/");
    const std::wstring file_pattern =
        sep == std::wstring::npos ? raw : raw.substr(sep + 1);
    const std::wstring dir_part = [&]() -> std::wstring {
        if (sep == std::wstring::npos) return L".";
        if (sep == 0) return L"\\";
        return raw.substr(0, sep);
    }();
    const std::wstring parent_long = ToLongPath(dir_part);
    const std::wstring parent_disp = ToDisplayPath(parent_long);

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((parent_long + L"\\" + file_pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        out.errors.push_back({parent_disp + L"\\" + file_pattern,
                              L"无法访问（不存在或权限不足）"});
        return;
    }
    do {
        if (IsDirectory(fd)) continue;  // 通配符仅匹配文件
        const std::wstring child_disp = parent_disp + L"\\" + fd.cFileName;
        FileMetadata meta;
        meta.display_path = child_disp;
        meta.attributes = fd.dwFileAttributes;
        meta.reparse_tag = fd.dwReserved0;
        meta.size = FindDataSize(fd);
        std::wstring reason;
        if (CheckFileCandidate(meta, &reason)) {
            out.items.push_back({parent_long + L"\\" + fd.cFileName, child_disp,
                                 meta.size});
        } else {
            out.skipped.push_back({child_disp, reason});
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

}  // namespace

bool CheckFileCandidate(const FileMetadata& metadata, std::wstring* skip_reason) {
    if (IsCriticalSystemFile(metadata.display_path)) {
        const std::size_t sep = metadata.display_path.find_last_of(L"\\/");
        *skip_reason = L"系统关键文件（" +
                       (sep == std::wstring::npos
                            ? metadata.display_path
                            : metadata.display_path.substr(sep + 1)) +
                       L"）";
        return false;
    }
    // 文件判定只看目录部分（最后一段是文件名，文件名本身叫 WinSxS 不误伤）
    const std::size_t sep = metadata.display_path.find_last_of(L"\\/");
    const std::wstring dir_part =
        sep == std::wstring::npos ? std::wstring()
                                  : metadata.display_path.substr(0, sep);
    if (HasWinSxSComponent(dir_part)) {
        *skip_reason = L"位于 WinSxS 目录";
        return false;
    }
    if (metadata.attributes & FILE_ATTRIBUTE_ENCRYPTED) {
        *skip_reason = L"EFS 加密文件";
        return false;
    }
    if (metadata.attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        // WOF 压缩文件本身带 reparse tag，放行交现状检测；其余（符号链接/
        // junction/Cloud 占位等）不属常规存储，跳过
        if (metadata.reparse_tag != IO_REPARSE_TAG_WOF) {
            *skip_reason = L"非常规 reparse point（符号链接/junction/Cloud 等）";
            return false;
        }
    }
    if (metadata.size <= kMinCompressibleSize) {
        *skip_reason = L"文件大小 ≤4KB";
        return false;
    }
    return true;
}

void ScanPaths(const std::vector<std::wstring>& raw_paths, ScanOutcome& out) {
    for (const std::wstring& raw : raw_paths) {
        if (HasWildcard(raw)) {
            HandleWildcard(raw, out);
        } else {
            HandleExplicitPath(raw, out);
        }
    }
}

}  // namespace ntfscompact
