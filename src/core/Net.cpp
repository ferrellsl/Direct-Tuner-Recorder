/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Net.h"

#include "Common.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#if !defined(__ANDROID__) || __ANDROID_API__ >= 24
#include <ifaddrs.h>
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dt
{
namespace
{

#ifdef _WIN32
using Socket = SOCKET;
void CloseSocket(intptr_t s)
{
  closesocket(static_cast<SOCKET>(s));
}
bool WouldBlock()
{
  const int error = WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
void SetNonBlocking(Socket s)
{
  u_long mode = 1;
  ioctlsocket(s, FIONBIO, &mode);
}
#else
using Socket = int;
void CloseSocket(intptr_t s)
{
  close(static_cast<int>(s));
}
bool WouldBlock()
{
  return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
}
void SetNonBlocking(Socket s)
{
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}
#endif

bool WaitSocket(Socket s, bool forWrite, int timeoutMs)
{
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval timeout;
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  const int result = select(static_cast<int>(s) + 1, forWrite ? nullptr : &set,
                            forWrite ? &set : nullptr, nullptr, &timeout);
  return result > 0;
}

struct Url
{
  std::string host;
  std::string port = "80";
  std::string path = "/";
};

bool ParseUrl(const std::string& url, Url& out)
{
  const std::string prefix = "http://";
  if (url.compare(0, prefix.size(), prefix) != 0)
    return false;
  const size_t hostStart = prefix.size();
  const size_t slash = url.find('/', hostStart);
  std::string authority =
      slash == std::string::npos ? url.substr(hostStart) : url.substr(hostStart, slash - hostStart);
  if (slash != std::string::npos)
    out.path = url.substr(slash);
  const size_t colon = authority.find(':');
  if (colon != std::string::npos)
  {
    out.port = authority.substr(colon + 1);
    authority.resize(colon);
  }
  out.host = authority;
  return !out.host.empty();
}

bool Connect(const Url& url, int timeoutMs, Socket& out)
{
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* list = nullptr;
  if (getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &list) != 0 || !list)
    return false;

  Socket s = socket(list->ai_family, list->ai_socktype, list->ai_protocol);
  bool ok = static_cast<intptr_t>(s) >= 0;
#ifdef _WIN32
  ok = s != INVALID_SOCKET;
#endif
  if (ok)
  {
    SetNonBlocking(s);
    if (connect(s, list->ai_addr, static_cast<int>(list->ai_addrlen)) != 0)
    {
      ok = WouldBlock() && WaitSocket(s, true, timeoutMs);
      if (ok)
      {
        int error = 0;
        socklen_t size = sizeof(error);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size);
        ok = error == 0;
      }
    }
    if (!ok)
      CloseSocket(s);
  }
  freeaddrinfo(list);
  if (ok)
  {
    // A big receive buffer rides out short stalls of the writing thread.
    int size = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    out = s;
  }
  return ok;
}

bool SendAll(Socket s, const std::string& data, int timeoutMs)
{
  size_t sent = 0;
  while (sent < data.size())
  {
    const int count = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
    if (count > 0)
    {
      sent += static_cast<size_t>(count);
      continue;
    }
    if (!WouldBlock() || !WaitSocket(s, true, timeoutMs))
      return false;
  }
  return true;
}

uint32_t Crc32(const uint8_t* data, size_t size)
{
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i)
  {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

std::vector<uint32_t> LocalIpv4Addresses()
{
  std::vector<uint32_t> addresses;
#ifdef _WIN32
  ULONG size = 32 * 1024;
  std::vector<uint8_t> buffer(size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) != NO_ERROR)
    return addresses;
  for (auto* adapter = adapters; adapter; adapter = adapter->Next)
  {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
      continue;
    for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next)
      addresses.push_back(
          reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr)->sin_addr.s_addr);
  }
#elif defined(__ANDROID__) && __ANDROID_API__ < 24
  // getifaddrs arrived in API 24: one socket on any interface still reaches the local network.
  addresses.push_back(htonl(INADDR_ANY));
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0)
    return addresses;
  for (auto* item = list; item; item = item->ifa_next)
    if (item->ifa_addr && item->ifa_addr->sa_family == AF_INET)
      addresses.push_back(reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr);
  freeifaddrs(list);
#endif
  return addresses;
}

} // namespace

