import std;

#include "interrupt.hpp"

#include <windows.h>

namespace ntfscompact {
namespace {

constexpr unsigned int kWrapupWaitBudgetMs{4000};
constexpr unsigned int kWrapupWaitStepMs{50};

InterruptFlags g_flags{};
ForceExitFn g_fn_force_exit{[]() { ExitProcess(kExitInterrupted); }};

void wait_step(unsigned int u4_ms) {
  Sleep(u4_ms);
}

BOOL WINAPI console_ctrl_trampoline(DWORD u4_event) {
  switch (u4_event) {
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      return deliver_system_stop_event(g_flags, g_fn_force_exit, wait_step,
                                       kWrapupWaitBudgetMs, kWrapupWaitStepMs);
    default:
      return deliver_console_event(g_flags, g_fn_force_exit);
  }
}

}

std::expected<InterruptWiring, std::wstring> install_interrupt_handler() {
  if (!SetConsoleCtrlHandler(console_ctrl_trampoline, TRUE)) {
    return std::unexpected(
        std::format(L"注册控制台中断处理器失败（错误码 {}）", GetLastError()));
  }
  return InterruptWiring{
      InterruptPoller{[]() { return observe_interrupt(g_flags); }},
      WrapupSignalFn{[]() { mark_wrapup_complete(g_flags); }}};
}

}
