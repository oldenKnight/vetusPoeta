// VetusPoeta.exe: the Win32 window hosting WebView2 (the UI is served from the ui\ folder next to
// the exe at https://app.vetuspoeta/ through a virtual host mapping), routing JSON messages between
// the UI and vpengine.exe, answering the shell-handled commands, and supervising the engine
// (watchdog ping, restart, settings replay, project.recover). DESIGN.md §9, §13; PREDESIGN 4.8.
#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine_host.h"
#include "host_logic.h"
#include "shell_util.h"
#include "webview2_api.h"

namespace vp {
namespace shell {
namespace {

constexpr UINT WM_APP_ENGINE_OUTPUT = WM_APP + 1;
constexpr UINT WM_APP_ENGINE_EXITED = WM_APP + 2;
constexpr UINT WM_APP_SHELL_CMD = WM_APP + 3;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickMs = 500;  // watchdog resolution; pings go out every 2 s (host_logic Watchdog)
constexpr DWORD kShutdownWaitMs = 3000;
constexpr wchar_t kWindowClass[] = L"VetusPoeta.MainWindow";
constexpr wchar_t kWindowTitle[] = L"vetus poeta";
constexpr wchar_t kMutexName[] = L"Local\\VetusPoeta.SingleInstance.5b1e9c47";
constexpr wchar_t kEngineExe[] = L"vpengine.exe";
constexpr wchar_t kLoaderDll[] = L"WebView2Loader.dll";
constexpr wchar_t kHostName[] = L"app.vetuspoeta";
constexpr wchar_t kStartUrl[] = L"https://app.vetuspoeta/index.html";
constexpr wchar_t kRuntimeDownloadUrl[] = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";
// Keeps the embedded browser from background traffic of its own (component updates, field
// trials): the app is offline unless the user turns the online check on (project rules).
constexpr wchar_t kBrowserArgs[] = L"--disable-background-networking";
// Injected into every document before the page's own scripts (host scripts are not subject to the
// page CSP). Files dropped anywhere on the window are handed to the shell as File objects, whose
// real paths only the host can read; the shell answers with the dialog.droppedFiles event. It runs
// in the bubble phase on window, after the page's own handlers (vp_start.js cancels the default
// itself); a drop the page ignored is taken here too, so WebView2 never navigates to the file.
// ES5, no globals.
constexpr wchar_t kDropScript[] =
    L"(function () {\n"
    L"  'use strict';\n"
    L"  var wv = window.chrome && window.chrome.webview;\n"
    L"  if (!wv || typeof wv.postMessageWithAdditionalObjects !== 'function' || window.top !== window) { return; }\n"
    L"  function hasFiles(e) {\n"
    L"    var t = e.dataTransfer && e.dataTransfer.types;\n"
    L"    if (!t) { return false; }\n"
    L"    for (var i = 0; i < t.length; i++) { if (t[i] === 'Files') { return true; } }\n"
    L"    return false;\n"
    L"  }\n"
    L"  window.addEventListener('dragover', function (e) {\n"
    L"    if (hasFiles(e) && !e.defaultPrevented) { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; }\n"
    L"  }, false);\n"
    L"  window.addEventListener('drop', function (e) {\n"
    L"    if (!hasFiles(e)) { return; }\n"
    L"    e.preventDefault();\n"
    L"    var files = e.dataTransfer.files;\n"
    L"    if (files && files.length) {\n"
    L"      wv.postMessageWithAdditionalObjects(JSON.stringify({ id: 0, cmd: 'dialog.droppedFiles', params: { emit: true } }), files);\n"
    L"    }\n"
    L"  }, false);\n"
    L"}());\n";
constexpr int kDefaultW = 1280, kDefaultH = 800;  // client area in DIPs
constexpr int kMinW = 1024, kMinH = 640;          // PREDESIGN 6.1
constexpr size_t kMaxShellQueue = 64;
constexpr size_t kMaxPendingOpens = 16;
constexpr UINT32 kMaxDroppedFiles = 256;
constexpr COREWEBVIEW2_COLOR kLightBg{255, 0xFB, 0xF7, 0xF0};  // gui/ui/css/tokens.css --bg (light)
constexpr COREWEBVIEW2_COLOR kDarkBg{255, 0x1B, 0x18, 0x15};   // --bg (dark)
constexpr DWORD kDwmUseImmersiveDarkMode = 20;     // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmUseImmersiveDarkModeOld = 19;  // same, Windows 10 before 20H1

// A COM callback object for the WebView2 handler interfaces, all of which have exactly one
// method, Invoke(A1, A2). COM objects own themselves through their reference count; this is
// the only place in the shell where an object deletes itself.
template <class I, class A1, class A2>
class Handler final : public I {
 public:
  using Fn = std::function<HRESULT(A1, A2)>;
  static ComPtr<I> make(REFIID iid, Fn fn) {
    auto h = std::make_unique<Handler>(iid, std::move(fn));
    return ComPtr<I>(h.release(), false);
  }
  Handler(REFIID iid, Fn fn) : iid_(iid), fn_(std::move(fn)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) { return E_POINTER; }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_)) {
      *ppv = static_cast<I*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --refs_;
    if (n == 0) { std::unique_ptr<Handler> self(this); }
    return n;
  }
  HRESULT STDMETHODCALLTYPE Invoke(A1 a, A2 b) override { return fn_ ? fn_(a, b) : S_OK; }

