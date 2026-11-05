// 路径收集与递归遍历（工单 #3）
// ScanPaths：路径参数（文件/目录/通配符）→ 候选文件 + 跳过记录 + 错误记录。
// 遍历原则：目录递归含子目录；不进任何 reparse 目录（符号链接/junction），
// NTFS 目录环必经 reparse point，故不进入即防环；路径内部规范化为 \\?\ 长路径。
#pragma once

import std;

#include <windows.h>

namespace ntfscompact {

// 通过过滤的候选文件
struct ScanItem {
    std::wstring long_path;     // \\?\ 规范长路径，检测/后续压缩用
    std::wstring display_path;  // 显示路径（去 \\?\ 前缀）
    std::uint64_t size = 0;          // 逻辑大小（字节）
};

// 跳过记录（GLOSSARY「跳过」：不做任何修改，仅记录）
struct ScanSkip {
    std::wstring display_path;
    std::wstring reason;
};

// 访问失败记录（非跳过，计失败）
struct ScanError {
    std::wstring display_path;
    std::wstring message;
};

struct ScanOutcome {
    std::vector<ScanItem> items;    // 候选文件
    std::vector<ScanSkip> skipped;  // 被跳过项（含原因）
    std::vector<ScanError> errors;  // 无法访问的路径（计失败）
};

// 文件元数据（来自 WIN32_FIND_DATAW），供过滤判定
struct FileMetadata {
    std::wstring display_path;
    DWORD attributes = 0;    // dwFileAttributes
    DWORD reparse_tag = 0;   // dwReserved0（属性含 REPARSE_POINT 时有效）
    std::uint64_t size = 0;       // nFileSize{Low,High}
};

// 单文件过滤判定（纯函数，便于合成属性单测）：黑名单文件名（pagefile/hiberfil/
// swapfile）、WinSxS 路径组件、EFS 加密、非常规 reparse point、≤4KB。
// 收录返回 true；跳过返回 false 并经 skip_reason 给出原因。
bool CheckFileCandidate(const FileMetadata& metadata, std::wstring* skip_reason);

// 展开路径参数并递归遍历。只读，无副作用。
void ScanPaths(const std::vector<std::wstring>& raw_paths, ScanOutcome& out);

}  // namespace ntfscompact
