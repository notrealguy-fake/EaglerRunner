// main.cpp - EaglerRunner
//
// A tiny, fast Windows shell that plays Eaglercraft-style HTML clients from the
// local disk. It embeds the WebView2 runtime (already present on Windows),
// exposes unlimited TCP/UDP sockets to the page, and keeps a persistent library
// of saved clients so the original .html file can be deleted afterwards.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <commdlg.h>
#include <dbghelp.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <string>
#include <unordered_map>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "library.h"
#include "net_bridge.h"
#include "settings.h"
#include "shim.h"
#include "util.h"
#include "downloader.h"

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

enum CommandId {
  ID_FILE_OPEN = 1001,
  ID_FILE_SAVE_LIBRARY,
  ID_LAUNCHER_HOME,
  ID_RELOAD,
  ID_EXIT,
  ID_VIEW_LIBRARY_FOLDER,
  ID_VIEW_ZOOM_IN,
  ID_VIEW_ZOOM_OUT,
  ID_VIEW_ZOOM_RESET,
  ID_VIEW_FULLSCREEN,
  ID_VIEW_LOW_END,
  ID_VIEW_DEVTOOLS,
  ID_HELP_ABOUT,
  ID_HELP_NETWORK,
  ID_RECENT_FIRST = 2000,
  ID_RECENT_LAST = 2199,
  ID_LIBRARY_FIRST = 3000,
  ID_LIBRARY_LAST = 3199,
};

constexpr UINT WM_APP_OFFER_LIBRARY = WM_APP + 2;
constexpr UINT WM_APP_DOWNLOADER_MANIFEST = WM_APP + 10;
constexpr UINT WM_APP_DOWNLOADER_PROGRESS = WM_APP + 11;
constexpr UINT WM_APP_DOWNLOADER_DONE = WM_APP + 12;
constexpr int ID_DOWNLOAD_EAGLERCRAFT = 3200;

const wchar_t* kWndClass = L"EaglerRunnerMain";
const wchar_t* kPromptClass = L"EaglerRunnerPrompt";
constexpr int kAppIconResource = 101;  // keep in sync with src/app.rc

std::string EscapeForJs(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    if (c == '\n' || c == '\r' || c == '\t') {
      out.push_back(' ');
      continue;
    }
    out.push_back(c);
  }
  return out;
}

// Strings that travel from worker threads to the UI thread inside a PostMessage
// payload are handed over through global memory (the standard Win32 pattern).
LPARAM MakeGlobalString(const std::string& text) {
  HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
  if (!block) return 0;
  void* data = GlobalLock(block);
  if (!data) {
    GlobalFree(block);
    return 0;
  }
  memcpy(data, text.data(), text.size() + 1);
  GlobalUnlock(block);
  return (LPARAM)block;
}

std::string TakeGlobalString(LPARAM lParam) {
  std::string out;
  if (!lParam) return out;
  HGLOBAL block = (HGLOBAL)lParam;
  if (const char* data = (const char*)GlobalLock(block)) {
    out.assign(data);
    GlobalUnlock(block);
  }
  GlobalFree(block);
  return out;
}

// ------------------------------------------------------------- crash reports
//
// Writes %LOCALAPPDATA%\EaglerRunner\crash.log (with a stack trace in
// "module + offset" form) and crash.dmp whenever the app is about to die.
// Offsets can be resolved with the linker map shipped next to the exe
// (build\Release\EaglerRunner.map).

void WriteCrashArtifacts(const char* reason, EXCEPTION_POINTERS* info) {
  std::wstring dir = util::DataDir();
  util::EnsureDir(dir);

  std::string log;
  {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char header[256];
    snprintf(header, sizeof(header),
             "EaglerRunner crash report\nwhen: %04d-%02d-%02d %02d:%02d:%02d\nreason: %s\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, reason);
    log += header;
  }

  HMODULE self = GetModuleHandleW(nullptr);
  if (info && info->ExceptionRecord) {
    char buf[256];
    snprintf(buf, sizeof(buf), "exception: 0x%08X at +0x%llX\n",
             (unsigned)info->ExceptionRecord->ExceptionCode,
             (unsigned long long)((uintptr_t)info->ExceptionRecord->ExceptionAddress -
                                  (uintptr_t)self));
    log += buf;
  }

  void* frames[48] = {0};
  USHORT count = CaptureStackBackTrace(1, 48, frames, nullptr);
  log += "stack:\n";
  for (USHORT i = 0; i < count; ++i) {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)frames[i], &mod);
    char modPath[MAX_PATH] = {0};
    if (mod) GetModuleFileNameA(mod, modPath, MAX_PATH);
    const char* slash = strrchr(modPath, '\\');
    char buf[512];
    snprintf(buf, sizeof(buf), "  #%02u %s + 0x%llX\n", (unsigned)i,
             slash ? slash + 1 : (modPath[0] ? modPath : "?"),
             (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)mod));
    log += buf;
  }
  util::WriteFileUtf8(util::JoinPath(dir, L"crash.log"), log);

  std::wstring dmpPath = util::JoinPath(dir, L"crash.dmp");
  HANDLE file = CreateFileW(dmpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    MINIDUMP_EXCEPTION_INFORMATION* meiPtr = nullptr;
    if (info && info->ExceptionRecord) {
      mei.ThreadId = GetCurrentThreadId();
      mei.ExceptionPointers = info;
      mei.ClientPointers = FALSE;
      meiPtr = &mei;
    }
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, meiPtr,
                      nullptr, nullptr);
    CloseHandle(file);
  }
}

LONG WINAPI UnhandledCrashFilter(EXCEPTION_POINTERS* info) {
  WriteCrashArtifacts("unhandled exception", info);
  return EXCEPTION_EXECUTE_HANDLER;
}

void OnTerminate() {
  // std::terminate ends in a fast-fail which bypasses SEH filters, so the
  // stack has to be captured here.
  WriteCrashArtifacts("std::terminate", nullptr);
  _exit(3);
}

