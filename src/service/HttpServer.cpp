/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "HttpServer.h"

#include "../core/Common.h"
#include "../core/Json.h"

#include <chrono>
#include <cstdlib>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dt
{
namespace
{

#ifdef _WIN32
void CloseSocket(intptr_t s)
{
  closesocket(static_cast<SOCKET>(s));
}
void ShutdownSocket(intptr_t s)
{
  shutdown(static_cast<SOCKET>(s), SD_BOTH);
}
#else
void CloseSocket(intptr_t s)
{
  close(static_cast<int>(s));
}
void ShutdownSocket(intptr_t s)
{
  shutdown(static_cast<int>(s), SHUT_RDWR);
}
#endif

bool WaitReadable(intptr_t s, int timeoutMs)
{
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  return select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &timeout) > 0;
}

const char* StatusText(int status)
{
  switch (status)
  {
    case 200:
      return "OK";
    case 206:
      return "Partial Content";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 409:
      return "Conflict";
    case 416:
      return "Range Not Satisfiable";
    case 503:
      return "Service Unavailable";
    default:
      return "Error";
  }
}

} // namespace

std::string HttpServerRequest::Param(const std::string& name) const
{
  size_t pos = 0;
  while (pos <= query.size())
  {
    size_t end = query.find('&', pos);
    if (end == std::string::npos)
      end = query.size();
    const std::string pair = query.substr(pos, end - pos);
    const size_t equals = pair.find('=');
    if (UrlDecode(pair.substr(0, equals)) == name)
      return equals == std::string::npos ? std::string() : UrlDecode(pair.substr(equals + 1));
    pos = end + 1;
  }
  return {};
}

std::string HttpServerRequest::Header(const std::string& name) const
{
  const auto it = headers.find(name);
  return it == headers.end() ? std::string() : it->second;
}

bool HttpServerResponse::SendRaw(const char* data, size_t size)
{
  size_t sent = 0;
  while (sent < size)
  {
    const int count = send(static_cast<decltype(socket(0, 0, 0))>(m_socket), data + sent,
                           static_cast<int>(size - sent), 0);
    if (count <= 0)
      return false;
    sent += static_cast<size_t>(count);
  }
  return true;
}

void HttpServerResponse::Send(int status, const std::string& contentType, const std::string& body)
{
  const std::string head = Format("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                                  "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                                  status, StatusText(status), contentType.c_str(), body.size());
  m_sent = true;
  if (SendRaw(head.data(), head.size()))
    SendRaw(body.data(), body.size());
}

void HttpServerResponse::SendError(int status, const std::string& text)
{
  Json json = Json::Object();
  json.Set("error", text);
  SendJson(status, json.Dump());
}

bool HttpServerResponse::Begin(int status, const std::string& contentType, const std::string& extraHeaders)
{
  const std::string head =
      Format("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\nCache-Control: no-store\r\n",
             status, StatusText(status), contentType.c_str()) +
      extraHeaders + "\r\n";
  m_sent = true;
  return SendRaw(head.data(), head.size());
}

bool HttpServerResponse::Write(const void* data, size_t size)
{
  return SendRaw(static_cast<const char*>(data), size);
}

HttpServer::~HttpServer()
{
  Stop();
}

bool HttpServer::Start(int port, HttpHandler handler, std::function<bool(const std::string&)> allow)
{
  m_handler = std::move(handler);
  m_allow = std::move(allow);
  m_stop = false;

  const auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (static_cast<intptr_t>(s) < 0)
    return false;
  const int on = 1;
#ifndef _WIN32
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
#else
  (void)on;
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(s, 32) != 0)
  {
    CloseSocket(static_cast<intptr_t>(s));
    return false;
  }
  m_listen = static_cast<intptr_t>(s);
  m_thread = std::thread([this] { Accept(); });
  return true;
}