void NetInit()
{
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

HttpStream::~HttpStream()
{
  Close();
}

void HttpStream::Close()
{
  const intptr_t s = m_socket.exchange(-1);
  if (s != -1)
    CloseSocket(s);
}

bool HttpStream::Open(const std::string& url, int timeoutMs, const std::string& method,
                      const std::string& extraHeaders, const std::string& body)
{
  Close();
  m_status = 0;
  m_deviceError = 0;
  m_headers.clear();
  m_pending.clear();
  m_pendingOffset = 0;

  Url parsed;
  if (!ParseUrl(url, parsed))
    return false;

  Socket s;
  if (!Connect(parsed, 3000, s))
    return false;
  m_socket = static_cast<intptr_t>(s);

  sockaddr_in local{};
  socklen_t localSize = sizeof(local);
  if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &localSize) == 0)
  {
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
    m_localAddress = text;
  }

  std::string request = method + " " + parsed.path + " HTTP/1.1\r\nHost: " + parsed.host + ":" +
                        parsed.port + "\r\nUser-Agent: Kodi-pvr.directtuner\r\nConnection: close\r\n" +
                        extraHeaders;
  if (!body.empty() || method == "POST" || method == "PUT")
    request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  request += "\r\n" + body;
  if (!SendAll(s, request, 3000))
  {
    Close();
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  std::string header;
  uint8_t chunk[4096];
  size_t headerEnd = std::string::npos;
  while (headerEnd == std::string::npos)
  {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0 || header.size() > 64 * 1024 || !WaitSocket(s, false, static_cast<int>(remaining)))
    {
      Close();
      return false;
    }
    const int count = recv(s, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
    if (count <= 0)
    {
      if (count < 0 && WouldBlock())
        continue;
      Close();
      return false;
    }
    header.append(reinterpret_cast<char*>(chunk), static_cast<size_t>(count));
    headerEnd = header.find("\r\n\r\n");
  }

  const size_t space = header.find(' ');
  if (space != std::string::npos)
    m_status = std::atoi(header.c_str() + space + 1);

  const std::string lower = ToLower(header.substr(0, headerEnd));
  m_headers = "\r\n" + lower + "\r\n";
  const size_t errorPos = lower.find("x-hdhomerun-error:");
  if (errorPos != std::string::npos)
    m_deviceError = std::atoi(header.c_str() + errorPos + 18);

  const size_t bodyStart = headerEnd + 4;
  m_pending.assign(header.begin() + static_cast<std::ptrdiff_t>(bodyStart), header.end());
  return true;
}

std::string HttpStream::Header(const std::string& name) const
{
  const size_t pos = m_headers.find("\r\n" + name + ":");
  if (pos == std::string::npos)
    return {};
  const size_t start = pos + name.size() + 3;
  return Trim(m_headers.substr(start, m_headers.find("\r\n", start) - start));
}

int HttpStream::Read(uint8_t* buffer, size_t size, int timeoutMs)
{
  if (m_pendingOffset < m_pending.size())
  {
    const size_t count = std::min(size, m_pending.size() - m_pendingOffset);
    std::memcpy(buffer, m_pending.data() + m_pendingOffset, count);
    m_pendingOffset += count;
    return static_cast<int>(count);
  }

  const intptr_t value = m_socket.load();
  if (value == -1)
    return -1;
  const auto s = static_cast<Socket>(value);
  if (!WaitSocket(s, false, timeoutMs))
    return m_socket.load() == -1 ? -1 : 0;
  const int count = recv(s, reinterpret_cast<char*>(buffer), static_cast<int>(size), 0);
  if (count > 0)
    return count;
  if (count < 0 && WouldBlock())
    return 0;
  return -1;
}

bool HttpGetText(const std::string& url, int timeoutMs, std::string& body, int* status)
{
  HttpStream stream;
  if (!stream.Open(url, timeoutMs))
    return false;
  if (status)
    *status = stream.Status();

  body.clear();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  uint8_t chunk[16 * 1024];
  while (std::chrono::steady_clock::now() < deadline)
  {
    const int count = stream.Read(chunk, sizeof(chunk), 500);
    if (count < 0)
      return stream.Status() == 200;
    body.append(reinterpret_cast<char*>(chunk), static_cast<size_t>(count));
  }
  return false;
}

bool HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                 int timeoutMs, int& status, std::string& reply)
{
  HttpStream stream;
  status = 0;
  reply.clear();
  if (!stream.Open(url, timeoutMs, method, {}, body))
    return false;
  status = stream.Status();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  uint8_t chunk[16 * 1024];
  while (std::chrono::steady_clock::now() < deadline)
  {
    const int count = stream.Read(chunk, sizeof(chunk), 500);
    if (count < 0)
      return true;
    reply.append(reinterpret_cast<char*>(chunk), static_cast<size_t>(count));
  }
  return false;
}