 private:
  std::atomic<ULONG> refs_{1};
  IID iid_;
  Fn fn_;
};

using EnvCompleted = Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>;
using ControllerCompleted = Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>;
using ScriptAdded = Handler<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, HRESULT, LPCWSTR>;
using MessageReceived = Handler<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>;
using NavigationStarting = Handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>;
using NewWindowRequested = Handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>;
using ProcessFailed = Handler<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>;

std::string utf8Paths(const std::vector<std::wstring>& paths) {
  std::vector<std::string> u;
  u.reserve(paths.size());
  for (const std::wstring& p : paths) { u.push_back(toUtf8(p)); }
  return jsonStringArray(u);
}

// Per-monitor-v2 DPI awareness for the whole process, before any window exists. The manifest asks
// for it too; this call makes it independent of how the manifest was embedded (it fails harmlessly
// when the manifest already set it).
void enablePerMonitorDpiAwareness() {
  using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  auto setContext = user32 ? reinterpret_cast<SetContextFn>(reinterpret_cast<void*>(
                                 GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
                           : nullptr;
  if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) { return; }
  if (setContext) { setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE); }
}

UINT windowDpi(HWND h) {
  UINT dpi = h ? GetDpiForWindow(h) : 0;
  return dpi ? dpi : 96;
}

UINT monitorDpi(HMONITOR mon) {
  // GetDpiForMonitor lives in shcore.dll (Windows 8.1+); resolved at run time.
  using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  static UniqueModule shcore(LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
  static GetDpiForMonitorFn fn =
      shcore.get() ? reinterpret_cast<GetDpiForMonitorFn>(
                         reinterpret_cast<void*>(GetProcAddress(shcore.get(), "GetDpiForMonitor")))
                   : nullptr;
  UINT x = 0, y = 0;
  if (fn && mon && SUCCEEDED(fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x) { return x; }
  UINT sys = GetDpiForSystem();
  return sys ? sys : 96;
}

// Non-client size of our window style at `dpi` (caption + borders), in physical pixels.
Size frameSize(UINT dpi) {
  RECT r{0, 0, 0, 0};
  if (!AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) { return Size{}; }
  return Size{int(r.right - r.left), int(r.bottom - r.top)};
}

Size workSize(HMONITOR mon) {
  MONITORINFO mi{};
  mi.cbSize = sizeof mi;
  if (!mon || !GetMonitorInfoW(mon, &mi)) { return Size{}; }
  return Size{int(mi.rcWork.right - mi.rcWork.left), int(mi.rcWork.bottom - mi.rcWork.top)};
}

struct ShellCommand {
  long long id = 0;
  std::string cmd;
  std::string params;
  std::vector<std::wstring> dropped;  // dialog.droppedFiles: real paths of the posted File objects
};

class App {
 public:
  explicit App(const CommandLine& cl) : devtools_(cl.devtools) {
    for (const std::string& f : cl.files) { pendingOpens_.push_back(fullPath(toWide(f))); }
  }
  ~App() = default;
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  bool create(HINSTANCE inst);

 private:
  static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l);
  LRESULT handle(UINT m, WPARAM w, LPARAM l);

  // window
  void restorePlacement(int& x, int& y, int& w, int& h, bool& maximized);
  void savePlacement();
  void rememberNormalRect();
  void resizeWebView();
  void applyTheme();
  void noteSettingsChange();
  void onClose();
  void raise();
  int message(Str text, UINT flags, const std::wstring& extra = std::wstring());

  // WebView2
  bool initWebView();
  HRESULT onEnvironment(HRESULT hr, ICoreWebView2Environment* env);
  HRESULT onController(HRESULT hr, ICoreWebView2Controller* controller);
  void configureWebView();
  void closeWebView();
  void failWebView(const char* what, HRESULT hr);
  HRESULT onWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args);
  HRESULT onNavigationStarting(ICoreWebView2NavigationStartingEventArgs* args);
  HRESULT onNewWindow(ICoreWebView2NewWindowRequestedEventArgs* args);
  HRESULT onProcessFailed(ICoreWebView2ProcessFailedEventArgs* args);
  void postToWeb(const std::string& json);
  void onUiReady();
  void requestOpen(const std::vector<std::wstring>& paths);
  void emitOpen(const std::vector<std::wstring>& paths, const char* source);
  void emitPower(bool force);

  // engine
  void startEngine();
  void forwardToEngine(const std::string& text, const WebRequest& req);
  void onEngineOutput();
  void onShellResponse(const EngineLine& e, const std::string& line);
  void applyStep(const RestartSequence::Step& step);
  void onTick();
  void restartEngine(const char* reason);
  void replyError(long long id, const char* code, const char* msg, Str hint);

  // shell commands
  void runShellCommands();
  void runShellCommand(const ShellCommand& c);
  bool filtersFor(const std::string& params, std::vector<FileFilter>& out);
  bool openFileDialog(const std::vector<FileFilter>& filters, bool multiple, const std::string& title,
                      std::vector<std::wstring>& out, HRESULT& hr);
  bool saveFileDialog(const std::vector<FileFilter>& filters, const std::string& suggested, const std::string& title,
                      std::wstring& out, HRESULT& hr);

  HINSTANCE inst_ = nullptr;
  HWND hwnd_ = nullptr;
  bool devtools_ = false;
  UniqueBrush brush_;
  bool dark_ = false;
  RECT normalRect_{};
  bool haveNormalRect_ = false;
  std::wstring dataDir_, logDir_, uiDir_;
  ShellText text_;
  std::string appliedLang_, appliedTheme_;

  UniqueModule loader_;
  ComPtr<ICoreWebView2Environment> env_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  EventRegistrationToken tokMessage_{}, tokNav_{}, tokNewWin_{}, tokFailed_{};
  int webviewRecreations_ = 0;
  bool uiReady_ = false;
  std::vector<std::wstring> pendingOpens_;
  int lastOnBattery_ = -1;
  std::deque<ShellCommand> shellQueue_;
  bool shellPosted_ = false;
  bool closing_ = false;

  LogFile log_;
  std::unique_ptr<EngineHost> engine_;
  bool engineUnavailable_ = false;
  bool crashLoopStopped_ = false;
  Watchdog watchdog_;
  RestartLimiter limiter_;
  SessionTracker tracker_;
  RestartSequence sequence_;
  bool restarting_ = false;  // replaying settings / recovering the project after a restart
  std::deque<std::string> outBuf_;
};

// ------------------------------------------------------------------------------ window

bool App::create(HINSTANCE inst) {
  inst_ = inst;
  dataDir_ = dataDir();
  logDir_ = dataDir_.empty() ? std::wstring() : dataDir_ + L"\\logs";
  if (!logDir_.empty() && ensureDir(logDir_)) { log_.open(logDir_ + L"\\engine.log"); }
  log_.note(std::string("vetus poeta ") + VP_SHELL_VERSION + " shell starting" + (devtools_ ? " (devtools)" : ""));
  uiDir_ = exeDir() + L"\\ui";

  // The engine owns settings.json; reading it here gives the language and theme before it runs.
  std::string settings;
  if (!dataDir_.empty() && readSmallFile(dataDir_ + L"\\settings.json", settings, size_t(1) << 20)) {
    tracker_.noteSettings(settings);
  }
  appliedLang_ = tracker_.lang().empty() ? std::string(systemIsSpanish() ? "es-MX" : "en-US") : tracker_.lang();
  text_.load(uiDir_, appliedLang_);
  appliedTheme_ = tracker_.theme();

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &App::wndProc;
  wc.hInstance = inst;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // WM_ERASEBKGND paints the theme colour
  wc.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&wc)) { return false; }

  int x = 0, y = 0, w = 0, h = 0;
  bool maximized = false;
  restorePlacement(x, y, w, h, maximized);
  hwnd_ = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, this);
  if (!hwnd_) { return false; }
  normalRect_ = RECT{x, y, x + w, y + h};
  haveNormalRect_ = true;
  applyTheme();
  ShowWindow(hwnd_, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
  UpdateWindow(hwnd_);
  rememberNormalRect();

  engine_ = std::make_unique<EngineHost>(hwnd_, WM_APP_ENGINE_OUTPUT, WM_APP_ENGINE_EXITED, log_);
  startEngine();
  SetTimer(hwnd_, kTickTimer, kTickMs, nullptr);
  if (!dirExists(uiDir_)) {
    log_.note("ui folder missing: " + toUtf8(uiDir_));
    message(Str::UiMissingText, MB_OK | MB_ICONERROR);
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    return true;
  }
  initWebView();  // on failure it has told the user and posted WM_CLOSE (clean engine exit)
  return true;
}

