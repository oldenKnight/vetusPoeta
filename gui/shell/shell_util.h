// Win32 helpers for the shell: RAII wrappers for handles, modules, COM pointers and threads,
// UTF-8/UTF-16 conversion, app folders, small file I/O, and the strings the shell shows itself.
#pragma once

#include <windows.h>
#include <objbase.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace vp {
namespace shell {

// Owns a kernel HANDLE (process, thread, pipe, file, job, mutex). nullptr and
// INVALID_HANDLE_VALUE both mean "empty".
class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE h) : h_(h) {}
  ~UniqueHandle() { reset(); }
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
  UniqueHandle(UniqueHandle&& o) noexcept : h_(o.release()) {}
  UniqueHandle& operator=(UniqueHandle&& o) noexcept {
    if (this != &o) { reset(o.release()); }
    return *this;
  }
  HANDLE get() const { return h_; }
  bool valid() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
  explicit operator bool() const { return valid(); }
  HANDLE release() {
    HANDLE h = h_;
    h_ = nullptr;
    return h;
  }
  void reset(HANDLE h = nullptr) {
    if (valid()) { CloseHandle(h_); }
    h_ = h;
  }

 private:
  HANDLE h_ = nullptr;
};

class UniqueModule {
 public:
  UniqueModule() = default;
  explicit UniqueModule(HMODULE m) : m_(m) {}
  ~UniqueModule() { reset(nullptr); }
  UniqueModule(const UniqueModule&) = delete;
  UniqueModule& operator=(const UniqueModule&) = delete;
  HMODULE get() const { return m_; }
  void reset(HMODULE m) {
    if (m_) { FreeLibrary(m_); }
    m_ = m;
  }

 private:
  HMODULE m_ = nullptr;
};

// GDI object (brush) owner.
class UniqueBrush {
 public:
  UniqueBrush() = default;
  ~UniqueBrush() { reset(nullptr); }
  UniqueBrush(const UniqueBrush&) = delete;
  UniqueBrush& operator=(const UniqueBrush&) = delete;
  HBRUSH get() const { return b_; }
  void reset(HBRUSH b) {
    if (b_) { DeleteObject(b_); }
    b_ = b;
  }

 private:
  HBRUSH b_ = nullptr;
};

// Minimal intrusive COM pointer (no __uuidof, works with the MIDL headers under MinGW).
template <class T>
class ComPtr {
 public:
  ComPtr() = default;
  ComPtr(std::nullptr_t) {}
  explicit ComPtr(T* p, bool addRef = true) : p_(p) {
    if (p_ && addRef) { p_->AddRef(); }
  }
  ~ComPtr() { reset(); }
  ComPtr(const ComPtr& o) : p_(o.p_) {
    if (p_) { p_->AddRef(); }
  }
  ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  ComPtr& operator=(const ComPtr& o) {
    if (o.p_) { o.p_->AddRef(); }
    reset();
    p_ = o.p_;
    return *this;
  }
  ComPtr& operator=(ComPtr&& o) noexcept {
    if (this != &o) {
      reset();
      p_ = o.p_;
      o.p_ = nullptr;
    }
    return *this;
  }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  T** put() {
    reset();
    return &p_;
  }
  void reset() {
    if (p_) {
      T* p = p_;
      p_ = nullptr;
      p->Release();
    }
  }
  // QueryInterface with an explicit IID.
  template <class U>
  HRESULT as(REFIID iid, ComPtr<U>& out) const {
    if (!p_) { return E_POINTER; }
    return p_->QueryInterface(iid, reinterpret_cast<void**>(out.put()));
  }

 private:
  T* p_ = nullptr;
};

// Owns a CoTaskMemAlloc'ed wide string (WebView2 and shell getters return these).
class CoTaskString {
 public:
  CoTaskString() = default;
  ~CoTaskString() {
    if (p_) { CoTaskMemFree(p_); }
  }
  CoTaskString(const CoTaskString&) = delete;
  CoTaskString& operator=(const CoTaskString&) = delete;
  LPWSTR* put() {
    if (p_) {
      CoTaskMemFree(p_);
      p_ = nullptr;
    }
    return &p_;
  }
  const wchar_t* get() const { return p_ ? p_ : L""; }
  bool empty() const { return !p_ || !*p_; }

 private:
  LPWSTR p_ = nullptr;
};

// A Win32 thread that is always joined (the destructor joins). CreateThread rather than
// std::thread so a blocked ReadFile/WriteFile can be cancelled with CancelSynchronousIo.
class JoinedThread {
 public:
  JoinedThread() = default;
  ~JoinedThread() { join(); }
  JoinedThread(const JoinedThread&) = delete;
  JoinedThread& operator=(const JoinedThread&) = delete;
  bool start(std::function<void()> fn);
  void join();
  // Joins, cancelling blocking I/O of the thread every 50 ms until it has finished.
  void joinCancellingIo();

 private:
  static DWORD WINAPI entry(LPVOID p);
  std::unique_ptr<std::function<void()>> fn_;  // outlives the thread: join() precedes reset
  UniqueHandle h_;
};

std::wstring toWide(std::string_view utf8);
std::string toUtf8(std::wstring_view w);
std::wstring exeDir();                    // folder of VetusPoeta.exe, no trailing slash
std::wstring envVar(const wchar_t* name);  // "" when unset
// The engine's data folder (same rule as vp::fs::dataDir): VP_DATA_DIR, else
// %LOCALAPPDATA%\vetus-poeta. Created. Holds settings.json, window.json, logs\, WebView2\.
std::wstring dataDir();
bool ensureDir(const std::wstring& dir);  // creates missing parents too
bool readSmallFile(const std::wstring& path, std::string& out, size_t maxBytes);
bool writeFileAtomic(const std::wstring& path, std::string_view data);
bool fileExists(const std::wstring& path);
bool dirExists(const std::wstring& path);
std::wstring fullPath(const std::wstring& p);
int64_t nowMs();
bool systemPrefersDark();     // HKCU ...\Themes\Personalize\AppsUseLightTheme == 0
bool systemIsSpanish();       // the Windows display language

// Strings the shell shows itself (native dialogs, error hints for shell-handled commands, file
// dialog filter names). They are looked up in the UI's string tables first
// (<exe>\ui\i18n\<lang>.json, keys app.shell.*), so the tables own them; the built-in en-US and
// es-MX texts are the fallback for a broken install (the ui folder missing) and until the keys exist.
enum class Str {
  RuntimeMissingTitle, RuntimeMissingText, LoaderMissingText, EngineMissingText, UiMissingText,
  EngineCrashLoopText, EngineRestartingHint, EngineUnavailableHint, WebViewFailedText, FilterSupported,
  FilterSubtitles, FilterProjects, FilterTexts, FilterAll, DialogFailedHint, FileNotFoundHint, BadUrlHint,
  BadParamsHint, UnknownCommandHint, ShellBusyHint,
};
class ShellText {
 public:
  // Reads <uiDir>\i18n\<es-MX|en-US>.json for `lang` (anything starting with "es" is es-MX).
  void load(const std::wstring& uiDir, std::string_view lang);
  const std::string& lang() const { return lang_; }
  std::wstring get(Str id) const;
  std::string utf8(Str id) const { return toUtf8(get(id)); }

 private:
  std::string lang_ = "en-US";
  std::string table_;  // the flat JSON object of the UI strings, or empty
};
const char* shellStringKey(Str id);

}  // namespace shell
}  // namespace vp