void HttpServer::Stop()
{
  if (m_stop.exchange(true))
    return;
  if (m_listen != -1)
  {
    CloseSocket(m_listen);
    m_listen = -1;
  }
  if (m_thread.joinable())
    m_thread.join();

  // Long replies, such as live TV, end when their sockets do.
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const intptr_t s : m_sockets)
      ShutdownSocket(s);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (m_active > 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void HttpServer::Accept()
{
  while (!m_stop)
  {
    if (!WaitReadable(m_listen, 500))
      continue;
    sockaddr_in from{};
    socklen_t size = sizeof(from);
    const auto s = accept(static_cast<decltype(socket(0, 0, 0))>(m_listen),
                          reinterpret_cast<sockaddr*>(&from), &size);
    if (static_cast<intptr_t>(s) < 0
#ifdef _WIN32
        || s == INVALID_SOCKET
#endif
    )
      continue;
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
    const std::string ip = text;
    if (m_allow && !m_allow(ip))
    {
      Log(LogLevel::Warning, "Refused a connection from %s", ip.c_str());
      CloseSocket(static_cast<intptr_t>(s));
      continue;
    }
    const int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    ++m_active;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_sockets.insert(static_cast<intptr_t>(s));
    }
    std::thread([this, s, ip] { Serve(static_cast<intptr_t>(s), ip); }).detach();
  }
}

void HttpServer::Serve(intptr_t s, std::string remoteIp)
{
  HttpServerRequest request;
  request.remoteIp = std::move(remoteIp);

  std::string data;
  size_t headerEnd = std::string::npos;
  char chunk[8192];
  while (headerEnd == std::string::npos && data.size() < 64 * 1024)
  {
    if (!WaitReadable(s, 10000))
      break;
    const int count = recv(static_cast<decltype(socket(0, 0, 0))>(s), chunk, sizeof(chunk), 0);
    if (count <= 0)
      break;
    data.append(chunk, static_cast<size_t>(count));
    headerEnd = data.find("\r\n\r\n");
  }

  HttpServerResponse response(s);
  if (headerEnd != std::string::npos)
  {
    const std::string head = data.substr(0, headerEnd);
    const size_t lineEnd = head.find("\r\n");
    const std::string line = head.substr(0, lineEnd);
    const size_t space1 = line.find(' ');
    const size_t space2 = line.find(' ', space1 + 1);
    request.method = line.substr(0, space1);
    std::string target = line.substr(space1 + 1, space2 - space1 - 1);
    const size_t question = target.find('?');
    if (question != std::string::npos)
    {
      request.query = target.substr(question + 1);
      target.resize(question);
    }
    request.path = UrlDecode(target);

    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size())
    {
      size_t end = head.find("\r\n", pos);
      if (end == std::string::npos)
        end = head.size();
      const std::string field = head.substr(pos, end - pos);
      const size_t colon = field.find(':');
      if (colon != std::string::npos)
        request.headers[ToLower(Trim(field.substr(0, colon)))] = Trim(field.substr(colon + 1));
      pos = end + 2;
    }

    request.body = data.substr(headerEnd + 4);
    const size_t length = static_cast<size_t>(std::atoll(request.Header("content-length").c_str()));
    while (request.body.size() < length && length < 16 * 1024 * 1024 && WaitReadable(s, 10000))
    {
      const int count = recv(static_cast<decltype(socket(0, 0, 0))>(s), chunk, sizeof(chunk), 0);
      if (count <= 0)
        break;
      request.body.append(chunk, static_cast<size_t>(count));
    }

    try
    {
      m_handler(request, response);
    }
    catch (const std::exception& e)
    {
      Log(LogLevel::Error, "Request %s %s failed: %s", request.method.c_str(), request.path.c_str(), e.what());
    }
    if (!response.Sent())
      response.SendError(404, "Not found");
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sockets.erase(s);
  }
  ShutdownSocket(s);
  CloseSocket(s);
  --m_active;
}

} // namespace dt
