/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "Backend.h"
#include "Json.h"

namespace dt
{

// The recorder service, reached over HTTP. It owns the tuner, guide, timers and recordings;
// this side only asks and listens for changes.
class RemoteBackend : public IBackend
{
public:
  RemoteBackend(IBackendHost& host, std::string baseUrl);
  ~RemoteBackend() override;

  // True if a recorder answers at baseUrl.
  static bool Probe(const std::string& baseUrl, std::string* name = nullptr);

  void Start() override;
  void Stop() override;
  void Refresh() override;

  bool Connected() const override { return m_connected; }
  std::string Name() const override { return "Direct Tuner Recorder"; }
  std::string Version() const override;
  std::string Hostname() const override { return m_baseUrl; }
  std::string ConnectionText() const override;

  std::vector<Channel> Channels() const override;
  std::map<int, std::string> ChannelIcons() const override;
  bool LookupChannel(int uid, Channel& out) const override;
  bool Signal(int channelUid, SignalInfo& out) override;

  std::vector<GuideEvent> GetEvents(int channelUid, time_t start, time_t end) override;
  bool FindEvent(int channelUid, unsigned int eventUid, GuideEvent& out) override;

  std::vector<Timer> GetTimers() override;
  unsigned int AddTimer(Timer timer, std::string& error) override;
  bool UpdateTimer(const Timer& timer) override;
  bool DeleteTimer(unsigned int id) override;

  std::vector<RecordingInfo> GetRecordings() override;
  bool DeleteRecording(const std::string& id, std::string& error) override;
  int64_t RecordingSize(const std::string& id) override;
  bool DriveSpace(uint64_t& total, uint64_t& used) override;
  std::unique_ptr<MediaReader> OpenRecording(const std::string& id) override;

  std::unique_ptr<LiveHandle> OpenLive(int channelUid, std::shared_ptr<IStreamSink> sink,
                                       std::string& error) override;

private:
  // Sends a request and parses the JSON reply. False when the service can't be reached or
  // answers with an error, whose text goes in error.
  bool Call(const std::string& method, const std::string& path, const std::string& body, Json& reply,
            std::string* error = nullptr, int timeoutMs = 10000);
  bool LoadInfo();
  bool LoadChannels();
  void Run();
  void SetConnected(bool connected, const std::string& text);

  IBackendHost& m_host;
  std::string m_baseUrl;

  std::atomic<bool> m_connected{false};
  mutable std::mutex m_mutex;
  std::string m_version;
  std::string m_tunerText;
  std::vector<Channel> m_channels;
  std::map<int, std::string> m_icons;

  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_refresh{false};
};

} // namespace dt