std::string PathToFileUrl(const std::wstring& path) {
  std::string utf8 = util::WideToUtf8(path);
  std::string out = "file:///";
  for (unsigned char c : utf8) {
    if (c == '\\') {
      out.push_back('/');
    } else if (c == ' ' || c == '#' || c == '?' || c == '%' || c == '[' || c == ']' || c > 0x7E) {
      char buf[8];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    } else {
      out.push_back((char)c);
    }
  }
  return out;
}

// -------------------------------------------------------------- name prompt ---

std::wstring g_promptValue;
bool g_promptAccepted = false;
HFONT g_uiFont = nullptr;
HBRUSH g_bgBrush = nullptr;

HFONT UiFont() {
  if (g_uiFont) return g_uiFont;
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
    g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
  }
  if (!g_uiFont) g_uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  return g_uiFont;
}

HBRUSH BgBrush() {
  if (!g_bgBrush) g_bgBrush = CreateSolidBrush(RGB(56, 56, 56));
  return g_bgBrush;
}

LRESULT CALLBACK NamePromptProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  static HWND edit = nullptr;
  switch (msg) {
    case WM_CREATE: {
      const wchar_t* initial = (const wchar_t*)((CREATESTRUCTW*)lParam)->lpCreateParams;
      HWND label = CreateWindowExW(0, L"STATIC", L"Name for this client:", WS_CHILD | WS_VISIBLE,
                                   16, 14, 320, 18, hwnd, nullptr, nullptr, nullptr);
      edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial ? initial : L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 16, 36, 320, 24,
                             hwnd, (HMENU)100, nullptr, nullptr);
      HWND ok = CreateWindowExW(0, L"BUTTON", L"Save",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 190, 72, 70,
                                26, hwnd, (HMENU)IDOK, nullptr, nullptr);
      HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    266, 72, 70, 26, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
      for (HWND h : {label, edit, ok, cancel}) SendMessageW(h, WM_SETFONT, (WPARAM)UiFont(), TRUE);
      SendMessageW(edit, EM_SETSEL, 0, -1);
      SetFocus(edit);
      return 0;
    }
    case WM_COMMAND: {
      if (LOWORD(wParam) == IDOK) {
        wchar_t buffer[256] = {0};
        GetWindowTextW(edit, buffer, 256);
        std::wstring value = util::SanitizeFileName(buffer);
        if (value.empty()) {
          MessageBoxW(hwnd, L"Please type a name first.", L"EaglerRunner", MB_OK | MB_ICONINFORMATION);
          SetFocus(edit);
          return 0;
        }
        g_promptValue = value;
        g_promptAccepted = true;
        DestroyWindow(hwnd);
      } else if (LOWORD(wParam) == IDCANCEL) {
        DestroyWindow(hwnd);
      }
      return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
      HDC dc = (HDC)wParam;
      SetTextColor(dc, RGB(230, 230, 230));
      SetBkColor(dc, msg == WM_CTLCOLOREDIT ? RGB(32, 32, 32) : RGB(56, 56, 56));
      return (LRESULT)BgBrush();
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::wstring PromptForName(HWND parent, const std::wstring& initial) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = NamePromptProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = BgBrush();
    wc.lpszClassName = kPromptClass;
    RegisterClassW(&wc);
    registered = true;
  }

  RECT pr{0, 0, 0, 0};
  GetWindowRect(parent, &pr);
  int width = 352, height = 146;
  int x = pr.left + ((pr.right - pr.left) - width) / 2;
  int y = pr.top + ((pr.bottom - pr.top) - height) / 2;

  g_promptValue.clear();
  g_promptAccepted = false;

  HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPromptClass, L"Save to Launcher",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, parent,
                              nullptr, GetModuleHandleW(nullptr), (LPVOID)initial.c_str());
  if (!hwnd) return L"";

  EnableWindow(parent, FALSE);
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  MSG msg;
  while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0)) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  EnableWindow(parent, TRUE);
  SetActiveWindow(parent);
  SetForegroundWindow(parent);
  return g_promptAccepted ? g_promptValue : L"";
}

}  // namespace

// ------------------------------------------------------------------ the app ---

class App {
 public:
  static App& Get() {
    static App instance;
    return instance;
  }

