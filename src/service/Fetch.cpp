/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Fetch.h"

#include "../core/Common.h"
#include "../core/Net.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>

#include <mutex>
#endif

namespace dt
{

#ifdef _WIN32

namespace
{

std::wstring Widen(const std::string& text)
{
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring out(static_cast<size_t>(size > 0 ? size - 1 : 0), L'\0');
  if (size > 1)
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
  return out;
}

} // namespace

bool FetchUrl(const std::string& url, std::string& body)
{
  if (url.rfind("http://", 0) == 0)
    return HttpGetText(url, 30000, body);

  const std::wstring wide = Widen(url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {};
  wchar_t path[4096] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 4096;
  wchar_t extra[4096] = {};
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = 4096;
  if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts))
    return false;

  bool ok = false;
  HINTERNET session = WinHttpOpen(L"DirectTunerRecorder/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connection = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
  const std::wstring target = std::wstring(path) + extra;
  HINTERNET request =
      connection ? WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                 : nullptr;
  if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr))
  {
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    body.clear();
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
      std::string chunk(available, '\0');
      DWORD read = 0;
      if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
        break;
      body.append(chunk.data(), read);
    }
    ok = status == 200 && !body.empty();
  }
  if (request)
    WinHttpCloseHandle(request);
  if (connection)
    WinHttpCloseHandle(connection);
  if (session)
    WinHttpCloseHandle(session);
  return ok;
}

#else

namespace
{

size_t Append(char* data, size_t size, size_t count, void* target)
{
  static_cast<std::string*>(target)->append(data, size * count);
  return size * count;
}

} // namespace

bool FetchUrl(const std::string& url, std::string& body)
{
  if (url.rfind("http://", 0) == 0)
    return HttpGetText(url, 30000, body);

  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  CURL* curl = curl_easy_init();
  if (!curl)
    return false;
  body.clear();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Append);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "DirectTunerRecorder/1.0");
  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  // The URL carries the tuner's DeviceAuth, so it never goes in the log.
  if (result != CURLE_OK)
    Log(LogLevel::Warning, "Guide download failed: %s", curl_easy_strerror(result));
  return result == CURLE_OK && status == 200 && !body.empty();
}

#endif

} // namespace dt
