#include "cli.hpp"

namespace ntfscompact {

ParseResult parse_args(const std::vector<std::wstring>& vec_args) {
  ParseResult pr_result{};
  bool b_has_algorithm{false};
  for (std::size_t u8_index{0}; u8_index < vec_args.size(); ++u8_index) {
    const std::wstring& str_arg{vec_args[u8_index]};
    if (str_arg == L"-a") {
      ++u8_index;
      if (u8_index >= vec_args.size()) {
        pr_result.str_error = L"选项 -a 缺少算法值";
        return pr_result;
      }
      const std::wstring& str_value{vec_args[u8_index]};
      if (str_value == L"xpress4k") {
        pr_result.opt_options.e_algorithm = Algorithm::kXpress4k;
      } else if (str_value == L"xpress8k") {
        pr_result.opt_options.e_algorithm = Algorithm::kXpress8k;
      } else if (str_value == L"xpress16k") {
        pr_result.opt_options.e_algorithm = Algorithm::kXpress16k;
      } else if (str_value == L"lzx") {
        pr_result.opt_options.e_algorithm = Algorithm::kLzx;
      } else {
        pr_result.str_error = L"非法算法值 '" + str_value +
                              L"'（应为 xpress4k|xpress8k|xpress16k|lzx）";
        return pr_result;
      }
      b_has_algorithm = true;
      if (pr_result.opt_options.b_uncompress) {
        pr_result.str_error = L"选项 -a 与 -u 互斥：解压模式不指定算法";
        return pr_result;
      }
    } else if (str_arg == L"-u") {
      if (b_has_algorithm) {
        pr_result.str_error = L"选项 -a 与 -u 互斥：解压模式不指定算法";
        return pr_result;
      }
      pr_result.opt_options.b_uncompress = true;
    } else if (str_arg == L"-n") {
      pr_result.opt_options.b_dry_run = true;
    } else if (str_arg == L"-q") {
      pr_result.opt_options.b_quiet = true;
    } else if (str_arg == L"-v") {
      pr_result.opt_options.b_verbose = true;
    } else if (str_arg == L"--help") {
      pr_result.b_want_help = true;
      pr_result.b_ok = true;
      return pr_result;
    } else if (str_arg == L"--version") {
      pr_result.b_want_version = true;
      pr_result.b_ok = true;
      return pr_result;
    } else if (!str_arg.empty() && str_arg[0] == L'-') {
      pr_result.str_error = L"未知选项 '" + str_arg + L"'";
      return pr_result;
    } else {
      pr_result.opt_options.vec_paths.push_back(str_arg);
    }
  }
  if (pr_result.opt_options.vec_paths.empty()) {
    pr_result.str_error = L"缺少路径参数";
    return pr_result;
  }
  pr_result.b_ok = true;
  return pr_result;
}

std::wstring usage_text() {
  return L"用法：ntfscompact <路径...> [-a xpress4k|xpress8k|xpress16k|lzx] [-u] [-n] [-q] [-v]\n"
         L"\n"
         L"NTFS 文件 WOF 压缩命令行工具。按指定算法压缩文件；已以指定算法压缩的跳过；\n"
         L"被占用时经升级释放链逐级尝试（Restart Manager 优雅关/强制关，无需提权）；\n"
         L"仍占用则跳过。\n"
         L"\n"
         L"选项：\n"
         L"  -a <算法>        指定算法：xpress4k | xpress8k | xpress16k | lzx（默认 xpress8k）\n"
         L"  -u               解压：解除压缩，恢复常规存储（与 -a 互斥）\n"
         L"  -n               干跑：只报告，不修改\n"
         L"  -q               静默：仅输出末尾汇总一行（失败仍输出到 stderr）\n"
         L"  -v               详细输出（含现状检测与字节数明细）\n"
         L"  --help           显示用法\n"
         L"  --version        显示版本\n"
         L"\n"
         L"退出码：0 全部成功；1 部分失败；2 用法错误\n";
}

}
