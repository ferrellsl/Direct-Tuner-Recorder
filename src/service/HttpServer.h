/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace dt
{

struct HttpServerRequest
{
  std::string method;
  std::string path;  // without the query
  std::string query; // raw, after the ?
  std::map<std::string, std::string> headers; // names in lower case
  std::string body;
  std::string remoteIp;

  // A query parameter, decoded.
  std::string Param(const std::string& name) const;
  std::string Header(const std::string& name) const;
};

// One connection's reply. Either Send() once, or Begin() and then Write() as data arrives.
class HttpServerResponse
{
public:
  explicit HttpServerResponse(intptr_t socket) : m_socket(socket) {}

  void Send(int status, const std::string& contentType, const std::string& body);
  void SendJson(int status, const std::string& json) { Send(status, "application/json", json); }
  void SendError(int status, const std::string& text);
  // Starts a reply whose length isn't known; it ends when the connection closes.
  bool Begin(int status, const std::string& contentType, const std::string& extraHeaders = {});
  bool Write(const void* data, size_t size);
  bool Sent() const { return m_sent; }

private:
  bool SendRaw(const char* data, size_t size);

  intptr_t m_socket;
  bool m_sent = false;
};

using HttpHandler = std::function<void(const HttpServerRequest&, HttpServerResponse&)>;

// A small HTTP/1.1 server: one thread per connection, one request per connection.
class HttpServer
{
public:
  ~HttpServer();

  // allow decides from the caller's address whether to answer at all.
  bool Start(int port, HttpHandler handler, std::function<bool(const std::string& ip)> allow);
  void Stop();
  int Connections() const { return m_active; }

private:
  void Accept();
  void Serve(intptr_t socket, std::string remoteIp);

  intptr_t m_listen = -1;
  HttpHandler m_handler;
  std::function<bool(const std::string&)> m_allow;
  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::atomic<int> m_active{0};
  std::mutex m_mutex;
  std::set<intptr_t> m_sockets;
};

} // namespace dt