namespace
{

// Sends packet as a broadcast from every local address and collects the replies.
template <typename OnReply>
void Broadcast(const std::vector<uint8_t>& packet, int port, int timeoutMs, OnReply onReply)
{
  std::vector<Socket> sockets;
  for (const uint32_t address : LocalIpv4Addresses())
  {
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = address;
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
    {
      CloseSocket(s);
      continue;
    }
    SetNonBlocking(s);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(static_cast<uint16_t>(port));
    target.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(s, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
           reinterpret_cast<sockaddr*>(&target), sizeof(target));
    sockets.push_back(s);
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!sockets.empty() && std::chrono::steady_clock::now() < deadline)
  {
    fd_set set;
    FD_ZERO(&set);
    int highest = 0;
    for (const Socket s : sockets)
    {
      FD_SET(s, &set);
      highest = std::max(highest, static_cast<int>(s));
    }
    timeval timeout{0, 100 * 1000};
    if (select(highest + 1, &set, nullptr, nullptr, &timeout) <= 0)
      continue;
    for (const Socket s : sockets)
    {
      if (!FD_ISSET(s, &set))
        continue;
      uint8_t reply[2048];
      sockaddr_in from{};
      socklen_t fromSize = sizeof(from);
      const int count = recvfrom(s, reinterpret_cast<char*>(reply), sizeof(reply), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromSize);
      if (count <= 0)
        continue;
      char text[INET_ADDRSTRLEN] = {};
      inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
      onReply(reply, count, std::string(text));
    }
  }

  for (const Socket s : sockets)
    CloseSocket(s);
}

} // namespace

std::vector<std::string> DiscoverRecorders(int timeoutMs)
{
  const std::string request = kRecorderDiscoverRequest;
  const std::string prefix = kRecorderDiscoverReply;
  std::vector<std::string> found;
  Broadcast(std::vector<uint8_t>(request.begin(), request.end()), kRecorderDiscoveryPort, timeoutMs,
            [&](const uint8_t* data, int size, const std::string& from) {
              const std::string text(reinterpret_cast<const char*>(data), static_cast<size_t>(size));
              if (text.compare(0, prefix.size(), prefix) != 0)
                return;
              // The reply carries the port; the address is wherever it came from.
              const std::string url = "http://" + from + ":" + Trim(text.substr(prefix.size()));
              if (std::find(found.begin(), found.end(), url) == found.end())
                found.push_back(url);
            });
  return found;
}

std::vector<std::string> DiscoverDevices(int timeoutMs)
{
  // Type 2 is a discover request. Tag 1 is the device type (1 = tuner) and tag 2 the device ID
  // (all ones = any). The packet ends with a little-endian CRC.
  std::vector<uint8_t> packet = {0x00, 0x02, 0x00, 0x0C, 0x01, 0x04, 0x00, 0x00,
                                 0x00, 0x01, 0x02, 0x04, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint32_t crc = Crc32(packet.data(), packet.size());
  for (int shift = 0; shift < 32; shift += 8)
    packet.push_back(static_cast<uint8_t>(crc >> shift));

  std::vector<Socket> sockets;
  for (const uint32_t address : LocalIpv4Addresses())
  {
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = address;
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
    {
      CloseSocket(s);
      continue;
    }
    SetNonBlocking(s);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(65001);
    target.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(s, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
           reinterpret_cast<sockaddr*>(&target), sizeof(target));
    sockets.push_back(s);
  }

  std::vector<std::string> found;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!sockets.empty() && std::chrono::steady_clock::now() < deadline)
  {
    fd_set set;
    FD_ZERO(&set);
    int highest = 0;
    for (const Socket s : sockets)
    {
      FD_SET(s, &set);
      highest = std::max(highest, static_cast<int>(s));
    }
    timeval timeout{0, 100 * 1000};
    if (select(highest + 1, &set, nullptr, nullptr, &timeout) <= 0)
      continue;
    for (const Socket s : sockets)
    {
      if (!FD_ISSET(s, &set))
        continue;
      uint8_t reply[2048];
      sockaddr_in from{};
      socklen_t fromSize = sizeof(from);
      const int count = recvfrom(s, reinterpret_cast<char*>(reply), sizeof(reply), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromSize);
      if (count < 8 || reply[0] != 0x00 || reply[1] != 0x03)
        continue;
      char text[INET_ADDRSTRLEN] = {};
      inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
      if (std::find(found.begin(), found.end(), text) == found.end())
        found.push_back(text);
    }
  }

  for (const Socket s : sockets)
    CloseSocket(s);
  return found;
}

} // namespace dt
