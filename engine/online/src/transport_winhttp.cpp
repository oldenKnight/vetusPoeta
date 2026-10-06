// The shipped transport on Windows: WinHTTP (system DLL winhttp.dll, part of the OS like WebView2's runtime).
// One session per request (the client sends at most one request per second), system proxy settings honoured
// (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, Windows 8.1+), TLS by the OS, no redirects followed beyond WinHTTP's
// default, response capped at 6 MB. Every handle lives in an RAII wrapper.
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <string>
#include <vector>

#include "vp/online.h"

namespace vp::online {

namespace {

constexpr size_t kMaxBody = 6u * 1024 * 1024;

struct Handle {
  HINTERNET h = nullptr;
  explicit Handle(HINTERNET x) : h(x) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
  explicit operator bool() const { return h != nullptr; }
};

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

std::string narrow(const wchar_t* w, size_t len) {
  if (!len) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), &s[0], n, nullptr, nullptr);
  return s;
}

Result<Response> fail(const char* step, int timeoutMs) {
  const DWORD e = GetLastError();
  std::string why;
  switch (e) {
    case ERROR_WINHTTP_TIMEOUT: why = "timed out after " + std::to_string(timeoutMs) + " ms"; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: why = "no connection (name not resolved)"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: why = "cannot connect to the server"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: why = "connection reset"; break;
    case ERROR_WINHTTP_SECURE_FAILURE: why = "secure connection failed"; break;
    default: why = "network error " + std::to_string(static_cast<unsigned long>(e)); break;
  }
  return Result<Response>(ErrorCode::OnlineFailed, why + " (" + step + ")");
}

class WinHttpTransport : public Transport {
 public:
  Result<Response> get(const std::string& url, int timeoutMs, const Headers& requestHeaders) override {
    try {
      return run(url, timeoutMs, requestHeaders);
    } catch (...) {
      return Result<Response>(ErrorCode::OnlineFailed, "WinHTTP: unexpected failure");
    }
  }

 private:
  Result<Response> run(const std::string& url, int timeoutMs, const Headers& requestHeaders) {
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wurl.empty() || !WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
      return Result<Response>(ErrorCode::BadParams, "WinHTTP: https URL expected");
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    std::wstring agent = L"vetus-poeta";
    std::wstring extra;
    for (const auto& h : requestHeaders) {
      if (h.first == "User-Agent") agent = widen(h.second);
      else extra += widen(h.first + ": " + h.second + "\r\n");
    }
    Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {   // older systems: fall back to the static proxy configuration
      session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
      if (!session) return fail("open", timeoutMs);
    }
    WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    Handle connect(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!connect) return fail("connect", timeoutMs);
    Handle request(WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) return fail("request", timeoutMs);
    if (!extra.empty() &&
        !WinHttpAddRequestHeaders(request.h, extra.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD))
      return fail("headers", timeoutMs);
    const ULONGLONG t0 = GetTickCount64();
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
      return fail("send", timeoutMs);
    if (!WinHttpReceiveResponse(request.h, nullptr)) return fail("receive", timeoutMs);

    Response r;
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
      return fail("status", timeoutMs);
    r.status = static_cast<int>(status);

    DWORD hdrBytes = 0;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hdrBytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (hdrBytes > 0 && hdrBytes < 1024 * 1024) {
      std::vector<wchar_t> raw(hdrBytes / sizeof(wchar_t) + 1, L'\0');
      if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                              &hdrBytes, WINHTTP_NO_HEADER_INDEX)) {
        const std::string all = narrow(raw.data(), hdrBytes / sizeof(wchar_t));
        size_t pos = all.find("\r\n");   // skip the status line
        while (pos != std::string::npos) {
          const size_t next = all.find("\r\n", pos + 2);
          const std::string line = all.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
          const size_t colon = line.find(':');
          if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            for (char& c : name)
              if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            size_t v = colon + 1;
            while (v < line.size() && line[v] == ' ') ++v;
            r.headers[name] = line.substr(v);
          }
          pos = next;
        }
      }
    }

    for (;;) {
      if (GetTickCount64() - t0 > static_cast<ULONGLONG>(timeoutMs) + 2000)
        return Result<Response>(ErrorCode::OnlineFailed, "timed out after " + std::to_string(timeoutMs) + " ms");
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(request.h, &avail)) return fail("read", timeoutMs);
      if (avail == 0) break;
      if (r.body.size() + avail > kMaxBody) return Result<Response>(ErrorCode::OnlineFailed, "response too large");
      const size_t old = r.body.size();
      r.body.resize(old + avail);
      DWORD got = 0;
      if (!WinHttpReadData(request.h, &r.body[old], avail, &got)) return fail("read", timeoutMs);
      r.body.resize(old + got);
      if (got == 0) break;
    }
    return r;
  }
};

}  // namespace

Result<std::unique_ptr<Transport>> makeSystemTransport(const Settings& settings) {
  if (!onlineAllowed(settings))
    return Result<std::unique_ptr<Transport>>(ErrorCode::OnlineDisabled, "online disabled",
                                              "Turn on the online check in Settings first.");
  try {
    return std::unique_ptr<Transport>(std::make_unique<WinHttpTransport>());
  } catch (...) {
    return Result<std::unique_ptr<Transport>>(ErrorCode::Internal, "WinHTTP: out of memory", "Restart the program.");
  }
}

}  // namespace vp::online

#endif  // _WIN32
