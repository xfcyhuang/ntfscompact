import std;

#include "interrupt.hpp"

#include <windows.h>

namespace ntfscompact {
namespace {

InterruptFlags g_flags{};
ForceExitFn g_fn_force_exit{[]() { ExitProcess(kExitInterrupted); }};

BOOL WINAPI console_ctrl_trampoline(DWORD) {
  return deliver_console_event(g_flags, g_fn_force_exit);
}

}

std::expected<InterruptPoller, std::wstring> install_interrupt_handler() {
  if (!SetConsoleCtrlHandler(console_ctrl_trampoline, TRUE)) {
    return std::unexpected(
        std::format(L"注册控制台中断处理器失败（错误码 {}）", GetLastError()));
  }
  return InterruptPoller{
      []() { return observe_interrupt(g_flags); }};
}

}
