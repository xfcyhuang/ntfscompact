// 测试用临时目录助手（工单 #3）：每实例在 %TEMP% 下建唯一目录，析构递归清理。
// 清理对 reparse 目录只删除 junction 本身、不进入（与产品遍历同一防环原则）。
#pragma once

#include <windows.h>

#include <cstdio>
#include <string>

namespace ntfscompact_test {

class TempDir {
public:
    // 内部 API 调用统一走 \\?\ 前缀（绝对路径超 MAX_PATH 时必需）
    static std::wstring LongApiPath(const std::wstring& absolute) {
        if (absolute.rfind(L"\\\\?\\", 0) == 0) return absolute;
        return L"\\\\?\\" + absolute;
    }

    TempDir() {
        wchar_t base[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, base);
        if (n == 0 || n >= MAX_PATH) return;
        static unsigned counter = 0;
        wchar_t tail[64];
        swprintf(tail, 64, L"ntfscompact-t3-%lu-%u",
                 static_cast<unsigned long>(GetCurrentProcessId()), counter++);
        dir_ = std::wstring(base) + tail;
        if (!CreateDirectoryW(dir_.c_str(), nullptr)) {
            dir_.clear();
            return;
        }
        ok_ = true;
    }

    ~TempDir() {
        if (ok_) RemoveRecursive(LongApiPath(dir_));
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    bool ok() const { return ok_; }
    const std::wstring& path() const { return dir_; }

    // 子路径：path() + "\\" + name
    std::wstring Sub(const std::wstring& name) const { return dir_ + L"\\" + name; }

    // 建子目录（可多级：dir\a\b 形式逐级创建）
    bool MakeDir(const std::wstring& sub) {
        const std::wstring full = Sub(sub);
        if (CreateDirectoryW(LongApiPath(full).c_str(), nullptr)) return true;
        // 逐级建（SHCreateDirectory 不引入，手工拆分）
        std::wstring cur;
        size_t i = 0;
        while (i < full.size()) {
            const size_t next = full.find(L'\\', i);
            if (next == std::wstring::npos) break;
            if (next > 0) {
                cur = full.substr(0, next);
                CreateDirectoryW(LongApiPath(cur).c_str(), nullptr);
            }
            i = next + 1;
        }
        return CreateDirectoryW(LongApiPath(full).c_str(), nullptr) ||
               ((GetFileAttributesW(LongApiPath(full).c_str()) &
                 FILE_ATTRIBUTE_DIRECTORY) != 0);
    }

    // 在子路径写 size 字节的文件（内容全零）
    bool WriteFile(const std::wstring& sub, unsigned long size) {
        const std::wstring full = Sub(sub);
        HANDLE h = CreateFileW(LongApiPath(full).c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        static const char kBuffer[65536] = {};
        unsigned long left = size;
        while (left > 0) {
            const DWORD chunk = left > sizeof(kBuffer)
                                    ? static_cast<DWORD>(sizeof(kBuffer))
                                    : static_cast<DWORD>(left);
            DWORD written = 0;
            if (!::WriteFile(h, kBuffer, chunk, &written, nullptr) ||
                written != chunk) {
                CloseHandle(h);
                return false;
            }
            left -= chunk;
        }
        CloseHandle(h);
        return true;
    }

private:
    // 递归删除：入参与递归子路径均带 \\?\ 前缀（长路径超 260 亦可删）
    static void RemoveRecursive(const std::wstring& long_dir) {
        const std::wstring pattern = long_dir + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 ||
                    wcscmp(fd.cFileName, L"..") == 0) {
                    continue;
                }
                const std::wstring child = long_dir + L"\\" + fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    RemoveRecursive(child);
                } else {
                    DeleteFileW(child.c_str());
                    RemoveDirectoryW(child.c_str());  // junction/符号链接本体
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(long_dir.c_str());
    }

    std::wstring dir_;
    bool ok_ = false;
};

}  // namespace ntfscompact_test