LRESULT CALLBACK App::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
    auto* self = static_cast<App*>(cs->lpCreateParams);
    self->hwnd_ = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (!self) { return DefWindowProcW(h, m, w, l); }
  return self->handle(m, w, l);
}

LRESULT App::handle(UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND: {
      RECT r{};
      GetClientRect(hwnd_, &r);
      if (brush_.get()) { FillRect(reinterpret_cast<HDC>(w), &r, brush_.get()); }
      return 1;
    }
    case WM_SIZE:
      if (controller_) { controller_->put_IsVisible(w == SIZE_MINIMIZED ? FALSE : TRUE); }
      resizeWebView();
      rememberNormalRect();
      return 0;
    case WM_MOVE:
    case WM_MOVING:
      if (controller_) { controller_->NotifyParentWindowPositionChanged(); }
      rememberNormalRect();
      break;
    case WM_SETFOCUS:
      if (controller_) { controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }
      return 0;
    case WM_GETMINMAXINFO: {
      // 1024x640 is a minimum of the CLIENT area in DIPs; the track size is the outer window in
      // physical pixels at this window's DPI, never more than the work area.
      auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
      HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
      Size s = outerSizeForClient(Size{kMinW, kMinH}, windowDpi(hwnd_), frameSize(windowDpi(hwnd_)), workSize(mon));
      mmi->ptMinTrackSize.x = s.w;
      mmi->ptMinTrackSize.y = s.h;
      return 0;
    }
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(l);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      resizeWebView();
      return 0;
    }
    case WM_SETTINGCHANGE:
      if (l && lstrcmpiW(reinterpret_cast<LPCWSTR>(l), L"ImmersiveColorSet") == 0) { applyTheme(); }
      resizeWebView();
      if (controller_) { controller_->NotifyParentWindowPositionChanged(); }
      break;
    case WM_DISPLAYCHANGE:
      resizeWebView();
      if (controller_) { controller_->NotifyParentWindowPositionChanged(); }
      break;
    case WM_POWERBROADCAST:
      if (w == PBT_APMPOWERSTATUSCHANGE) { emitPower(false); }
      return TRUE;
    case WM_COPYDATA: {
      const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(l);
      std::vector<std::string> paths;
      if (!cds || !decodeOpenPayload(uint32_t(cds->dwData), cds->lpData, cds->cbData, paths)) { return FALSE; }
      std::vector<std::wstring> wide;
      for (const std::string& p : paths) { wide.push_back(toWide(p)); }
      raise();
      requestOpen(wide);
      return TRUE;
    }
    case WM_TIMER:
      if (w == kTickTimer) { onTick(); }
      return 0;
    case WM_APP_ENGINE_OUTPUT:
      onEngineOutput();
      return 0;
    case WM_APP_ENGINE_EXITED:
      if (engine_ && unsigned(w) == engine_->generation() && !closing_ && !crashLoopStopped_) {
        onEngineOutput();  // deliver whatever arrived before the exit
        restartEngine("engine process exited");
      }
      return 0;
    case WM_APP_SHELL_CMD:
      runShellCommands();
      return 0;
    case WM_CLOSE:
      onClose();
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_NCDESTROY: {
      LRESULT r = DefWindowProcW(hwnd_, m, w, l);
      SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      hwnd_ = nullptr;
      return r;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd_, m, w, l);
}

void App::restorePlacement(int& x, int& y, int& w, int& h, bool& maximized) {
  POINT origin{0, 0};
  HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  UINT dpi = monitorDpi(primary);
  RECT workArea{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
  Rect work{workArea.left, workArea.top, workArea.right, workArea.bottom};
  Size def = outerSizeForClient(Size{kDefaultW, kDefaultH}, dpi, frameSize(dpi), workSize(primary));
  Placement p = defaultPlacement(work, def.w, def.h);
  std::string json;
  Placement saved;
  if (!dataDir_.empty() && readSmallFile(dataDir_ + L"\\window.json", json, 4096) && placementFromJson(json, saved)) {
    RECT r{saved.x, saved.y, saved.x + saved.w, saved.y + saved.h};
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(mon, &mi)) {
      Rect mw{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom};
      UINT mdpi = monitorDpi(mon);
      Size minOuter = outerSizeForClient(Size{kMinW, kMinH}, mdpi, frameSize(mdpi), workSize(mon));
      p = sanitizePlacement(saved, mw, minOuter.w, minOuter.h);
      p.maximized = saved.maximized;
    }
  }
  x = p.x;
  y = p.y;
  w = p.w;
  h = p.h;
  maximized = p.maximized;
}

void App::rememberNormalRect() {
  if (!hwnd_ || IsIconic(hwnd_) || IsZoomed(hwnd_)) { return; }
  RECT r;
  if (GetWindowRect(hwnd_, &r)) {
    normalRect_ = r;
    haveNormalRect_ = true;
  }
}

void App::savePlacement() {
  if (!hwnd_ || dataDir_.empty() || !haveNormalRect_) { return; }
  Placement p;
  p.x = normalRect_.left;
  p.y = normalRect_.top;
  p.w = normalRect_.right - normalRect_.left;
  p.h = normalRect_.bottom - normalRect_.top;
  p.maximized = IsZoomed(hwnd_) != FALSE;
  if (!writeFileAtomic(dataDir_ + L"\\window.json", placementToJson(p))) { log_.note("could not save window.json"); }
}

void App::resizeWebView() {
  if (!controller_ || !hwnd_) { return; }
  // Bounds are relative to our client area in physical pixels (the default raw-pixel bounds mode);
  // the client rect of a per-monitor-v2 window is exactly that.
  RECT r{};
  if (!GetClientRect(hwnd_, &r)) { return; }
  controller_->put_Bounds(r);
}

void App::applyTheme() {
  dark_ = themeIsDark(tracker_.theme(), systemPrefersDark());
  const COREWEBVIEW2_COLOR& c = dark_ ? kDarkBg : kLightBg;
  brush_.reset(CreateSolidBrush(RGB(c.R, c.G, c.B)));
  if (hwnd_) {
    BOOL on = dark_ ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &on, sizeof on))) {
      DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkModeOld, &on, sizeof on);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
  ComPtr<ICoreWebView2Controller2> c2;
  if (controller_ && SUCCEEDED(controller_.as(IID_ICoreWebView2Controller2, c2))) { c2->put_DefaultBackgroundColor(c); }
}

// A settings result went by (from the UI's own settings.get/set or the restart replay).
void App::noteSettingsChange() {
  if (tracker_.theme() != appliedTheme_) {
    appliedTheme_ = tracker_.theme();
    applyTheme();
  }
  if (!tracker_.lang().empty() && tracker_.lang() != appliedLang_) {
    appliedLang_ = tracker_.lang();
    text_.load(uiDir_, appliedLang_);
  }
}

