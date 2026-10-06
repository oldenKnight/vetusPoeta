// Single include point for the WebView2 declarations: the real SDK header when CMake could download
// it (VP_WEBVIEW2_HAVE_SDK=1), else the checked-in subset with identical names (tools/webview2_subset.py).
#pragma once
#include <windows.h>

#if defined(__GNUC__)
#pragma GCC system_header
#endif

#if VP_WEBVIEW2_HAVE_SDK
#include <WebView2.h>
#else
#include "webview2_subset.h"
#endif

// Loader entry points. MSVC links WebView2LoaderStatic.lib (VP_WEBVIEW2_STATIC=1) and calls them
// directly (declared by WebView2.h); MinGW resolves them from WebView2Loader.dll with GetProcAddress.
typedef HRESULT(STDAPICALLTYPE* VpCreateEnvironmentFn)(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder, ICoreWebView2EnvironmentOptions* options,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);
typedef HRESULT(STDAPICALLTYPE* VpGetVersionFn)(PCWSTR browserExecutableFolder, LPWSTR* versionInfo);