  bool Create(HINSTANCE instance, const std::wstring& initialFile) {
    m_instance = instance;
    m_settings.Load();
    m_pendingOpen = initialFile;
    m_quietOpen = !initialFile.empty();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &App::WndProcThunk;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, (LPCWSTR)MAKEINTRESOURCEW(kAppIconResource));
    wc.hIconSm = (HICON)LoadImageW(instance, (LPCWSTR)MAKEINTRESOURCEW(kAppIconResource), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 24));
    wc.lpszClassName = kWndClass;
    wc.lpszMenuName = nullptr;
    if (!RegisterClassExW(&wc)) return false;

    int width = m_settings.winW;
    int height = m_settings.winH;
    int x = m_settings.winX;
    int y = m_settings.winY;
    if (x < 0 || y < 0) {
      RECT work{};
      SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
      x = work.left + ((work.right - work.left) - width) / 2;
      y = work.top + ((work.bottom - work.top) - height) / 2;
    }

    m_hwnd = CreateWindowExW(0, kWndClass, L"EaglerRunner", WS_OVERLAPPEDWINDOW,
                             x, y, width, height, nullptr, nullptr, instance, this);
    if (!m_hwnd) return false;

    ApplyDarkTitleBar();
    DragAcceptFiles(m_hwnd, TRUE);
    m_net.Start(m_hwnd, NetBridge::kNotifyMessage);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    StartWebView();
    return true;
  }

  int Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
  }

 private:
  // ------------------------------------------------------------- window glue
  static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
      CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
      self = (App*)cs->lpCreateParams;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
      self->m_hwnd = hwnd;
    } else {
      self = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->WndProc(msg, wParam, lParam);
  }

  LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
      case WM_CREATE:
        RebuildMenus();
        return 0;

      case WM_SIZE:
        ResizeWebView();
        return 0;

      case WM_MOVE:
        SaveGeometry();
        return 0;

      case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;  // swallow Alt menu beep
        break;

      case WM_APP_OFFER_LIBRARY:
        OfferLibrarySave();
        return 0;

      case NetBridge::kNotifyMessage:
        DrainNetwork();
        return 0;

      case WM_APP_DOWNLOADER_MANIFEST:
        OnDownloaderManifest(lParam);
        return 0;

      case WM_APP_DOWNLOADER_PROGRESS:
        OnDownloaderProgress(lParam);
        return 0;

      case WM_APP_DOWNLOADER_DONE:
        OnDownloaderDone(lParam);
        return 0;

      case WM_DROPFILES: {
        HDROP drop = (HDROP)wParam;
        wchar_t path[MAX_PATH * 2] = {0};
        if (DragQueryFileW(drop, 0, path, MAX_PATH * 2)) OpenPath(path);
        DragFinish(drop);
        return 0;
      }

      case WM_COMMAND:
        OnCommand(LOWORD(wParam));
        return 0;

      case WM_CLOSE:
        SaveGeometry();
        m_settings.Save();
        m_net.Stop();
        if (m_controller) m_controller->Close();
        DestroyWindow(m_hwnd);
        return 0;

      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

      case WM_KEYDOWN:
        if (HandleKey((int)wParam)) return 0;
        break;

      default:
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
  }

  bool HandleKey(int vk) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (vk) {
      case VK_F5: Reload(); return true;
      case VK_F6: OpenDownloaderUI(); return true;
      case VK_F11: ToggleFullscreen(); return true;
      case VK_F12: OpenDevTools(); return true;
      case VK_ESCAPE:
        if (m_fullscreen) { ToggleFullscreen(); return true; }
        return false;
      case 'O': if (ctrl) { OpenFileDialog(); return true; } return false;
      case 'S': if (ctrl) { AddCurrentToLibrary(); return true; } return false;
      case 'L': if (ctrl) { LoadLauncher(false); return true; } return false;
      case 'R': if (ctrl) { Reload(); return true; } return false;
      case VK_OEM_PLUS:
      case VK_ADD: if (ctrl) { SetZoom(m_settings.zoomPercent + 10); return true; } return false;
      case VK_OEM_MINUS:
      case VK_SUBTRACT: if (ctrl) { SetZoom(m_settings.zoomPercent - 10); return true; } return false;
      case '0': if (ctrl) { SetZoom(100); return true; } return false;
      default: return false;
    }
  }

  void ApplyDarkTitleBar() {
    BOOL dark = TRUE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (19 on older Windows 10 builds)
    if (FAILED(DwmSetWindowAttribute(m_hwnd, 20, &dark, sizeof(dark)))) {
      DwmSetWindowAttribute(m_hwnd, 19, &dark, sizeof(dark));
    }
  }

  void SaveGeometry() {
    if (m_fullscreen) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(m_hwnd, &wp)) {
      RECT& r = wp.rcNormalPosition;
      m_settings.winX = r.left;
      m_settings.winY = r.top;
      m_settings.winW = r.right - r.left;
      m_settings.winH = r.bottom - r.top;
    }
  }

  // ------------------------------------------------------------------- menus
  void RebuildMenus() {
    HMENU bar = CreateMenu();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_FILE_OPEN, L"&Open HTML File...\tCtrl+O");

    HMENU recents = CreatePopupMenu();
    m_recentIds.clear();
    int recentIndex = 0;
    for (const std::wstring& path : m_settings.recents) {
      int id = ID_RECENT_FIRST + recentIndex;
      if (id > ID_RECENT_LAST) break;
      std::wstring label = util::FileName(path);
      if (!util::FileExists(path)) label += L"  (missing)";
      AppendMenuW(recents, MF_STRING, id, label.c_str());
      m_recentIds[id] = path;
      ++recentIndex;
    }
    if (recentIndex == 0) AppendMenuW(recents, MF_STRING | MF_GRAYED, 0, L"(nothing yet)");
    AppendMenuW(file, MF_POPUP, (UINT_PTR)recents, L"Open &Recent");

    HMENU library = CreatePopupMenu();
    m_libraryIds.clear();
    std::vector<LibraryEntry> entries = LibraryList();
    int libraryIndex = 0;
    for (const LibraryEntry& e : entries) {
      int id = ID_LIBRARY_FIRST + libraryIndex;
      if (id > ID_LIBRARY_LAST) break;
      AppendMenuW(library, MF_STRING, id, e.name.c_str());
      m_libraryIds[id] = e.name;
      ++libraryIndex;
    }
    if (libraryIndex == 0) AppendMenuW(library, MF_STRING | MF_GRAYED, 0, L"(library is empty)");
    AppendMenuW(file, MF_POPUP, (UINT_PTR)library, L"&Library");

    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_SAVE_LIBRARY, L"&Save to Launcher...\tCtrl+S");
    AppendMenuW(file, MF_STRING, ID_DOWNLOAD_EAGLERCRAFT, L"&Download Eaglercraft Version...\tF6");
    AppendMenuW(file, MF_STRING, ID_VIEW_LIBRARY_FOLDER, L"Open Library F&older");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_LAUNCHER_HOME, L"Back to &Launcher\tCtrl+L");
    AppendMenuW(file, MF_STRING, ID_RELOAD, L"&Reload\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_EXIT, L"E&xit");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, ID_VIEW_ZOOM_IN, L"Zoom &In\tCtrl++");
    AppendMenuW(view, MF_STRING, ID_VIEW_ZOOM_OUT, L"Zoom &Out\tCtrl+-");
    AppendMenuW(view, MF_STRING, ID_VIEW_ZOOM_RESET, L"&Reset Zoom\tCtrl+0");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, ID_VIEW_FULLSCREEN, L"&Fullscreen\tF11");
    AppendMenuW(view, MF_STRING | (m_settings.lowEnd ? MF_CHECKED : 0), ID_VIEW_LOW_END,
                L"&Potato PC Mode (no GPU)");
    AppendMenuW(view, MF_STRING, ID_VIEW_DEVTOOLS, L"Developer &Tools\tF12");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_HELP_NETWORK, L"&Networking Info");
    AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, L"&About EaglerRunner");

    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");

    HMENU old = GetMenu(m_hwnd);
    SetMenu(m_hwnd, bar);
    if (old) DestroyMenu(old);
    DrawMenuBar(m_hwnd);
  }

  void OnCommand(int id) {
    if (id >= ID_RECENT_FIRST && id <= ID_RECENT_LAST) {
      auto it = m_recentIds.find(id);
      if (it != m_recentIds.end()) OpenPath(it->second);
      return;
    }
    if (id >= ID_LIBRARY_FIRST && id <= ID_LIBRARY_LAST) {
      auto it = m_libraryIds.find(id);
      if (it != m_libraryIds.end()) {
        LibraryEntry entry;
        if (LibraryFind(it->second, entry)) OpenPath(LibraryHtmlPath(entry), true);
      }
      return;
    }
    switch (id) {
      case ID_FILE_OPEN: OpenFileDialog(); break;
      case ID_FILE_SAVE_LIBRARY: AddCurrentToLibrary(); break;
      case ID_DOWNLOAD_EAGLERCRAFT: OpenDownloaderUI(); break;
      case ID_VIEW_LIBRARY_FOLDER:
        util::EnsureDir(LibraryRoot());
        ShellExecuteW(nullptr, L"open", LibraryRoot().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
      case ID_LAUNCHER_HOME: LoadLauncher(false); break;
      case ID_RELOAD: Reload(); break;
      case ID_EXIT: PostMessageW(m_hwnd, WM_CLOSE, 0, 0); break;
      case ID_VIEW_ZOOM_IN: SetZoom(m_settings.zoomPercent + 10); break;
      case ID_VIEW_ZOOM_OUT: SetZoom(m_settings.zoomPercent - 10); break;
      case ID_VIEW_ZOOM_RESET: SetZoom(100); break;
      case ID_VIEW_FULLSCREEN: ToggleFullscreen(); break;
      case ID_VIEW_LOW_END: SetLowEnd(!m_settings.lowEnd); break;
      case ID_VIEW_DEVTOOLS: OpenDevTools(); break;
      case ID_HELP_ABOUT: ShowAbout(); break;
      case ID_HELP_NETWORK: ShowNetworkInfo(); break;
      default: break;
    }
  }

  // ---------------------------------------------------------------- webview
  void StartWebView() {
    std::wstring userData = util::JoinPath(util::DataDir(), L"WebView2");
    util::EnsureDir(userData);

    std::wstring args =
        L"--disable-background-timer-throttling "
        L"--disable-backgrounding-occluded-windows "
        L"--disable-renderer-backgrounding "
        L"--autoplay-policy=no-user-gesture-required "
        L"--disable-features=CalculateNativeWinOcclusion,HardwareMediaKeyHandling ";
    if (m_settings.lowEnd) {
      args +=
          L"--disable-gpu --disable-gpu-compositing --disable-accelerated-2d-canvas "
          L"--disable-accelerated-video-decode --renderer-process-limit=1 ";
    }

    ComPtr<ICoreWebView2EnvironmentOptions> options =
        Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (options) options->put_AdditionalBrowserArguments(args.c_str());

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
              if (FAILED(result) || !environment) {
                ShowWebViewFailure(result);
                return S_OK;
              }
              m_environment = environment;
              environment->CreateCoreWebView2Controller(
                  m_hwnd,
                  Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                      [this](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {
                        if (FAILED(result2) || !controller) {
                          ShowWebViewFailure(result2);
                          return S_OK;
                        }
                        m_controller = controller;
                        m_controller->get_CoreWebView2(&m_webview);
                        if (m_webview) m_webview.As(&m_webview3);
                        ConfigureWebView();
                        ResizeWebView();

                        // First navigation: the file from the command line, or the launcher.
                        if (!m_pendingOpen.empty()) {
                          std::wstring target = m_pendingOpen;
                          m_pendingOpen.clear();
                          OpenPath(target);
                        } else {
                          LoadLauncher(true);
                        }
                        return S_OK;
                      })
                      .Get());
              return S_OK;
            })
            .Get());

    if (FAILED(hr)) ShowWebViewFailure(hr);
  }

  void ConfigureWebView() {
    if (!m_webview) return;

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(m_webview->get_Settings(&settings)) && settings) {
      settings->put_IsStatusBarEnabled(FALSE);
      settings->put_AreDefaultContextMenusEnabled(TRUE);
      settings->put_AreDevToolsEnabled(TRUE);
      settings->put_AreDefaultScriptDialogsEnabled(TRUE);
      settings->put_IsZoomControlEnabled(FALSE);
      // AreBrowserAcceleratorKeysEnabled lives on the versioned interface.
      ComPtr<ICoreWebView2Settings3> settings3;
      if (SUCCEEDED(settings.As(&settings3)) && settings3) {
        settings3->put_AreBrowserAcceleratorKeysEnabled(TRUE);
      }
    }
    if (m_controller) m_controller->put_ZoomFactor(m_settings.zoomPercent / 100.0);

    // The web engine swallows normal key presses, so shortcuts are routed through
    // the accelerator hook instead of relying on WM_KEYDOWN.
    if (m_controller) {
      m_controller->add_AcceleratorKeyPressed(
          Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
              [this](ICoreWebView2Controller*,
                     ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
                if (SUCCEEDED(args->get_KeyEventKind(&kind)) &&
                    (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                     kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)) {
                  UINT vk = 0;
                  if (SUCCEEDED(args->get_VirtualKey(&vk)) && HandleKey((int)vk)) {
                    args->put_Handled(TRUE);
                  }
                }
                return S_OK;
              })
              .Get(),
          nullptr);
    }

    m_webview->AddScriptToExecuteOnDocumentCreated(
        util::Utf8ToWide(shim::kJsBridge).c_str(), nullptr);

    m_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
              LPWSTR raw = nullptr;
              if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                std::wstring message(raw);
                CoTaskMemFree(raw);
                HandleBridgeMessage(message);
              }
              return S_OK;
            })
            .Get(),
        nullptr);

    m_webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
              LPWSTR uri = nullptr;
              if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                args->put_Handled(TRUE);
                ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                CoTaskMemFree(uri);
              }
              return S_OK;
            })
            .Get(),
        nullptr);

    m_webview->add_DocumentTitleChanged(
        Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
            [this](ICoreWebView2* sender, IUnknown*) -> HRESULT {
              LPWSTR title = nullptr;
              if (SUCCEEDED(sender->get_DocumentTitle(&title)) && title) {
                std::wstring text(title);
                CoTaskMemFree(title);
                if (!text.empty()) {
                  if (text == L"EaglerRunner") {
                    SetWindowTextW(m_hwnd, text.c_str());
                  } else {
                    SetWindowTextW(m_hwnd, (text + L" - EaglerRunner").c_str());
                  }
                }
              }
              return S_OK;
            })
            .Get(),
        nullptr);

    m_webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
              PushStateToPage();
              PushLibraryToPage();
              PushRecentsToPage();
              NotifyLauncherLoaded();
              if (m_openDownloadsOnLoad) {
                m_openDownloadsOnLoad = false;
                RunJs("window.eaglerRunner&&window.eaglerRunner._emit(\"openDownloads\",{});");
              }
              return S_OK;
            })
            .Get(),
        nullptr);
  }

  void NotifyLauncherLoaded() {
    // Tell the launcher page whether a client or the launcher itself is showing.
    std::string script =
        std::string("window.eaglerRunner&&window.eaglerRunner._emit(\"view\",{launcher:") +
        (m_currentFile.empty() ? "true" : "false") + "});";
    RunJs(script);
  }

  void ResizeWebView() {
    if (!m_controller) return;
    RECT bounds{};
    GetClientRect(m_hwnd, &bounds);
    m_controller->put_Bounds(bounds);
  }

  void Reload() {
    if (m_webview) m_webview->Reload();
  }

  void OpenDevTools() {
    if (m_webview) m_webview->OpenDevToolsWindow();
  }

  void SetZoom(int percent) {
    if (percent < 25) percent = 25;
    if (percent > 400) percent = 400;
    m_settings.zoomPercent = percent;
    m_settings.Save();
    if (m_controller) m_controller->put_ZoomFactor(percent / 100.0);
    PushStateToPage();
  }

  void ToggleFullscreen() {
    if (!m_fullscreen) {
      GetWindowRect(m_hwnd, &m_savedRect);
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
      GetMonitorInfoW(monitor, &mi);
      SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
      SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                   SWP_FRAMECHANGED);
      m_fullscreen = true;
    } else {
      SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
      SetWindowPos(m_hwnd, nullptr, m_savedRect.left, m_savedRect.top,
                   m_savedRect.right - m_savedRect.left, m_savedRect.bottom - m_savedRect.top,
                   SWP_FRAMECHANGED | SWP_NOZORDER);
      m_fullscreen = false;
    }
    ResizeWebView();
  }

  void SetLowEnd(bool on) {
    if (on == m_settings.lowEnd) return;
    m_settings.lowEnd = on;
    m_settings.Save();
    RebuildMenus();

    int choice = MessageBoxW(
        m_hwnd,
        on ? L"Potato PC Mode turns off GPU rendering. It needs to reload the client.\n\n"
             L"Reload now?"
           : L"GPU rendering will be turned back on.\n\nReload now?",
        L"EaglerRunner", MB_YESNO | MB_ICONQUESTION);
    if (choice == IDYES) {
      std::wstring current = m_currentFile;
      if (m_controller) {
        m_controller->Close();
        m_controller = nullptr;
        m_environment = nullptr;     // <-- ADD: reset environment so new one can create cleanly
        m_webview = nullptr;
        m_webview3 = nullptr;
      }
      m_currentFile = current;
      StartWebView();
      if (!current.empty()) {
        // Wait for the new controller, then reopen the same file.
        m_pendingOpen = current;
        m_currentFile.clear();
      }
    }
    PushStateToPage();
  }

  // ------------------------------------------------------------- navigation
  void LoadLauncher(bool firstRun) {
    m_currentFile.clear();
    m_pendingOpen.clear();
    std::wstring path = util::JoinPath(util::ExeDir(), L"resources\\launcher.html");
    if (util::FileExists(path)) {
      NavigateToLocalFile(path);
    } else if (m_webview) {
      m_webview->NavigateToString(FallbackLauncherHtml().c_str());
    }
    PushStateToPage();
    (void)firstRun;
  }

  void NavigateToLocalFile(const std::wstring& path) {
    if (!m_webview) return;
    std::wstring dir = util::ParentDir(path);
    std::wstring file = util::FileName(path);

    bool mapped = false;
    if (m_webview3) {
      // Serving the folder through a virtual host gives the client a real,
      // persistent origin: localStorage/IndexedDB work, ws:// is not treated as
      // mixed content, and relative assets load normally.
      mapped = SUCCEEDED(m_webview3->SetVirtualHostNameToFolderMapping(
          L"eagler.local", dir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW));
    }
    if (mapped) {
      std::string url = "http://eagler.local/" + util::PercentEncode(util::WideToUtf8(file));
      m_webview->Navigate(util::Utf8ToWide(url).c_str());
    } else {
      m_webview->Navigate(util::Utf8ToWide(PathToFileUrl(path)).c_str());
    }
  }

  void OpenFileDialog() {
    wchar_t file[MAX_PATH * 2] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"HTML files (*.html;*.htm)\0*.html;*.htm\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"Open HTML file";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    OpenPath(file);
  }

  void OpenPath(const std::wstring& path, bool fromLibrary = false) {
    if (!util::FileExists(path)) {
      MessageBoxW(m_hwnd, (L"This file is missing:\n\n" + path).c_str(), L"EaglerRunner",
                  MB_OK | MB_ICONWARNING);
      m_settings.RemoveRecent(path);
      RebuildMenus();
      PushRecentsToPage();
      return;
    }
    m_currentFile = util::Normalize(path);
    m_pendingOpen.clear();
    NavigateToLocalFile(m_currentFile);
    m_settings.AddRecent(m_currentFile);
    RebuildMenus();

    if (!fromLibrary && m_settings.askToSave && !m_quietOpen) {
      PostMessageW(m_hwnd, WM_APP_OFFER_LIBRARY, 0, 0);
    }
    m_quietOpen = false;
  }

  // ---------------------------------------------------------------- library
  void OfferLibrarySave() {
    if (m_currentFile.empty()) return;
    std::wstring fileName = util::FileName(m_currentFile);
    std::wstring text = L"Save \"" + fileName +
                        L"\" to your launcher library?\n\n"
                        L"A copy is stored inside EaglerRunner, so this client keeps working "
                        L"even if you delete or move the original file.";
    if (MessageBoxW(m_hwnd, text.c_str(), L"EaglerRunner", MB_YESNO | MB_ICONQUESTION) == IDYES) {
      AddCurrentToLibrary();
    }
  }

  void AddCurrentToLibrary() {
    if (m_currentFile.empty()) {
      MessageBoxW(m_hwnd, L"Open an HTML client first, then save it to the library.",
                  L"EaglerRunner", MB_OK | MB_ICONINFORMATION);
      return;
    }
    std::wstring suggested = util::FileStem(m_currentFile);
    std::wstring name = PromptForName(m_hwnd, suggested);
    if (name.empty()) return;

    LibraryEntry entry;
    std::wstring error;
    if (!LibrarySave(m_currentFile, name, error, &entry)) {
      MessageBoxW(m_hwnd, error.c_str(), L"EaglerRunner", MB_OK | MB_ICONERROR);
      return;
    }

    RebuildMenus();
    PushLibraryToPage();

    std::wstring message = L"\"" + entry.name + L"\" is now in your library (" + entry.sourceSize +
                           L").\n\nIt stays available even if the original file is deleted.\n\n"
                           L"Open the saved copy now?";
    if (MessageBoxW(m_hwnd, message.c_str(), L"EaglerRunner", MB_YESNO | MB_ICONINFORMATION) ==
        IDYES) {
      OpenPath(LibraryHtmlPath(entry), true);
    }
  }

  void DeleteLibraryEntry(const std::wstring& name) {
    LibraryEntry entry;
    if (!LibraryFind(name, entry)) return;
    std::wstring text = L"Delete \"" + entry.name + L"\" from the library?\n\nThis removes the saved copy.";
    if (MessageBoxW(m_hwnd, text.c_str(), L"EaglerRunner", MB_YESNO | MB_ICONWARNING) != IDYES) {
      return;
    }
    LibraryDelete(name);
    RebuildMenus();
    PushLibraryToPage();
  }

  // ------------------------------------------------------------- page bridge
  void HandleBridgeMessage(const std::wstring& message) {
    std::string utf8 = util::WideToUtf8(message);
    if (utf8.rfind("app.", 0) == 0) {
      HandleAppCommand(utf8.substr(4));
    } else if (utf8.rfind("downloader.", 0) == 0) {
      // eaglerDownloader.* posts bare "downloader.<verb>|..." lines.
      HandleAppCommand(utf8);
    } else {
      m_net.Send(utf8);
    }
  }

  void HandleAppCommand(const std::string& line) {
    std::vector<std::string> parts = util::Split(line, '|');
    if (parts.empty()) return;
    const std::string& command = parts[0];
    auto arg = [&](size_t index) -> std::wstring {
      if (index >= parts.size()) return L"";
      return util::Utf8ToWide(util::PercentDecode(parts[index]));
    };
    auto argStr = [&](size_t index) -> std::string {
      if (index >= parts.size()) return "";
      return util::PercentDecode(parts[index]);
    };

    if (command == "openDialog") {
      OpenFileDialog();
    } else if (command == "open") {
      OpenPath(arg(1));
    } else if (command == "openLibrary") {
      LibraryEntry entry;
      if (LibraryFind(arg(1), entry)) OpenPath(LibraryHtmlPath(entry), true);
    } else if (command == "deleteLibrary") {
      DeleteLibraryEntry(arg(1));
    } else if (command == "addToLibrary") {
      AddCurrentToLibrary();
    } else if (command == "launcher") {
      LoadLauncher(false);
    } else if (command == "reload") {
      Reload();
    } else if (command == "fullscreen") {
      ToggleFullscreen();
    } else if (command == "openFolder") {
      util::EnsureDir(LibraryRoot());
      ShellExecuteW(nullptr, L"open", LibraryRoot().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (command == "setLowEnd") {
      SetLowEnd(arg(1) == L"1");
    } else if (command == "setAsk") {
      m_settings.askToSave = (arg(1) == L"1");
      m_settings.Save();
      PushStateToPage();
    } else if (command == "setTheme") {
      m_settings.theme = (arg(1) == L"light") ? L"light" : L"dark";
      m_settings.Save();
      PushStateToPage();
    } else if (command == "refresh") {
      PushStateToPage();
      PushLibraryToPage();
      PushRecentsToPage();
    } else if (command == "forgetRecent") {
      m_settings.RemoveRecent(arg(1));
      RebuildMenus();
      PushRecentsToPage();
    } else if (command == "exit") {
      PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    } else if (command == "downloader.manifest") {
      HandleDownloaderManifest(argStr(1));
    } else if (command == "downloader.download") {
      HandleDownloaderDownload(argStr(1), argStr(2));
    }
  }

  // Opens the in-launcher "Get Eaglercraft" dialog. When a client is currently
  // playing, first hop back to the launcher page and open it there.
  void OpenDownloaderUI() {
    if (!m_webview) {
      m_openDownloadsOnLoad = true;
      return;
    }
    if (m_currentFile.empty()) {
      RunJs("window.eaglerRunner&&window.eaglerRunner._emit(\"openDownloads\",{});");
    } else {
      m_openDownloadsOnLoad = true;
      LoadLauncher(false);
    }
  }

  void HandleDownloaderManifest(const std::string& requestId) {
    m_downloader.FetchManifest([this, requestId](const std::vector<DownloaderVersion>& versions,
                                                  const std::string& error) {
      std::string json = "{\"id\":\"" + util::JsonEscape(requestId) + "\",\"versions\":[";
      for (size_t i = 0; i < versions.size(); ++i) {
        const auto& v = versions[i];
        if (i) json += ",";
        json += "{";
        json += "\"id\":\"" + util::JsonEscape(v.id) + "\",";
        json += "\"displayName\":\"" + util::JsonEscape(v.displayName) + "\",";
        json += "\"mcVersion\":\"" + util::JsonEscape(v.mcVersion) + "\",";
        json += "\"type\":\"" + util::JsonEscape(v.type) + "\",";
        json += "\"url\":\"" + util::JsonEscape(v.url) + "\",";
        json += "\"size\":\"" + util::JsonEscape(v.size) + "\"";
        json += "}";
      }
      json += "]";
      if (!error.empty()) json += ",\"error\":\"" + util::JsonEscape(error) + "\"";
      json += "}";
      if (LPARAM payload = MakeGlobalString(json)) {
        PostMessageW(m_hwnd, WM_APP_DOWNLOADER_MANIFEST, 0, payload);
      }
    });
  }

  void HandleDownloaderDownload(const std::string& requestId, const std::string& versionId) {
    m_downloader.DownloadVersion(versionId, [this, requestId](const DownloadProgress& prog) {
      std::string json = "{\"id\":\"" + util::JsonEscape(requestId) + "\"";
      json += ",\"version\":\"" + util::JsonEscape(prog.id) + "\"";
      json += ",\"percent\":" + std::to_string(prog.percent);
      json += ",\"status\":\"" + util::JsonEscape(prog.status) + "\"";
      if (!prog.error.empty()) json += ",\"error\":\"" + util::JsonEscape(prog.error) + "\"";
      if (!prog.resultPath.empty()) json += ",\"resultPath\":\"" + util::JsonEscape(prog.resultPath) + "\"";
      json += "}";

      bool finished =
          prog.status == "done" || prog.status == "error" || prog.status == "downloaded";
      LPARAM payload = MakeGlobalString(json);
      if (!payload) return;
      PostMessageW(m_hwnd, finished ? WM_APP_DOWNLOADER_DONE : WM_APP_DOWNLOADER_PROGRESS, 0,
                   payload);
    });
  }

  void OnDownloaderManifest(LPARAM lParam) {
    std::string json = TakeGlobalString(lParam);
    if (json.empty()) return;
    RunJs("window.eaglerDownloader&&window.eaglerDownloader._emit(\"manifest\"," + json + ");");
  }

  void OnDownloaderProgress(LPARAM lParam) {
    std::string json = TakeGlobalString(lParam);
    if (json.empty()) return;
    RunJs("window.eaglerDownloader&&window.eaglerDownloader._emit(\"progress\"," + json + ");");
  }

  void OnDownloaderDone(LPARAM lParam) {
    std::string json = TakeGlobalString(lParam);
    if (json.empty()) return;
    RunJs("window.eaglerDownloader&&window.eaglerDownloader._emit(\"done\"," + json + ");");
  }

  void RunJs(const std::string& script) {
    if (!m_webview || script.empty()) return;
    m_webview->ExecuteScript(util::Utf8ToWide(script).c_str(), nullptr);
  }

  void DrainNetwork() {
    std::string batch = m_net.Drain();
    if (batch.empty()) return;
    std::string script;
    int count = 0;
    for (const std::string& line : util::Split(batch, '\n')) {
      if (line.empty()) continue;
      script += "window.eaglerNet&&window.eaglerNet._dispatch(\"";
      script += EscapeForJs(line);
      script += "\");";
      if (++count >= 250) {
        RunJs(script);
        script.clear();
        count = 0;
      }
    }
    if (!script.empty()) RunJs(script);
  }

  void PushStateToPage() {
    std::string json = "{";
    json += "\"file\":\"" + util::JsonEscape(util::WideToUtf8(m_currentFile)) + "\"";
    json += ",\"fileName\":\"" + util::JsonEscape(util::WideToUtf8(util::FileName(m_currentFile))) + "\"";
    json += std::string(",\"launcher\":") + (m_currentFile.empty() ? "true" : "false");
    json += std::string(",\"lowEnd\":") + (m_settings.lowEnd ? "true" : "false");
    json += std::string(",\"askToSave\":") + (m_settings.askToSave ? "true" : "false");
    json += std::string(",\"showStatusBar\":") + (m_settings.showStatusBar ? "true" : "false");
    json += ",\"theme\":\"" + util::JsonEscape(util::WideToUtf8(m_settings.theme)) + "\"";
    json += ",\"zoom\":" + std::to_string(m_settings.zoomPercent);
    json += ",\"connections\":" + std::to_string(m_net.OpenConnectionCount());
    json += ",\"netReady\":" + std::string(m_net.Running() ? "true" : "false");
    json += ",\"libraryRoot\":\"" + util::JsonEscape(util::WideToUtf8(LibraryRoot())) + "\"";
    json += "}";
    RunJs("window.eaglerRunner&&window.eaglerRunner._emit(\"state\"," + json + ");");
  }

  void PushLibraryToPage() {
    std::string json = "[";
    bool first = true;
    for (const LibraryEntry& e : LibraryList()) {
      if (!first) json += ",";
      first = false;
      json += "{";
      json += "\"name\":\"" + util::JsonEscape(util::WideToUtf8(e.name)) + "\"";
      json += ",\"html\":\"" + util::JsonEscape(util::WideToUtf8(LibraryHtmlPath(e))) + "\"";
      json += ",\"added\":\"" + util::JsonEscape(util::WideToUtf8(e.added)) + "\"";
      json += ",\"size\":\"" + util::JsonEscape(util::WideToUtf8(e.sourceSize)) + "\"";
      json += ",\"from\":\"" + util::JsonEscape(util::WideToUtf8(e.origPath)) + "\"";
      json += std::string(",\"present\":") +
              (util::FileExists(LibraryHtmlPath(e)) ? "true" : "false");
      json += "}";
    }
    json += "]";
    RunJs("window.eaglerRunner&&window.eaglerRunner._emit(\"library\"," + json + ");");
  }

  void PushRecentsToPage() {
    std::string json = "[";
    bool first = true;
    for (const std::wstring& path : m_settings.recents) {
      if (!first) json += ",";
      first = false;
      json += "{";
      json += "\"path\":\"" + util::JsonEscape(util::WideToUtf8(path)) + "\"";
      json += ",\"name\":\"" + util::JsonEscape(util::WideToUtf8(util::FileName(path))) + "\"";
      json += std::string(",\"present\":") + (util::FileExists(path) ? "true" : "false");
      json += "}";
    }
    json += "]";
    RunJs("window.eaglerRunner&&window.eaglerRunner._emit(\"recents\"," + json + ");");
  }

  // ------------------------------------------------------------------ dialogs
  void ShowAbout() {
    std::wstring text =
        L"EaglerRunner\n\n"
        L"A lightweight C++ shell that plays Eaglercraft-style HTML clients on Windows.\n\n"
        L"- WebView2 rendering (fast, low memory, works on old PCs)\n"
        L"- Pulls in HTML files directly from disk\n"
        L"- Unrestricted WebSocket, TCP and UDP networking\n"
        L"- Persistent client library (survives deleting the original file)\n"
        L"- Potato PC Mode: software rendering for very weak machines\n\n"
        L"Shortcuts: Ctrl+O open, Ctrl+S save to library, Ctrl+L launcher, F5 reload, "
        L"F11 fullscreen, F12 dev tools, Ctrl +/- zoom.";
    MessageBoxW(m_hwnd, text.c_str(), L"About EaglerRunner", MB_OK | MB_ICONINFORMATION);
  }

  void ShowNetworkInfo() {
    std::wstring text =
        L"Networking\n\n"
        L"Nothing is proxied, filtered or blocked.\n\n"
        L"WebSocket (ws://, wss://) runs directly in the WebView2 engine - the same "
        L"connections a normal browser makes, including to localhost.\n\n"
        L"Raw TCP and UDP are additionally exposed to any loaded page through "
        L"JavaScript:\n\n"
        L"  window.eaglerNet.tcp.connect(host, port)   -> id\n"
        L"  window.eaglerNet.tcp.send(id, data)\n"
        L"  window.eaglerNet.tcp.close(id)\n"
        L"  window.eaglerNet.udp.open(bindPort)        -> id\n"
        L"  window.eaglerNet.udp.send(id, host, port, data)\n"
        L"  window.eaglerNet.udp.close(id)\n\n"
        L"  window.eaglerNet.on('open' | 'data' | 'close' | 'error', fn)\n\n"
        L"Binary data is transferred as Uint8Array/ArrayBuffer. IPv4 and IPv6 are "
        L"both supported.";
    MessageBoxW(m_hwnd, text.c_str(), L"Networking", MB_OK | MB_ICONINFORMATION);
  }

  void ShowWebViewFailure(HRESULT hr) {
    wchar_t buffer[512];
    swprintf(buffer, 512,
             L"Could not start the web engine (WebView2).\n\n"
             L"HRESULT: 0x%08X\n\n"
             L"Install the free \"Microsoft Edge WebView2 Runtime\" and try again.",
             (unsigned)hr);
    MessageBoxW(m_hwnd, buffer, L"EaglerRunner", MB_OK | MB_ICONERROR);
  }

  std::wstring FallbackLauncherHtml() {
    return L"<!doctype html><html><head><meta charset=\"utf-8\"><title>EaglerRunner</title>"
           L"<style>body{background:#1d1d1d;color:#e8e8e8;font-family:Segoe UI,sans-serif;"
           L"display:flex;align-items:center;justify-content:center;height:100vh;margin:0;"
           L"flex-direction:column;gap:16px}h1{margin:0}button{padding:12px 22px;font-size:15px;"
           L"background:#5a5a5a;color:#fff;border:2px solid #000;cursor:pointer}"
           L"button:hover{background:#3c8527}</style></head><body>"
           L"<h1>EaglerRunner</h1>"
           L"<p>The launcher resources folder is missing, but you can still play.</p>"
           L"<button onclick=\"window.eaglerRunner.openFileDialog()\">Open HTML File</button>"
           L"</body></html>";
  }

  HINSTANCE m_instance = nullptr;
  HWND m_hwnd = nullptr;
  ComPtr<ICoreWebView2Environment> m_environment;
  ComPtr<ICoreWebView2Controller> m_controller;
  ComPtr<ICoreWebView2> m_webview;
  ComPtr<ICoreWebView2_3> m_webview3;

  NetBridge m_net;
  Settings m_settings;
  Downloader m_downloader;

  std::wstring m_currentFile;   // empty -> launcher is showing
  std::wstring m_pendingOpen;

  bool m_fullscreen = false;
  bool m_openDownloadsOnLoad = false;
  bool m_quietOpen = false;  // launched from the command line: no library prompt
  RECT m_savedRect{};

  std::unordered_map<int, std::wstring> m_recentIds;
  std::unordered_map<int, std::wstring> m_libraryIds;
};

int RunApplication(HINSTANCE instance, const std::wstring& initialFile) {
  SetUnhandledExceptionFilter(UnhandledCrashFilter);
  std::set_terminate(OnTerminate);

  HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com) && com != RPC_E_CHANGED_MODE) return 1;

  // Per-monitor DPI awareness (optional, ignored on very old Windows).
  if (HMODULE user32 = LoadLibraryW(L"user32.dll")) {
    using SetDpiFn = BOOL(WINAPI*)(HANDLE);
    auto setDpi = (SetDpiFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (setDpi) {
      const HANDLE kPerMonitorV2 = (HANDLE)-4;
      setDpi(kPerMonitorV2);
    }
  }

  {
    App& app = App::Get();
    if (!app.Create(instance, initialFile)) {
      MessageBoxW(nullptr, L"EaglerRunner could not create its window.", L"EaglerRunner",
                  MB_OK | MB_ICONERROR);
      CoUninitialize();
      return 1;
    }
    int result = app.Run();
    CoUninitialize();
    return result;
  }
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
  std::wstring initialFile;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv) {
    if (argc > 1) {
      std::wstring candidate = argv[1];
      if (util::FileExists(candidate)) initialFile = util::Normalize(candidate);
    }
    LocalFree(argv);
  }
  return RunApplication(instance, initialFile);
}
