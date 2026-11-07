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
    } else if (str_arg == L"--force-close-all") {
      pr_result.opt_options.b_force_close_all = true;
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
  return L"用法：ntfscompact <路径...> [-a xpress4k|xpress8k|xpress16k|lzx] [-u] [-n] [-q] [-v] [--force-close-all]\n"
         L"\n"
         L"NTFS 文件 WOF 压缩命令行工具。按指定算法压缩文件；已以指定算法压缩的跳过；\n"
         L"被占用时经升级释放链逐级尝试（Oplock 协商、Restart Manager 优雅关/强制关、\n"
         L"句柄强关——需管理员权限）；仍占用则跳过。\n"
         L"\n"
         L"选项：\n"
         L"  -a <算法>        指定算法：xpress4k | xpress8k | xpress16k | lzx（默认 xpress8k）\n"
         L"  -u               解压：解除压缩，恢复常规存储（与 -a 互斥）\n"
         L"  -n               干跑：只报告，不修改\n"
         L"  -q               静默：仅输出末尾汇总一行（失败仍输出到 stderr）\n"
         L"  -v               详细输出（含现状检测与字节数明细）\n"
         L"  --force-close-all\n"
         L"                   放行句柄强关风险闸门（默认只关低风险占用者；explorer、\n"
         L"                   数据库/虚拟机/编辑器类有状态进程与系统关键进程默认跳过；\n"
         L"                   此开关显式放行全部，自担风险）\n"
         L"  --help           显示用法\n"
         L"  --version        显示版本\n"
         L"\n"
         L"中断：运行期间 Ctrl+C（关窗/注销/关机同理）在当前项边界停机，在途项\n"
         L"（含占用升级链全程）完整走完后恢复只读守卫、统计字节，输出部分汇总\n"
         L"与中断标注行；收尾期间再次事件立即强制退出。极少数内核态卡死线程\n"
         L"连强制退出也无法中断，无解。\n"
         L"\n"
         L"退出码：0 全部成功；1 部分失败；2 用法错误；3 已被 Ctrl+C 中断\n";
}

}
