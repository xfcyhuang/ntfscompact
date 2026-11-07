#pragma once

import std;

#include <windows.h>

namespace ntfscompact {

inline constexpr unsigned int kExitInterrupted{3};

using InterruptPoller = std::function<bool()>;
using ForceExitFn = std::function<void()>;

struct InterruptFlags {
  std::atomic<bool> b_requested{false};
  std::atomic<bool> b_finishing{false};
};

inline bool interrupt_requested(const InterruptPoller& fn_poll) {
  return static_cast<bool>(fn_poll) && fn_poll();
}

inline BOOL deliver_console_event(InterruptFlags& fl_flags,
                                  const ForceExitFn& fn_force_exit) {
  if (fl_flags.b_finishing.load(std::memory_order_acquire)) {
    if (fn_force_exit) {
      fn_force_exit();
    }
    return TRUE;
  }
  fl_flags.b_requested.store(true, std::memory_order_release);
  return TRUE;
}

inline bool observe_interrupt(InterruptFlags& fl_flags) {
  if (!fl_flags.b_requested.load(std::memory_order_acquire)) {
    return false;
  }
  fl_flags.b_finishing.store(true, std::memory_order_release);
  return true;
}

std::expected<InterruptPoller, std::wstring> install_interrupt_handler();

}