int App::message(Str text, UINT flags, const std::wstring& extra) {
  std::wstring body = text_.get(text) + extra;
  return MessageBoxW(hwnd_, body.c_str(), kWindowTitle, flags);
}

void App::raise() {
  if (!hwnd_) { return; }
  if (IsIconic(hwnd_)) { ShowWindow(hwnd_, SW_RESTORE); }
  SetForegroundWindow(hwnd_);
}

void App::onClose() {
  if (closing_) { return; }
  closing_ = true;
  savePlacement();
  KillTimer(hwnd_, kTickTimer);
  ShowWindow(hwnd_, SW_HIDE);  // feels instant while the engine gets its clean exit
  if (engine_) {
    // The engine's clean exit autosaves a dirty project and removes its lock file.
    engine_->shutdown(makeRequest(kShutdownId, "engine.shutdown", "{}"), kShutdownWaitMs);
  }
  closeWebView();
  DestroyWindow(hwnd_);
}

// ---------------------------------------------------------------------------- WebView2

bool App::initWebView() {
  VpCreateEnvironmentFn createEnv = nullptr;
  VpGetVersionFn getVersion = nullptr;
#if VP_WEBVIEW2_STATIC
  createEnv = &CreateCoreWebView2EnvironmentWithOptions;
  getVersion = &GetAvailableCoreWebView2BrowserVersionString;
#else
  if (!loader_.get()) {
    std::wstring dll = exeDir() + L"\\" + kLoaderDll;
    loader_.reset(LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
  }
  if (loader_.get()) {
    getVersion = reinterpret_cast<VpGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(loader_.get(), "GetAvailableCoreWebView2BrowserVersionString")));
    createEnv = reinterpret_cast<VpCreateEnvironmentFn>(
        reinterpret_cast<void*>(GetProcAddress(loader_.get(), "CreateCoreWebView2EnvironmentWithOptions")));
  }
#endif
  if (!createEnv) {
    log_.note("WebView2Loader.dll missing or unusable");
    message(Str::LoaderMissingText, MB_OK | MB_ICONERROR);
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    return false;
  }
  if (getVersion) {
    CoTaskString version;
    if (FAILED(getVersion(nullptr, version.put())) || version.empty()) {
      log_.note("WebView2 runtime not installed");
      std::wstring title = text_.get(Str::RuntimeMissingTitle);
      std::wstring body = text_.get(Str::RuntimeMissingText);
      if (MessageBoxW(hwnd_, body.c_str(), title.c_str(), MB_YESNO | MB_ICONWARNING) == IDYES) {
        ShellExecuteW(nullptr, L"open", kRuntimeDownloadUrl, nullptr, nullptr, SW_SHOWNORMAL);
      }
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
      return false;
    }
    log_.note("WebView2 runtime " + toUtf8(version.get()));
  }
  if (envVar(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS").empty()) {
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", kBrowserArgs);
  }
  std::wstring userData = dataDir_.empty() ? std::wstring() : dataDir_ + L"\\WebView2";
  auto handler = EnvCompleted::make(IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                                    [this](HRESULT hr, ICoreWebView2Environment* env) { return onEnvironment(hr, env); });
  HRESULT hr = createEnv(nullptr, userData.empty() ? nullptr : userData.c_str(), nullptr, handler.get());
  if (FAILED(hr)) {
    failWebView("CreateCoreWebView2EnvironmentWithOptions", hr);
    return false;
  }
  return true;
}

