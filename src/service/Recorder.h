/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "../core/Backend.h"
#include "../core/Json.h"
#include "HttpServer.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dt
{

constexpr const char* kRecorderVersion = "1.0.0";

struct RecorderConfig
{
  BackendSettings backend;
  int port = 8866;
  bool allowNetwork = true;

  // Reads recorder.json, filling in defaults for anything missing, and writes it back so the
  // file always shows every setting.
  static RecorderConfig Load(const std::string& path, const std::string& defaultRecordings);
};

class LiveRelay;

// The recorder service: the backend, its HTTP API and the discovery responder.
class Recorder : public IBackendHost
{
public:
  Recorder(RecorderConfig config, std::string dataFolder);
  ~Recorder() override;

  bool Start(std::string& error);
  void Stop();
  // After the computer wakes.
  void Resume();

  // For keeping the computer awake and waking it.
  bool Busy() const;
  time_t NextRecordingStart() const;

  // IBackendHost
  void Notify(bool isError, const std::string& text) override;
  void ConnectionChanged(bool connected, const std::string& text) override;
  void ChannelsChanged() override { Push("channels"); }
  void GuideChanged() override { Push("guide"); }
  void TimersChanged() override { Push("timers"); }
  void RecordingsChanged() override { Push("recordings"); }
  void LiveStopped(int channelUid, const std::string& reason) override;

private:
  void Handle(const HttpServerRequest& request, HttpServerResponse& response);
  void ServeEvents(const HttpServerRequest& request, HttpServerResponse& response);
  void ServeLive(int channelUid, HttpServerResponse& response);
  void ServeRecording(const HttpServerRequest& request, HttpServerResponse& response);
  void Push(const std::string& type, Json extra = Json::Object());
  void Discovery();
  bool Allowed(const std::string& ip) const;

  RecorderConfig m_config;
  std::string m_dataFolder;
  std::unique_ptr<Backend> m_backend;
  HttpServer m_server;

  std::mutex m_eventMutex;
  std::condition_variable m_eventReady;
  long long m_seq = 0;
  std::deque<std::pair<long long, Json>> m_events;
  bool m_stopping = false;

  std::mutex m_liveMutex;
  std::vector<std::weak_ptr<LiveRelay>> m_relays;

  std::thread m_discovery;
  std::atomic<bool> m_stopDiscovery{false};
  intptr_t m_discoverySocket = -1;
};

} // namespace dt
