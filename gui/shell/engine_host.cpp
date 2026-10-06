// Engine process host (see engine_host.h).
#include "engine_host.h"

#include <cstdio>
#include <vector>

#include "host_logic.h"

namespace vp {
namespace shell {

// ----------------------------------------------------------------------------- LogFile

void LogFile::open(const std::wstring& path) {
  std::lock_guard<std::mutex> lock(mu_);
  path_ = path;
  reopenLocked();
}

void LogFile::reopenLocked() {
  f_.reset(CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  size_ = 0;
  LARGE_INTEGER sz;
  if (f_ && GetFileSizeEx(f_.get(), &sz)) { size_ = uint64_t(sz.QuadPart); }
}

void LogFile::write(const char* data, size_t n) {
  std::lock_guard<std::mutex> lock(mu_);
  if (path_.empty()) { return; }
  if (logNeedsRotation(size_, n)) {
    f_.reset();
    for (const auto& op : logRotationPlan(path_)) {
      if (op.second.empty()) { DeleteFileW(op.first.c_str()); }
      else { MoveFileExW(op.first.c_str(), op.second.c_str(), MOVEFILE_REPLACE_EXISTING); }
    }
    reopenLocked();
  }
  if (!f_) { return; }
  while (n > 0) {
    DWORD chunk = DWORD(n > (1u << 20) ? (1u << 20) : n);
    DWORD put = 0;
    if (!WriteFile(f_.get(), data, chunk, &put, nullptr) || put == 0) { return; }
    size_ += put;
    data += put;
    n -= put;
  }
}

void LogFile::note(std::string_view text) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  char head[64];
  int k = std::snprintf(head, sizeof head, "%04u-%02u-%02u %02u:%02u:%02u.%03u [shell] ", unsigned(t.wYear),
                        unsigned(t.wMonth), unsigned(t.wDay), unsigned(t.wHour), unsigned(t.wMinute),
                        unsigned(t.wSecond), unsigned(t.wMilliseconds));
  std::string line(head, k > 0 ? size_t(k) : 0);
  line.append(text.data(), text.size() > kLogLineCap ? kLogLineCap : text.size());
  line += "\r\n";
  write(line.data(), line.size());
}

// -------------------------------------------------------------------------- EngineHost

namespace {

// An anonymous pipe whose child end is inheritable and whose parent end is not.
bool makePipe(UniqueHandle& parentEnd, UniqueHandle& childEnd, bool childReads, DWORD size) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;
  HANDLE r = nullptr, w = nullptr;
  if (!CreatePipe(&r, &w, &sa, size)) { return false; }
  UniqueHandle rh(r), wh(w);
  UniqueHandle& parent = childReads ? wh : rh;
  if (!SetHandleInformation(parent.get(), HANDLE_FLAG_INHERIT, 0)) { return false; }
  parentEnd = childReads ? std::move(wh) : std::move(rh);
  childEnd = childReads ? std::move(rh) : std::move(wh);
  return true;
}

class AttributeList {
 public:
  explicit AttributeList(DWORD count) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, count, 0, &size);
    buf_.resize(size);
    list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf_.data());
    if (!InitializeProcThreadAttributeList(list_, count, 0, &size)) { list_ = nullptr; }
  }
  ~AttributeList() { if (list_) { DeleteProcThreadAttributeList(list_); } }
  AttributeList(const AttributeList&) = delete;
  AttributeList& operator=(const AttributeList&) = delete;
  LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

 private:
  std::vector<unsigned char> buf_;
  LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

}  // namespace

bool EngineHost::start(const std::wstring& exePath, const std::wstring& args, DWORD* winError) {
  kill();
  auto fail = [&]() {
    if (winError) { *winError = GetLastError(); }
    job_.reset(); proc_.reset(); stdinW_.reset(); stdoutR_.reset(); stderrR_.reset();
    return false;
  };
  UniqueHandle childIn, childOut, childErr;
  if (!makePipe(stdinW_, childIn, true, 64 << 10) || !makePipe(stdoutR_, childOut, false, 1 << 20) ||
      !makePipe(stderrR_, childErr, false, 64 << 10)) {
    return fail();
  }

  // Kill-on-close job: the engine (and anything it starts) dies with the shell or on kill().
  job_.reset(CreateJobObjectW(nullptr, nullptr));
  if (!job_) { return fail(); }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
  lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &lim, sizeof lim);

  // Only the three pipe ends are inherited, nothing else the shell has open.
  HANDLE inherit[3] = {childIn.get(), childOut.get(), childErr.get()};
  AttributeList attrs(1);
  if (!attrs.get() || !UpdateProcThreadAttribute(attrs.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                 sizeof inherit, nullptr, nullptr)) {
    return fail();
  }
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = childIn.get();
  si.StartupInfo.hStdOutput = childOut.get();
  si.StartupInfo.hStdError = childErr.get();
  si.lpAttributeList = attrs.get();

  std::wstring cmd = L"\"" + exePath + L"\"";
  if (!args.empty()) { cmd += L" " + args; }
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(L'\0');
  std::wstring dir = exePath.substr(0, exePath.find_last_of(L"\\/"));
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exePath.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                      nullptr, dir.empty() ? nullptr : dir.c_str(), &si.StartupInfo, &pi)) {
    return fail();
  }
  proc_.reset(pi.hProcess);
  UniqueHandle thread(pi.hThread);
  if (!AssignProcessToJobObject(job_.get(), proc_.get())) {
    // Still usable without the job (e.g. restrictive parent job); kill() terminates the process.
    log_.note("engine: could not assign the process to a job object");
  }
  ResumeThread(thread.get());
  // The child owns its ends now; closing ours lets reads see EOF when it exits.
  childIn.reset();
  childOut.reset();
  childErr.reset();

  ++gen_;
  stopping_ = false;
  {
    std::lock_guard<std::mutex> lock(outMu_);
    outQ_.clear(); outBytes_ = 0; posted_ = false;
  }
  {
    std::lock_guard<std::mutex> lock(inMu_);
    inQ_.clear(); inBytes_ = 0; closeStdin_ = false;
  }
  unsigned gen = gen_;
  log_.note("engine started, pid " + std::to_string(pi.dwProcessId));
  if (!outThread_.start([this, gen] { readStdout(gen); }) || !errThread_.start([this] { readStderr(); }) ||
      !inThread_.start([this] { writeStdin(); })) {
    kill();
    if (winError) { *winError = ERROR_NOT_ENOUGH_MEMORY; }
    return false;
  }
  return true;
}