void App::failWebView(const char* what, HRESULT hr) {
  log_.note(std::string(what) + " failed, hr " + std::to_string(long(hr)));
  message(Str::WebViewFailedText, MB_OK | MB_ICONERROR);
  PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

HRESULT App::onEnvironment(HRESULT hr, ICoreWebView2Environment* env) {
  if (closing_ || !hwnd_) { return S_OK; }
  if (FAILED(hr) || !env) {
    failWebView("WebView2 environment", hr);
    return S_OK;
  }
  env_ = ComPtr<ICoreWebView2Environment>(env);
  auto handler = ControllerCompleted::make(IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                                           [this](HRESULT h, ICoreWebView2Controller* c) { return onController(h, c); });
  HRESULT h2 = env_->CreateCoreWebView2Controller(hwnd_, handler.get());
  if (FAILED(h2)) { return onController(h2, nullptr); }
  return S_OK;
}

HRESULT App::onController(HRESULT hr, ICoreWebView2Controller* controller) {
  if (closing_ || !hwnd_) {
    if (controller) { controller->Close(); }
    return S_OK;
  }
  if (FAILED(hr) || !controller) {
    failWebView("WebView2 controller", hr);
    return S_OK;
  }
  controller_ = ComPtr<ICoreWebView2Controller>(controller);
  if (FAILED(controller_->get_CoreWebView2(webview_.put())) || !webview_) {
    failWebView("get_CoreWebView2", E_FAIL);
    return S_OK;
  }
  configureWebView();
  return S_OK;
}

void App::configureWebView() {
  applyTheme();  // default background colour of the view
  ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(webview_->get_Settings(settings.put())) && settings) {
    settings->put_IsScriptEnabled(TRUE);
    settings->put_IsWebMessageEnabled(TRUE);
    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    settings->put_AreDevToolsEnabled(devtools_ ? TRUE : FALSE);
    settings->put_AreDefaultContextMenusEnabled(devtools_ ? TRUE : FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    settings->put_AreHostObjectsAllowed(FALSE);
    ComPtr<ICoreWebView2Settings3> s3;
    if (SUCCEEDED(settings.as(IID_ICoreWebView2Settings3, s3))) {
      // Release: no reload/print/find/zoom accelerators that would break the single-page app
      // (editing keys such as Ctrl+C/V/Z are not affected).
      s3->put_AreBrowserAcceleratorKeysEnabled(devtools_ ? TRUE : FALSE);
    }
    ComPtr<ICoreWebView2Settings4> s4;
    if (SUCCEEDED(settings.as(IID_ICoreWebView2Settings4, s4))) {
      s4->put_IsGeneralAutofillEnabled(FALSE);
      s4->put_IsPasswordAutosaveEnabled(FALSE);
    }
    ComPtr<ICoreWebView2Settings5> s5;
    if (SUCCEEDED(settings.as(IID_ICoreWebView2Settings5, s5))) { s5->put_IsPinchZoomEnabled(FALSE); }
    ComPtr<ICoreWebView2Settings6> s6;
    if (SUCCEEDED(settings.as(IID_ICoreWebView2Settings6, s6))) { s6->put_IsSwipeNavigationEnabled(FALSE); }
    ComPtr<ICoreWebView2Settings8> s8;
    if (SUCCEEDED(settings.as(IID_ICoreWebView2Settings8, s8))) {
      s8->put_IsReputationCheckingRequired(FALSE);  // no SmartScreen lookups: the app is offline
    }
  }
  ComPtr<ICoreWebView2_3> wv3;
  HRESULT hr = webview_.as(IID_ICoreWebView2_3, wv3);
  if (SUCCEEDED(hr)) {
    // Only the app's own origin may load these files (DENY: no other origin, not even as a
    // sub-resource). The page's CSP (DESIGN §13) is in index.html and applies unchanged.
    hr = wv3->SetVirtualHostNameToFolderMapping(kHostName, uiDir_.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
  }
  if (FAILED(hr)) {
    failWebView("SetVirtualHostNameToFolderMapping (WebView2 Runtime too old?)", hr);
    return;
  }
  webview_->add_WebMessageReceived(
      MessageReceived::make(IID_ICoreWebView2WebMessageReceivedEventHandler,
                            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) { return onWebMessage(a); })
          .get(),
      &tokMessage_);
  webview_->add_NavigationStarting(
      NavigationStarting::make(IID_ICoreWebView2NavigationStartingEventHandler,
                               [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) {
                                 return onNavigationStarting(a);
                               })
          .get(),
      &tokNav_);
  webview_->add_NewWindowRequested(
      NewWindowRequested::make(IID_ICoreWebView2NewWindowRequestedEventHandler,
                               [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) { return onNewWindow(a); })
          .get(),
      &tokNewWin_);
  webview_->add_ProcessFailed(
      ProcessFailed::make(IID_ICoreWebView2ProcessFailedEventHandler,
                          [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) { return onProcessFailed(a); })
          .get(),
      &tokFailed_);
  resizeWebView();
  controller_->put_IsVisible(TRUE);
  if (GetFocus() == hwnd_) { controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }
  // Navigate once the drop script is registered, so the first document has it too.
  HRESULT added = webview_->AddScriptToExecuteOnDocumentCreated(
      kDropScript, ScriptAdded::make(IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
                                     [this](HRESULT h, LPCWSTR) {
                                       if (FAILED(h)) { log_.note("drop script not added, hr " + std::to_string(long(h))); }
                                       if (webview_ && !closing_) { webview_->Navigate(kStartUrl); }
                                       return S_OK;
                                     })
                       .get());
  if (FAILED(added)) {
    log_.note("AddScriptToExecuteOnDocumentCreated failed, hr " + std::to_string(long(added)));
    webview_->Navigate(kStartUrl);
  }
}

void App::closeWebView() {
  if (webview_) {
    webview_->remove_WebMessageReceived(tokMessage_);
    webview_->remove_NavigationStarting(tokNav_);
    webview_->remove_NewWindowRequested(tokNewWin_);
    webview_->remove_ProcessFailed(tokFailed_);
  }
  webview_.reset();
  if (controller_) { controller_->Close(); }
  controller_.reset();
  uiReady_ = false;
}

HRESULT App::onWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
  if (!args) { return S_OK; }
  CoTaskString source;
  if (FAILED(args->get_Source(source.put())) || !isAppUri(toUtf8(source.get()))) { return S_OK; }
  CoTaskString msg;
  if (FAILED(args->TryGetWebMessageAsString(msg.put()))) {
    if (FAILED(args->get_WebMessageAsJson(msg.put()))) { return S_OK; }
  }
  if (!uiReady_) {
    uiReady_ = true;
    onUiReady();
  }
  std::string text = toUtf8(msg.get());
  WebRequest req;
  if (!parseWebRequest(text, req)) {
    long long id = 0;
    if (jsonGetInt(text, "id", id)) { replyError(id, "bad_params", "malformed request", Str::BadParamsHint); }
    return S_OK;
  }
  Route route = routeCommand(req.cmd);
  if (route == Route::Engine) {
    forwardToEngine(text, req);
    return S_OK;
  }
  if (route == Route::Reject) {
    replyError(req.id, "bad_params", "unknown shell command", Str::UnknownCommandHint);
    return S_OK;
  }
  if (shellQueue_.size() >= kMaxShellQueue) {
    replyError(req.id, "busy", "too many pending shell commands", Str::ShellBusyHint);
    return S_OK;
  }
  ShellCommand c;
  c.id = req.id;
  c.cmd = req.cmd;
  c.params.assign(req.params.data(), req.params.size());
  if (c.cmd == "dialog.droppedFiles") {
    // chrome.webview.postMessageWithAdditionalObjects(text, files): the File objects only live as
    // long as the event args, so resolve their real paths now.
    ComPtr<ICoreWebView2WebMessageReceivedEventArgs> base(args);
    ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> a2;
    ComPtr<ICoreWebView2ObjectCollectionView> objects;
    UINT32 count = 0;
    if (SUCCEEDED(base.as(IID_ICoreWebView2WebMessageReceivedEventArgs2, a2)) &&
        SUCCEEDED(a2->get_AdditionalObjects(objects.put())) && objects && SUCCEEDED(objects->get_Count(&count))) {
      for (UINT32 i = 0; i < count && i < kMaxDroppedFiles; ++i) {
        ComPtr<IUnknown> obj;
        ComPtr<ICoreWebView2File> file;
        if (FAILED(objects->GetValueAtIndex(i, obj.put())) || !obj || FAILED(obj.as(IID_ICoreWebView2File, file))) {
          continue;
        }
        CoTaskString p;
        if (SUCCEEDED(file->get_Path(p.put())) && !p.empty()) { c.dropped.emplace_back(p.get()); }
      }
    }
  }
  // Modal dialogs must not run inside a WebView2 event handler: defer to the message loop.
  shellQueue_.push_back(std::move(c));
  if (!shellPosted_) {
    shellPosted_ = true;
    PostMessageW(hwnd_, WM_APP_SHELL_CMD, 0, 0);
  }
  return S_OK;
}

HRESULT App::onNavigationStarting(ICoreWebView2NavigationStartingEventArgs* args) {
  if (!args) { return S_OK; }
  CoTaskString uri;
  if (FAILED(args->get_Uri(uri.put()))) { return S_OK; }
  std::string u = toUtf8(uri.get());
  if (isAppUri(u)) {
    uiReady_ = false;  // a (re)load: the next message from the page means it is ready again
    return S_OK;
  }
  args->put_Cancel(TRUE);  // never leave the app
  // A file dropped on the window: WebView2 navigates to it when the page does not take the drop.
  std::string path;
  if (filePathFromFileUri(u, path)) {
    emitOpen(std::vector<std::wstring>{toWide(path)}, "drop");
    return S_OK;
  }
  BOOL user = FALSE;
  args->get_IsUserInitiated(&user);
  if (user && isSafeExternalUrl(u)) { ShellExecuteW(hwnd_, L"open", uri.get(), nullptr, nullptr, SW_SHOWNORMAL); }
  return S_OK;
}

