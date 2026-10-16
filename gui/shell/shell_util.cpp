// Win32 helpers for the shell (see shell_util.h).
#include "shell_util.h"

#include <shlobj.h>

#include <vector>

#include "host_logic.h"

namespace vp {
namespace shell {

bool JoinedThread::start(std::function<void()> fn) {
  join();
  fn_ = std::make_unique<std::function<void()>>(std::move(fn));
  h_.reset(CreateThread(nullptr, 0, &JoinedThread::entry, fn_.get(), 0, nullptr));
  if (!h_.valid()) {
    fn_.reset();
    return false;
  }
  return true;
}

DWORD WINAPI JoinedThread::entry(LPVOID p) {
  (*static_cast<std::function<void()>*>(p))();
  return 0;
}

void JoinedThread::join() {
  if (h_.valid()) {
    WaitForSingleObject(h_.get(), INFINITE);
    h_.reset();
  }
  fn_.reset();
}

void JoinedThread::joinCancellingIo() {
  if (!h_.valid()) { return; }
  while (WaitForSingleObject(h_.get(), 0) != WAIT_OBJECT_0) {
    CancelSynchronousIo(h_.get());
    if (WaitForSingleObject(h_.get(), 50) == WAIT_OBJECT_0) { break; }
  }
  join();
}

std::wstring toWide(std::string_view s) {
  if (s.empty()) { return std::wstring(); }
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  if (n <= 0) { return std::wstring(); }
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}

std::string toUtf8(std::wstring_view w) {
  if (w.empty()) { return std::string(); }
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) { return std::string(); }
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring exeDir() {
  std::vector<wchar_t> buf(MAX_PATH);
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n == 0) { return std::wstring(); }
    if (n < buf.size()) {
      std::wstring p(buf.data(), n);
      size_t slash = p.find_last_of(L"\\/");
      return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
    }
    if (buf.size() >= 32768) { return std::wstring(); }
    buf.resize(buf.size() * 2);
  }
}

std::wstring envVar(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) { return std::wstring(); }
  std::wstring v(n, L'\0');
  n = GetEnvironmentVariableW(name, &v[0], n);
  v.resize(n);
  return v;
}

bool ensureDir(const std::wstring& dir) {
  if (dir.empty()) { return false; }
  DWORD a = GetFileAttributesW(dir.c_str());
  if (a != INVALID_FILE_ATTRIBUTES) { return (a & FILE_ATTRIBUTE_DIRECTORY) != 0; }
  size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos && slash > 2) { ensureDir(dir.substr(0, slash)); }
  return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring dataDir() {
  std::wstring dir = envVar(L"VP_DATA_DIR");
  if (dir.empty()) {
    CoTaskString base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, base.put())) &&
        !base.empty()) {
      dir = std::wstring(base.get()) + L"\\vetus-poeta";
    }
  }
  if (!dir.empty()) { ensureDir(dir); }
  return dir;
}

bool fileExists(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring fullPath(const std::wstring& p) {
  DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
  if (n == 0) { return p; }
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(p.c_str(), n, &out[0], nullptr);
  out.resize(n);
  return out;
}

bool readSmallFile(const std::wstring& path, std::string& out, size_t maxBytes) {
  out.clear();
  UniqueHandle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!f) { return false; }
  LARGE_INTEGER size;
  if (!GetFileSizeEx(f.get(), &size) || size.QuadPart < 0 || uint64_t(size.QuadPart) > maxBytes) { return false; }
  out.resize(size_t(size.QuadPart));
  DWORD got = 0;
  if (!out.empty() && (!ReadFile(f.get(), &out[0], DWORD(out.size()), &got, nullptr) || got != out.size())) {
    out.clear();
    return false;
  }
  return true;
}

