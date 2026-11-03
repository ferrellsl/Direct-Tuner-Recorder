/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace dt
{

void NetInit();

// Plain http GET to a device on the LAN. Https goes through the host's fetcher instead.
bool HttpGetText(const std::string& url, int timeoutMs, std::string& body, int* status = nullptr);

class HttpStream
{
public:
  HttpStream() = default;
  ~HttpStream();
  HttpStream(const HttpStream&) = delete;
  HttpStream& operator=(const HttpStream&) = delete;

  // Connects and reads the response headers. The tuner can take 10 seconds to answer.
  // extraHeaders are whole lines, each ending in \r\n.
  bool Open(const std::string& url, int timeoutMs, const std::string& method = "GET",
            const std::string& extraHeaders = {}, const std::string& body = {});
  // Bytes read, 0 on a timeout, or -1 when the connection has ended.
  int Read(uint8_t* buffer, size_t size, int timeoutMs);
  void Close();

  int Status() const { return m_status; }
  // The X-HDHomeRun-Error code, such as 805 for all tuners in use.
  int DeviceError() const { return m_deviceError; }
  const std::string& LocalAddress() const { return m_localAddress; }
  // A response header's value, or empty. The name is lower case.
  std::string Header(const std::string& name) const;

private:
  std::atomic<intptr_t> m_socket{-1};
  int m_status = 0;
  std::string m_headers; // lower case
  int m_deviceError = 0;
  std::string m_localAddress;
  std::vector<uint8_t> m_pending;
  size_t m_pendingOffset = 0;
};

// A whole request and reply, for small JSON calls. False when the server can't be reached;
// otherwise status holds the HTTP status.
bool HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                 int timeoutMs, int& status, std::string& reply);

// Finds tuners with the HDHomeRun UDP discovery broadcast. Returns their IP addresses.
std::vector<std::string> DiscoverDevices(int timeoutMs);

// The recorder service answers this broadcast with its address, as "http://ip:port". A low port:
// Windows reserves random blocks of high ones for Hyper-V.
constexpr int kRecorderDiscoveryPort = 8866;
constexpr const char* kRecorderDiscoverRequest = "DIRECTTUNER-RECORDER?";
constexpr const char* kRecorderDiscoverReply = "DIRECTTUNER-RECORDER ";
std::vector<std::string> DiscoverRecorders(int timeoutMs);

} // namespace dt