HRESULT App::onNewWindow(ICoreWebView2NewWindowRequestedEventArgs* args) {
  if (!args) { return S_OK; }
  args->put_Handled(TRUE);
  CoTaskString uri;
  BOOL user = FALSE;
  args->get_IsUserInitiated(&user);
  if (SUCCEEDED(args->get_Uri(uri.put())) && user && isSafeExternalUrl(toUtf8(uri.get()))) {
    ShellExecuteW(hwnd_, L"open", uri.get(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  return S_OK;
}

HRESULT App::onProcessFailed(ICoreWebView2ProcessFailedEventArgs* args) {
  COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
  if (args) { args->get_ProcessFailedKind(&kind); }
  log_.note("WebView2 process failed, kind " + std::to_string(int(kind)));
  if (closing_) { return S_OK; }
  if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
    // The whole browser is gone: rebuild the view (a few times at most).
    closeWebView();
    env_.reset();
    if (++webviewRecreations_ > 3) {
      message(Str::WebViewFailedText, MB_OK | MB_ICONERROR);
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    } else {
      initWebView();
    }
  } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
             kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
    if (webview_) { webview_->Reload(); }
  }
  return S_OK;
}

void App::postToWeb(const std::string& json) {
  if (!webview_) { return; }
  std::wstring w = toWide(json);
  webview_->PostWebMessageAsString(w.c_str());  // the UI's message event gets the JSON text in e.data
}

void App::onUiReady() {
  emitPower(true);
  if (!pendingOpens_.empty()) {
    std::vector<std::wstring> paths;
    paths.swap(pendingOpens_);
    emitOpen(paths, "launch");
  }
}

void App::emitOpen(const std::vector<std::wstring>& paths, const char* source) {
  postToWeb(makeEvent("dialog.droppedFiles", "\"paths\":" + utf8Paths(paths) + ",\"source\":" + jsonQuote(source)));
}

void App::requestOpen(const std::vector<std::wstring>& paths) {
  std::vector<std::wstring> full;
  for (const std::wstring& p : paths) { full.push_back(fullPath(p)); }
  if (uiReady_) {
    emitOpen(full, "launch");
    return;
  }
  for (std::wstring& p : full) {
    if (pendingOpens_.size() < kMaxPendingOpens) { pendingOpens_.push_back(std::move(p)); }
  }
}

void App::emitPower(bool force) {
  SYSTEM_POWER_STATUS s{};
  if (!GetSystemPowerStatus(&s)) { return; }
  int onBattery = s.ACLineStatus == 0 ? 1 : 0;  // 255 (unknown) counts as mains
  if (!force && onBattery == lastOnBattery_) { return; }
  lastOnBattery_ = onBattery;
  postToWeb(makeEvent("power.status", onBattery ? "\"onBattery\":true" : "\"onBattery\":false"));
}

// ------------------------------------------------------------------------------ engine

void App::startEngine() {
  std::wstring dir = exeDir();
  std::wstring exe = dir + L"\\" + kEngineExe;
  if (!fileExists(exe)) {
    engineUnavailable_ = true;
    log_.note("engine not found: " + toUtf8(exe));
    message(Str::EngineMissingText, MB_OK | MB_ICONERROR);
    return;
  }
  std::string args = "serve";
  if (!dataDir_.empty()) { args += " --data " + quoteWindowsArg(toUtf8(dataDir_)); }
  // The dist layout keeps the lexicons (*.vpl) in data\ next to the exe; without that folder the
  // engine uses its own defaults (VP_LEXICON_DIR, <exe>\lexicons, <data>\lexicons).
  if (dirExists(dir + L"\\data")) { args += " --lexicons " + quoteWindowsArg(toUtf8(dir + L"\\data")); }
  DWORD err = 0;
  if (!engine_->start(exe, toWide(args), &err)) {
    engineUnavailable_ = true;
    log_.note("engine failed to start, error " + std::to_string(err));
    message(Str::EngineMissingText, MB_OK | MB_ICONERROR);
    return;
  }
  engineUnavailable_ = false;
  watchdog_.reset(nowMs());
}

void App::replyError(long long id, const char* code, const char* msg, Str hint) {
  postToWeb(makeErrorResponse(id, code, msg, text_.utf8(hint)));
}

void App::forwardToEngine(const std::string& text, const WebRequest& req) {
  if (text.find('\n') != std::string::npos || text.find('\r') != std::string::npos) {
    replyError(req.id, "bad_params", "request contains a line break", Str::BadParamsHint);
    return;
  }
  if (restarting_) {  // recovering the project; the UI resets itself on engine.restarted
    replyError(req.id, "busy", "engine restarting", Str::EngineRestartingHint);
    return;
  }
  if (engineUnavailable_ || crashLoopStopped_ || !engine_ || !engine_->running()) {
    replyError(req.id, "internal", "engine unavailable", Str::EngineUnavailableHint);
    return;
  }
  tracker_.onRequest(req.id, req.cmd, req.params);
  if (!engine_->send(text)) {
    tracker_.onResponse(req.id, false, std::string_view());
    replyError(req.id, "busy", "engine input queue full", Str::ShellBusyHint);
  }
}

void App::onEngineOutput() {
  if (!engine_) { return; }
  engine_->takeOutput(outBuf_);
  if (outBuf_.empty()) { return; }
  watchdog_.onPong(nowMs());  // any line proves the engine is alive
  while (!outBuf_.empty()) {
    std::string line = std::move(outBuf_.front());
    outBuf_.pop_front();
    EngineLine e = classifyEngineLine(line);
    if (e.kind == EngineLine::Response && e.id < 0) {
      onShellResponse(e, line);
    } else if (e.kind == EngineLine::Response) {
      tracker_.onResponse(e.id, e.ok, line);
      postToWeb(line);
    } else if (e.kind == EngineLine::Event) {
      postToWeb(line);
    } else {
      log_.note("engine stdout (not JSON): " + line.substr(0, 200));
    }
  }
  std::deque<std::string>().swap(outBuf_);  // drained lines may have been large cue pages; keep nothing
  noteSettingsChange();  // a settings result may have changed the theme or the language
}

void App::onShellResponse(const EngineLine& e, const std::string& line) {
  if (e.id == kPingId || e.id == kShutdownId) { return; }
  if (e.id == kSettingsId && e.ok) {
    std::string_view result;
    if (jsonFindRaw(line, "result", result)) { tracker_.noteSettings(result); }
  }
  if (!e.ok) { log_.note("shell request " + std::to_string(e.id) + " failed: " + line.substr(0, 300)); }
  applyStep(sequence_.onResponse(e.id, e.ok, nowMs()));
}

void App::applyStep(const RestartSequence::Step& step) {
  for (const std::string& l : step.send) {
    if (!engine_ || !engine_->send(l)) { log_.note("could not send to the restarted engine: " + l.substr(0, 200)); }
  }
  if (!step.event.empty()) {
    restarting_ = false;
    log_.note("engine restarted: " + step.event);
    postToWeb(step.event);
  }
}

void App::restartEngine(const char* reason) {
  if (closing_ || !engine_) { return; }
  log_.note(std::string("restarting engine: ") + reason);
  engine_->kill();
  sequence_.cancel();
  restarting_ = false;
  // Requests the old engine never answered fail now; settings patches among them are replayed.
  std::vector<std::string> replay;
  for (long long id : tracker_.onEngineRestart(&replay)) {
    replyError(id, "internal", "engine restarted", Str::EngineRestartingHint);
  }
  if (!limiter_.allow(nowMs())) {
    crashLoopStopped_ = true;
    log_.note("engine restarted 3 times within 60 s; not restarting it again until the app is reopened");
    message(Str::EngineCrashLoopText, MB_OK | MB_ICONWARNING, logDir_ + L"\\engine.log");
    return;
  }
  startEngine();
  if (engineUnavailable_) { return; }
  restarting_ = true;
  applyStep(sequence_.start(tracker_, replay, nowMs()));
}

void App::onTick() {
  if (closing_ || crashLoopStopped_ || engineUnavailable_ || !engine_ || !engine_->running()) { return; }
  int64_t now = nowMs();
  if (engine_->processExited()) {
    onEngineOutput();
    restartEngine("engine process exited");
    return;
  }
  if (sequence_.timedOut(now)) {
    restartEngine("project recovery did not answer");
    return;
  }
  switch (watchdog_.tick(now)) {
    case Watchdog::Action::SendPing:
      engine_->send(makeRequest(kPingId, "engine.ping", "{}"));
      break;
    case Watchdog::Action::Restart:
      restartEngine("no answer for 10 s");
      break;
    case Watchdog::Action::None:
      break;
  }
}

// ------------------------------------------------------------------------ shell commands

void App::runShellCommands() {
  shellPosted_ = false;
  while (!shellQueue_.empty() && !closing_) {
    ShellCommand c = std::move(shellQueue_.front());
    shellQueue_.pop_front();
    runShellCommand(c);
  }
}

bool App::filtersFor(const std::string& params, std::vector<FileFilter>& out) {
  out.clear();
  std::string_view raw;
  if (jsonFindRaw(params, "filters", raw) && raw != "null") {
    if (!parseFilters(raw, out)) { return false; }
    if (out.size() == 1 && out[0].name.empty()) {
      // A plain extension list: name it after what it holds, and let the user pick any file too.
      const std::string& pat = out[0].pattern;
      bool subs = true;
      size_t start = 0;
      while (start < pat.size()) {
        size_t end = pat.find(';', start);
        std::string ext = pat.substr(start + 2, (end == std::string::npos ? pat.size() : end) - start - 2);
        subs = subs && (ext == "srt" || ext == "vtt" || ext == "ass" || ext == "ssa");
        if (end == std::string::npos) { break; }
        start = end + 1;
      }
      Str name = subs ? Str::FilterSubtitles
                 : pat == "*.vpoeta" ? Str::FilterProjects
                 : pat == "*.txt" ? Str::FilterTexts
                                  : Str::FilterSupported;
      out[0].name = text_.utf8(name);
      out.push_back(FileFilter{text_.utf8(Str::FilterAll), "*.*", ""});
    }
    return true;
  }
  // Default: everything the app opens.
  auto add = [&](Str name, const char* pattern, const char* ext) {
    out.push_back(FileFilter{text_.utf8(name), pattern, ext});
  };
  add(Str::FilterSupported, "*.vpoeta;*.srt;*.vtt;*.ass;*.ssa;*.txt", "");
  add(Str::FilterSubtitles, "*.srt;*.vtt;*.ass;*.ssa", "srt");
  add(Str::FilterProjects, "*.vpoeta", "vpoeta");
  add(Str::FilterTexts, "*.txt", "txt");
  add(Str::FilterAll, "*.*", "");
  return true;
}

void App::runShellCommand(const ShellCommand& c) {
  std::string title;
  jsonGetString(c.params, "title", title);
  if (c.cmd == "dialog.openFile") {
    bool multiple = false;
    jsonGetBool(c.params, "multiple", multiple);
    std::vector<FileFilter> filters;
    if (!filtersFor(c.params, filters)) {
      replyError(c.id, "bad_params", "bad filters", Str::BadParamsHint);
      return;
    }
    std::vector<std::wstring> paths;
    HRESULT hr = S_OK;
    if (!openFileDialog(filters, multiple, title, paths, hr)) {
      log_.note("open dialog failed, hr " + std::to_string(long(hr)));
      replyError(c.id, "internal", "open dialog failed", Str::DialogFailedHint);
      return;
    }
    std::string first = paths.empty() ? std::string("null") : jsonQuote(toUtf8(paths[0]));
    postToWeb(makeOkResponse(c.id, "{\"path\":" + first + ",\"paths\":" + utf8Paths(paths) +
                                       ",\"cancelled\":" + (paths.empty() ? "true" : "false") + "}"));
  } else if (c.cmd == "dialog.saveFile") {
    std::string suggested;
    if (!jsonGetString(c.params, "suggested", suggested)) { jsonGetString(c.params, "suggestedName", suggested); }
    std::vector<FileFilter> filters;
    if (!filtersFor(c.params, filters)) {
      replyError(c.id, "bad_params", "bad filters", Str::BadParamsHint);
      return;
    }
    std::wstring path;
    HRESULT hr = S_OK;
    if (!saveFileDialog(filters, suggested, title, path, hr)) {
      log_.note("save dialog failed, hr " + std::to_string(long(hr)));
      replyError(c.id, "internal", "save dialog failed", Str::DialogFailedHint);
      return;
    }
    postToWeb(makeOkResponse(c.id, "{\"path\":" + (path.empty() ? std::string("null") : jsonQuote(toUtf8(path))) +
                                       ",\"cancelled\":" + (path.empty() ? "true" : "false") + "}"));
  } else if (c.cmd == "dialog.droppedFiles") {
    bool emit = false;
    jsonGetBool(c.params, "emit", emit);
    if (!emit) {
      postToWeb(makeOkResponse(c.id, "{\"paths\":" + utf8Paths(c.dropped) + "}"));
    } else if (!c.dropped.empty()) {
      emitOpen(c.dropped, "drop");  // posted by kDropScript: the event the screens listen to
    }
  } else if (c.cmd == "shell.revealFile") {
    std::string p, norm;
    if (!jsonGetString(c.params, "path", p) || !normaliseRevealPath(p, norm)) {
      replyError(c.id, "bad_params", "not an absolute path", Str::BadParamsHint);
      return;
    }
    std::wstring wp = toWide(norm);
    if (fileExists(wp)) {
      PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(wp.c_str());
      HRESULT hr = pidl ? SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0) : E_FAIL;
      if (pidl) { ILFree(pidl); }
      if (FAILED(hr)) {
        std::wstring args = L"/select,\"" + wp + L"\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
      }
    } else if (dirExists(wp)) {
      ShellExecuteW(hwnd_, L"explore", wp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
      replyError(c.id, "not_found", "file not found", Str::FileNotFoundHint);
      return;
    }
    postToWeb(makeOkResponse(c.id, "{}"));
  } else if (c.cmd == "shell.openExternal") {
    std::string url;
    if (!jsonGetString(c.params, "url", url) || !isSafeExternalUrl(url)) {
      replyError(c.id, "bad_params", "only http(s) URLs", Str::BadUrlHint);
      return;
    }
    ShellExecuteW(hwnd_, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    postToWeb(makeOkResponse(c.id, "{}"));
  }
}

namespace {
// Keeps the wide strings alive while IFileDialog holds COMDLG_FILTERSPEC pointers into them.
struct FilterSpecs {
  std::vector<std::wstring> names, patterns;
  std::vector<COMDLG_FILTERSPEC> specs;
  explicit FilterSpecs(const std::vector<FileFilter>& f) {
    for (const FileFilter& x : f) {
      names.push_back(toWide(x.name));
      patterns.push_back(toWide(x.pattern));
    }
    for (size_t i = 0; i < f.size(); ++i) { specs.push_back(COMDLG_FILTERSPEC{names[i].c_str(), patterns[i].c_str()}); }
  }
  void apply(IFileDialog* dlg) const {
    if (specs.empty()) { return; }
    dlg->SetFileTypes(UINT(specs.size()), specs.data());
    dlg->SetFileTypeIndex(1);
  }
};
}  // namespace

bool App::openFileDialog(const std::vector<FileFilter>& filters, bool multiple, const std::string& title,
                         std::vector<std::wstring>& out, HRESULT& hr) {
  out.clear();
  ComPtr<IFileOpenDialog> dlg;
  hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                        reinterpret_cast<void**>(dlg.put()));
  if (FAILED(hr)) { return false; }
  FILEOPENDIALOGOPTIONS opts = 0;
  dlg->GetOptions(&opts);
  opts |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
  if (multiple) { opts |= FOS_ALLOWMULTISELECT; }
  dlg->SetOptions(opts);
  FilterSpecs specs(filters);
  specs.apply(dlg.get());
  if (!title.empty()) { dlg->SetTitle(toWide(title).c_str()); }
  hr = dlg->Show(hwnd_);
  if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    hr = S_OK;
    return true;
  }
  if (FAILED(hr)) { return false; }
  ComPtr<IShellItemArray> items;
  if (FAILED(hr = dlg->GetResults(items.put()))) { return false; }
  DWORD n = 0;
  items->GetCount(&n);
  for (DWORD i = 0; i < n; ++i) {
    ComPtr<IShellItem> item;
    CoTaskString path;
    if (SUCCEEDED(items->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, path.put())) &&
        !path.empty()) {
      out.emplace_back(path.get());
    }
  }
  return true;
}