void EngineHost::readStdout(unsigned gen) {
  LineSplitter splitter;
  std::vector<char> buf(64 << 10);
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(stdoutR_.get(), buf.data(), DWORD(buf.size()), &got, nullptr) || got == 0) { break; }
    bool stop = false;
    splitter.feed(buf.data(), got, [&](std::string_view line) {
      if (stop) { return; }
      std::unique_lock<std::mutex> lock(outMu_);
      // Bounded: block while the UI thread has not drained what is already queued.
      outCv_.wait(lock, [&] { return stopping_ || (outBytes_ < kOutMaxBytes && outQ_.size() < kOutMaxLines); });
      if (stopping_) { stop = true; return; }
      outQ_.emplace_back(line);
      outBytes_ += line.size();
      if (!posted_) {
        posted_ = true;
        PostMessageW(hwnd_, msgOutput_, WPARAM(gen), 0);
      }
    });
    if (stop) { return; }
  }
  if (splitter.dropped() > 0) { log_.note("engine: dropped over-long stdout lines: " + std::to_string(splitter.dropped())); }
  if (!stopping_) { PostMessageW(hwnd_, msgExited_, WPARAM(gen), 0); }
}

void EngineHost::readStderr() {
  std::vector<char> buf(16 << 10);
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(stderrR_.get(), buf.data(), DWORD(buf.size()), &got, nullptr) || got == 0) { break; }
    log_.write(buf.data(), got);
  }
}

void EngineHost::writeStdin() {
  for (;;) {
    std::string line;
    {
      std::unique_lock<std::mutex> lock(inMu_);
      inCv_.wait(lock, [&] { return stopping_ || closeStdin_ || !inQ_.empty(); });
      if (stopping_) { return; }
      if (inQ_.empty()) {  // closeStdin_ with nothing left: signal EOF to the engine
        stdinW_.reset();
        return;
      }
      line = std::move(inQ_.front());
      inQ_.pop_front();
      inBytes_ -= line.size();
    }
    line.push_back('\n');
    const char* p = line.data();
    size_t n = line.size();
    while (n > 0) {
      DWORD put = 0;
      if (!WriteFile(stdinW_.get(), p, DWORD(n), &put, nullptr) || put == 0) { return; }  // engine gone
      p += put;
      n -= put;
    }
  }
}

bool EngineHost::send(std::string line) {
  if (!proc_.valid()) { return false; }
  std::lock_guard<std::mutex> lock(inMu_);
  if (closeStdin_ || inQ_.size() >= kInMaxLines || inBytes_ + line.size() > kInMaxBytes) { return false; }
  inBytes_ += line.size();
  inQ_.push_back(std::move(line));
  inCv_.notify_one();
  return true;
}

void EngineHost::takeOutput(std::deque<std::string>& out) {
  std::lock_guard<std::mutex> lock(outMu_);
  out.swap(outQ_);
  outQ_.clear();
  outBytes_ = 0;
  posted_ = false;
  outCv_.notify_all();
}

bool EngineHost::processExited() const {
  return !proc_.valid() || WaitForSingleObject(proc_.get(), 0) == WAIT_OBJECT_0;
}

void EngineHost::shutdown(const std::string& shutdownLine, DWORD waitMs) {
  if (!proc_.valid()) { return; }
  send(shutdownLine);
  {
    std::lock_guard<std::mutex> lock(inMu_);
    closeStdin_ = true;
    inCv_.notify_all();
  }
  if (WaitForSingleObject(proc_.get(), waitMs) == WAIT_OBJECT_0) {
    log_.note("engine exited cleanly");
  } else {
    log_.note("engine did not exit in time; terminating");
  }
  kill();
}

void EngineHost::kill() {
  if (proc_.valid() && WaitForSingleObject(proc_.get(), 0) != WAIT_OBJECT_0) {
    if (job_) { TerminateJobObject(job_.get(), 1); }
    TerminateProcess(proc_.get(), 1);
    WaitForSingleObject(proc_.get(), 5000);
  }
  stopThreads();
  proc_.reset();
  job_.reset();  // kill-on-close takes any leftover child process with it
  stdinW_.reset();
  stdoutR_.reset();
  stderrR_.reset();
}

void EngineHost::stopThreads() {
  stopping_ = true;
  {
    std::lock_guard<std::mutex> lock(outMu_);
    outCv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> lock(inMu_);
    inCv_.notify_all();
  }
  // Reads end with EOF once the job is gone; cancelling covers a grandchild that still
  // holds a pipe end, and a writer blocked on a full stdin pipe.
  outThread_.joinCancellingIo();
  errThread_.joinCancellingIo();
  inThread_.joinCancellingIo();
  std::lock_guard<std::mutex> lock(outMu_);
  outQ_.clear();
  outBytes_ = 0;
  posted_ = false;
}

}  // namespace shell
}  // namespace vp
