import std;

#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>

namespace {

struct HoldState {
  HANDLE h_file{INVALID_HANDLE_VALUE};
  bool b_stubborn{false};
};

HoldState g_state{};

int coop_hold(const wchar_t* ptr_file, UINT u4_close_after_ms) {
  HANDLE h_file{CreateFileW(ptr_file, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return 3;
  }
  REQUEST_OPLOCK_INPUT_BUFFER roi_input{};
  roi_input.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
  roi_input.StructureLength = sizeof(roi_input);
  roi_input.RequestedOplockLevel = OPLOCK_LEVEL_CACHE_READ |
                                   OPLOCK_LEVEL_CACHE_WRITE |
                                   OPLOCK_LEVEL_CACHE_HANDLE;
  roi_input.Flags = REQUEST_OPLOCK_INPUT_FLAG_REQUEST;
  REQUEST_OPLOCK_OUTPUT_BUFFER roo_output{};
  OVERLAPPED ov_break{};
  ov_break.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ov_break.hEvent == nullptr) {
    CloseHandle(h_file);
    return 4;
  }
  const BOOL b_posted{DeviceIoControl(
      h_file, FSCTL_REQUEST_OPLOCK, &roi_input, sizeof(roi_input),
      &roo_output, sizeof(roo_output), nullptr, &ov_break)};
  if (b_posted == FALSE && GetLastError() != ERROR_IO_PENDING) {
    CloseHandle(ov_break.hEvent);
    CloseHandle(h_file);
    return 5;
  }
  HANDLE arr_waits[2]{ov_break.hEvent, nullptr};
  DWORD u4_wait_count{1};
  HANDLE h_timer{nullptr};
  if (u4_close_after_ms > 0) {
    h_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (h_timer != nullptr) {
      LARGE_INTEGER li_due{};
      li_due.QuadPart = -10000LL * static_cast<long long>(u4_close_after_ms);
      SetWaitableTimer(h_timer, &li_due, 0, nullptr, nullptr, FALSE);
      arr_waits[1] = h_timer;
      u4_wait_count = 2;
    }
  }
  WaitForMultipleObjects(u4_wait_count, arr_waits, FALSE, INFINITE);
  if (h_timer != nullptr) {
    CloseHandle(h_timer);
  }
  CloseHandle(ov_break.hEvent);
  CloseHandle(h_file);
  return 0;
}

int lock0_hold(const wchar_t* ptr_file, UINT u4_close_after_ms) {
  HANDLE h_file{CreateFileW(ptr_file, 0, FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return 3;
  }
  Sleep(u4_close_after_ms > 0 ? u4_close_after_ms : INFINITE);
  CloseHandle(h_file);
  return 0;
}

LRESULT CALLBACK hold_window_procedure(HWND h_wnd, UINT u_msg, WPARAM w_param,
                                       LPARAM l_param) {
  switch (u_msg) {
    case WM_CLOSE:
      if (!g_state.b_stubborn) {
        DestroyWindow(h_wnd);
      }
      return 0;
    case WM_ENDSESSION:
      if (w_param != 0 && !g_state.b_stubborn) {
        DestroyWindow(h_wnd);
      }
      return 0;
    case WM_TIMER:
      DestroyWindow(h_wnd);
      return 0;
    case WM_DESTROY:
      if (g_state.h_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_state.h_file);
        g_state.h_file = INVALID_HANDLE_VALUE;
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h_wnd, u_msg, w_param, l_param);
}

}

int WINAPI wWinMain(HINSTANCE h_instance, HINSTANCE, PWSTR, int) {
  int i_argc{0};
  wchar_t** arr_argv{CommandLineToArgvW(GetCommandLineW(), &i_argc)};
  if (arr_argv == nullptr || i_argc < 2) {
    return 2;
  }
  bool b_coop{false};
  bool b_lock0{false};
  UINT u4_close_after_ms{0};
  for (int i4_index{2}; i4_index < i_argc; ++i4_index) {
    if (std::wcscmp(arr_argv[i4_index], L"--stubborn") == 0) {
      g_state.b_stubborn = true;
    } else if (std::wcscmp(arr_argv[i4_index], L"--coop") == 0) {
      b_coop = true;
    } else if (std::wcscmp(arr_argv[i4_index], L"--lock0") == 0) {
      b_lock0 = true;
    } else if (std::wcscmp(arr_argv[i4_index], L"--close-after-ms") == 0 &&
               i4_index + 1 < i_argc) {
      ++i4_index;
      u4_close_after_ms =
          static_cast<UINT>(std::wcstoul(arr_argv[i4_index], nullptr, 10));
    }
  }
  if (b_coop) {
    const int i4_code{coop_hold(arr_argv[1], u4_close_after_ms)};
    LocalFree(arr_argv);
    return i4_code;
  }
  if (b_lock0) {
    const int i4_code{lock0_hold(arr_argv[1], u4_close_after_ms)};
    LocalFree(arr_argv);
    return i4_code;
  }
  g_state.h_file = CreateFileW(arr_argv[1], GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
  if (g_state.h_file == INVALID_HANDLE_VALUE) {
    LocalFree(arr_argv);
    return 3;
  }
  LocalFree(arr_argv);
  WNDCLASSEXW wc_hold{};
  wc_hold.cbSize = sizeof(wc_hold);
  wc_hold.lpfnWndProc = hold_window_procedure;
  wc_hold.hInstance = h_instance;
  wc_hold.lpszClassName = L"ntfscompact_hold_window";
  if (RegisterClassExW(&wc_hold) == 0) {
    return 4;
  }
  const HWND h_wnd{CreateWindowExW(0, L"ntfscompact_hold_window",
                                   L"ntfscompact hold", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   CW_USEDEFAULT, nullptr, nullptr, h_instance,
                                   nullptr)};
  if (h_wnd == nullptr) {
    return 5;
  }
  ShowWindow(h_wnd, SW_HIDE);
  if (u4_close_after_ms > 0) {
    SetTimer(h_wnd, 1, u4_close_after_ms, nullptr);
  }
  MSG msg_hold{};
  while (GetMessageW(&msg_hold, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg_hold);
    DispatchMessageW(&msg_hold);
  }
  return static_cast<int>(msg_hold.wParam);
}