bool App::saveFileDialog(const std::vector<FileFilter>& filters, const std::string& suggested, const std::string& title,
                         std::wstring& out, HRESULT& hr) {
  out.clear();
  ComPtr<IFileSaveDialog> dlg;
  hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog,
                        reinterpret_cast<void**>(dlg.put()));
  if (FAILED(hr)) { return false; }
  FILEOPENDIALOGOPTIONS opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN);
  FilterSpecs specs(filters);
  specs.apply(dlg.get());
  std::string ext = extensionOf(suggested);
  if (ext.empty() && !filters.empty()) { ext = filters[0].firstExt; }
  if (!ext.empty()) { dlg->SetDefaultExtension(toWide(ext).c_str()); }
  if (!title.empty()) { dlg->SetTitle(toWide(title).c_str()); }
  std::string dir, name;
  splitDirName(suggested, dir, name);
  if (!name.empty()) { dlg->SetFileName(toWide(name).c_str()); }
  std::string normDir;
  if (!dir.empty() && normaliseRevealPath(dir, normDir) && dirExists(toWide(normDir))) {
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(toWide(normDir).c_str(), nullptr, IID_IShellItem,
                                              reinterpret_cast<void**>(folder.put())))) {
      dlg->SetFolder(folder.get());
    }
  }
  hr = dlg->Show(hwnd_);
  if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    hr = S_OK;
    return true;
  }
  if (FAILED(hr)) { return false; }
  ComPtr<IShellItem> item;
  CoTaskString path;
  if (FAILED(hr = dlg->GetResult(item.put())) || FAILED(hr = item->GetDisplayName(SIGDN_FILESYSPATH, path.put()))) {
    return false;
  }
  out = path.get();
  return true;
}

