#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define CODEX_MAX_HOOKS 64

typedef struct CodexCursorHook {
  HWND window;
  WNDPROC previous;
} CodexCursorHook;

static CodexCursorHook g_hooks[CODEX_MAX_HOOKS];
static volatile LONG g_hook_count = 0;
static volatile LONG g_hidden = 0;
static HCURSOR g_previous_cursor = NULL;

static LRESULT CALLBACK CodexCursorGuardProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
  if (InterlockedCompareExchange(&g_hidden, 0, 0) != 0 &&
      message == WM_SETCURSOR &&
      LOWORD(lparam) == HTCLIENT) {
    SetCursor(NULL);
    return TRUE;
  }

  LONG count = InterlockedCompareExchange(&g_hook_count, 0, 0);
  for (LONG index = 0; index < count; ++index) {
    if (g_hooks[index].window == window && g_hooks[index].previous != NULL) {
      LRESULT result = CallWindowProcW(
          g_hooks[index].previous,
          window,
          message,
          wparam,
          lparam);
      /*
       * Flutter or a cursor plugin may call SetCursor while processing the
       * movement itself. Reassert after the original window procedure so the
       * local hardware arrow cannot be painted over the remote cursor.
       */
      if (InterlockedCompareExchange(&g_hidden, 0, 0) != 0 &&
          message == WM_MOUSEMOVE) {
        SetCursor(NULL);
      }
      return result;
    }
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

static BOOL CodexHookWindow(HWND window) {
  LONG count = InterlockedCompareExchange(&g_hook_count, 0, 0);
  for (LONG index = 0; index < count; ++index) {
    if (g_hooks[index].window == window) {
      return TRUE;
    }
  }
  if (count >= CODEX_MAX_HOOKS) {
    return FALSE;
  }

  SetLastError(ERROR_SUCCESS);
  WNDPROC previous =
      (WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC);
  if (previous == NULL && GetLastError() != ERROR_SUCCESS) {
    return FALSE;
  }

  g_hooks[count].window = window;
  g_hooks[count].previous = previous;
  InterlockedExchange(&g_hook_count, count + 1);

  SetLastError(ERROR_SUCCESS);
  LONG_PTR replaced = SetWindowLongPtrW(
      window,
      GWLP_WNDPROC,
      (LONG_PTR)CodexCursorGuardProc);
  if (replaced == 0 && GetLastError() != ERROR_SUCCESS) {
    InterlockedExchange(&g_hook_count, count);
    g_hooks[count].window = NULL;
    g_hooks[count].previous = NULL;
    return FALSE;
  }
  return TRUE;
}

static BOOL CALLBACK CodexHookChildWindow(HWND window, LPARAM lparam) {
  (void)lparam;
  DWORD window_process = 0;
  GetWindowThreadProcessId(window, &window_process);
  if (window_process == GetCurrentProcessId()) {
    CodexHookWindow(window);
  }
  return TRUE;
}

static BOOL CALLBACK CodexHookTopLevelWindow(HWND window, LPARAM lparam) {
  (void)lparam;
  DWORD window_process = 0;
  GetWindowThreadProcessId(window, &window_process);
  if (window_process != GetCurrentProcessId() || !IsWindowVisible(window)) {
    return TRUE;
  }

  CodexHookWindow(window);
  EnumChildWindows(window, CodexHookChildWindow, 0);
  return TRUE;
}

static void CodexRestoreHooks(void) {
  LONG count = InterlockedCompareExchange(&g_hook_count, 0, 0);
  for (LONG index = count - 1; index >= 0; --index) {
    HWND window = g_hooks[index].window;
    WNDPROC previous = g_hooks[index].previous;
    if (window != NULL && previous != NULL && IsWindow(window)) {
      WNDPROC current =
          (WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC);
      if (current == CodexCursorGuardProc) {
        SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)previous);
      }
    }
    g_hooks[index].window = NULL;
    g_hooks[index].previous = NULL;
  }
  InterlockedExchange(&g_hook_count, 0);
}

__declspec(dllexport) int __cdecl codex_cursor_guard_set_hidden(int hidden) {
  if (hidden != 0) {
    BOOL first_hide =
        InterlockedCompareExchange(&g_hidden, 1, 0) == 0;
    if (first_hide) {
      InterlockedExchange(&g_hook_count, 0);
    }
    /*
     * Fullscreen Flutter can create or replace child HWNDs after relative
     * mode starts. Re-enumerate on every acquire/reassert so WM_SETCURSOR is
     * owned for those late windows as well.
     */
    EnumWindows(CodexHookTopLevelWindow, 0);
    HCURSOR previous_cursor = SetCursor(NULL);
    if (first_hide) {
      g_previous_cursor = previous_cursor;
    }
    return (int)InterlockedCompareExchange(&g_hook_count, 0, 0);
  }

  if (InterlockedCompareExchange(&g_hidden, 0, 1) != 0) {
    CodexRestoreHooks();
    SetCursor(g_previous_cursor);
    g_previous_cursor = NULL;
  }
  return 0;
}

__declspec(dllexport) int __cdecl codex_cursor_guard_hook_count(void) {
  return (int)InterlockedCompareExchange(&g_hook_count, 0, 0);
}