bool writeFileAtomic(const std::wstring& path, std::string_view data) {
  std::wstring tmp = path + L".tmp";
  {
    UniqueHandle f(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f) { return false; }
    DWORD put = 0;
    if (!WriteFile(f.get(), data.data(), DWORD(data.size()), &put, nullptr) || put != data.size()) {
      f.reset();
      DeleteFileW(tmp.c_str());
      return false;
    }
    FlushFileBuffers(f.get());
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

int64_t nowMs() { return int64_t(GetTickCount64()); }

bool systemPrefersDark() {
  DWORD value = 1, size = sizeof value;
  LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  return st == ERROR_SUCCESS && value == 0;
}

bool systemIsSpanish() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH; }

// ------------------------------------------------------------------------------ strings

namespace {
struct StrEntry {
  const char* key;
  const wchar_t* en;
  const wchar_t* es;
};
const StrEntry kStrings[] = {
  {"app.shell.runtimeMissing.title", L"vetus poeta needs Microsoft Edge WebView2",
   L"vetus poeta necesita Microsoft Edge WebView2"},
  {"app.shell.runtimeMissing.text",
   L"The Microsoft Edge WebView2 Runtime is not installed on this computer. It is free and already part of "
   L"Windows 10 and 11 on most computers.\n\nOpen the download page now? (This is the only time vetus poeta "
   L"opens a web page by itself.)",
   L"El componente Microsoft Edge WebView2 Runtime no está instalado en esta computadora. Es gratuito y ya "
   L"viene con Windows 10 y 11 en la mayoría de las computadoras.\n\n¿Abrir ahora la página de descarga? "
   L"(Es la única vez que vetus poeta abre una página web por su cuenta.)"},
  {"app.shell.loaderMissing.text",
   L"WebView2Loader.dll is missing next to VetusPoeta.exe. Install vetus poeta again.",
   L"Falta WebView2Loader.dll junto a VetusPoeta.exe. Vuelve a instalar vetus poeta."},
  {"app.shell.engineMissing.text",
   L"The translator (vpengine.exe) could not be started. It must be next to VetusPoeta.exe. Install vetus "
   L"poeta again.",
   L"No se pudo iniciar el traductor (vpengine.exe). Debe estar junto a VetusPoeta.exe. Vuelve a instalar "
   L"vetus poeta."},
  {"app.shell.uiMissing.text",
   L"The folder \"ui\" is missing next to VetusPoeta.exe. Install vetus poeta again.",
   L"Falta la carpeta \"ui\" junto a VetusPoeta.exe. Vuelve a instalar vetus poeta."},
  {"app.shell.engineCrashLoop.text",
   L"The translator keeps stopping, so vetus poeta stopped restarting it. Your work is saved automatically. "
   L"Close vetus poeta and open it again. If it happens again, this log file helps to find the cause:\n\n",
   L"El traductor se detiene una y otra vez, así que vetus poeta dejó de reiniciarlo. Tu trabajo se guarda "
   L"automáticamente. Cierra vetus poeta y vuelve a abrirlo. Si vuelve a pasar, este archivo de registro "
   L"ayuda a encontrar la causa:\n\n"},
  {"app.shell.engineRestarting.hint", L"The translator is restarting. Try again in a moment.",
   L"El traductor se está reiniciando. Inténtalo de nuevo en un momento."},
  {"app.shell.engineUnavailable.hint", L"The translator is not running. Close vetus poeta and open it again.",
   L"El traductor no está funcionando. Cierra vetus poeta y vuelve a abrirlo."},
  {"app.shell.webviewFailed.text",
   L"vetus poeta could not open its window (WebView2 error). Restart the computer and try again.",
   L"vetus poeta no pudo abrir su ventana (error de WebView2). Reinicia la computadora e inténtalo de nuevo."},
  {"app.shell.filter.supported.label", L"Subtitles, texts and projects", L"Subtítulos, textos y proyectos"},
  {"app.shell.filter.subtitles.label", L"Subtitles", L"Subtítulos"},
  {"app.shell.filter.projects.label", L"vetus poeta projects", L"Proyectos de vetus poeta"},
  {"app.shell.filter.texts.label", L"Texts", L"Textos"},
  {"app.shell.filter.all.label", L"All files", L"Todos los archivos"},
  {"app.shell.dialogFailed.hint", L"The file window could not be opened. Try again.",
   L"No se pudo abrir la ventana de archivos. Inténtalo de nuevo."},
  {"app.shell.fileNotFound.hint", L"That file is no longer there. It may have been moved or deleted.",
   L"Ese archivo ya no está ahí. Tal vez se movió o se borró."},
  {"app.shell.badUrl.hint", L"Only web links (http or https) can be opened.",
   L"Solo se pueden abrir enlaces web (http o https)."},
  {"app.shell.badParams.hint", L"Something went wrong inside vetus poeta. Try again.",
   L"Algo salió mal dentro de vetus poeta. Inténtalo de nuevo."},
  {"app.shell.unknownCommand.hint", L"This version of vetus poeta cannot do that.",
   L"Esta versión de vetus poeta no puede hacer eso."},
  {"app.shell.busy.hint", L"vetus poeta is busy. Try again in a moment.",
   L"vetus poeta está ocupado. Inténtalo de nuevo en un momento."},
};
static_assert(sizeof(kStrings) / sizeof(kStrings[0]) == size_t(Str::ShellBusyHint) + 1, "string table size");
}  // namespace

const char* shellStringKey(Str id) { return kStrings[size_t(id)].key; }

void ShellText::load(const std::wstring& uiDir, std::string_view lang) {
  lang_ = uiLanguage(lang);
  table_.clear();
  if (uiDir.empty()) { return; }
  std::string text;
  if (readSmallFile(uiDir + L"\\i18n\\" + toWide(lang_) + L".json", text, size_t(4) << 20)) { table_.swap(text); }
}

std::wstring ShellText::get(Str id) const {
  const StrEntry& e = kStrings[size_t(id)];
  std::string v;
  if (!table_.empty() && jsonGetString(table_, e.key, v) && !v.empty()) { return toWide(v); }
  return lang_ == "es-MX" ? e.es : e.en;
}

}  // namespace shell
}  // namespace vp