// ------------------------------------------------------------------- process entry helpers

class ComScope {
 public:
  ComScope() : ok_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {}
  ~ComScope() {
    if (ok_) { CoUninitialize(); }
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  bool ok() const { return ok_; }

 private:
  bool ok_;
};

std::vector<std::string> commandLineArgs() {
  std::vector<std::string> out;
  int n = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
  if (!argv) { return out; }
  for (int i = 1; i < n; ++i) { out.push_back(toUtf8(argv[i])); }
  LocalFree(argv);
  return out;
}

// Second instance: hand the files to the first one (WM_COPYDATA) and bring it forward.
void forwardToFirstInstance(const CommandLine& cl) {
  HWND other = nullptr;
  for (int i = 0; i < 50 && !other; ++i) {  // the first instance may still be starting
    other = FindWindowW(kWindowClass, nullptr);
    if (!other) { Sleep(100); }
  }
  if (!other) { return; }
  DWORD pid = 0;
  GetWindowThreadProcessId(other, &pid);
  if (pid) { AllowSetForegroundWindow(pid); }
  if (!cl.files.empty()) {
    std::vector<std::string> full;
    for (const std::string& f : cl.files) { full.push_back(toUtf8(fullPath(toWide(f)))); }
    std::string payload = encodeOpenPayload(full);
    if (!payload.empty() && payload.size() <= kMaxCopyDataBytes) {
      COPYDATASTRUCT cds{};
      cds.dwData = kCopyDataMagic;
      cds.cbData = DWORD(payload.size());
      cds.lpData = &payload[0];
      DWORD_PTR result = 0;
      SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 5000, &result);
    }
  }
  if (IsIconic(other)) { ShowWindow(other, SW_RESTORE); }
  SetForegroundWindow(other);
}

}  // namespace
}  // namespace shell
}  // namespace vp

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  using namespace vp::shell;
  enablePerMonitorDpiAwareness();
  CommandLine cl = parseCommandLine(commandLineArgs());

  UniqueHandle mutex(CreateMutexW(nullptr, FALSE, kMutexName));
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    forwardToFirstInstance(cl);
    return 0;
  }

  ComScope com;
  if (!com.ok()) { return 1; }
  App app(cl);
  if (!app.create(inst)) { return 1; }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return int(msg.wParam);
}
