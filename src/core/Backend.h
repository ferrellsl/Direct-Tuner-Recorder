/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "Device.h"
#include "Guide.h"
#include "Pump.h"
#include "RecordingStore.h"
#include "Scheduler.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dt
{

// Told about changes, by whichever backend is in use.
class IBackendHost
{
public:
  virtual ~IBackendHost() = default;
  virtual void Notify(bool isError, const std::string& text) = 0;
  virtual void ConnectionChanged(bool connected, const std::string& text) = 0;
  virtual void ChannelsChanged() = 0;
  virtual void GuideChanged() = 0;
  virtual void TimersChanged() = 0;
  virtual void RecordingsChanged() = 0;
  // Live TV on this channel was stopped, for example because a recording needed the tuner.
  virtual void LiveStopped(int channelUid, const std::string& reason) = 0;
};

// Keeps live TV flowing into a sink until destroyed.
class LiveHandle
{
public:
  virtual ~LiveHandle() = default;
};

// Reads a recording, waiting at the end while it's still being recorded.
class MediaReader
{
public:
  virtual ~MediaReader() = default;
  // Bytes read, 0 at the end, -1 on an error.
  virtual int Read(uint8_t* buffer, size_t size) = 0;
  virtual int64_t Seek(int64_t position, int whence) = 0;
  virtual int64_t Length() = 0;
};

struct SignalInfo
{
  std::string adapter;
  std::string status;
  int strength = 0; // percent
  int quality = 0;
};

// Everything a front end needs: the Kodi add-on talks to this, whether the work happens in its
// own process (Backend) or in the recorder service (RemoteBackend).
class IBackend
{
public:
  virtual ~IBackend() = default;

  virtual void Start() = 0;
  virtual void Stop() = 0;
  // After the computer wakes: reconnect and reread what may have changed.
  virtual void Refresh() = 0;

  virtual bool Connected() const = 0;
  virtual std::string Name() const = 0;
  virtual std::string Version() const = 0;
  virtual std::string Hostname() const = 0;
  virtual std::string ConnectionText() const = 0;

  virtual std::vector<Channel> Channels() const = 0;
  virtual std::map<int, std::string> ChannelIcons() const = 0;
  virtual bool LookupChannel(int uid, Channel& out) const = 0;
  virtual bool Signal(int channelUid, SignalInfo& out) = 0;

  virtual std::vector<GuideEvent> GetEvents(int channelUid, time_t start, time_t end) = 0;
  virtual bool FindEvent(int channelUid, unsigned int eventUid, GuideEvent& out) = 0;

  virtual std::vector<Timer> GetTimers() = 0;
  virtual unsigned int AddTimer(Timer timer, std::string& error) = 0;
  virtual bool UpdateTimer(const Timer& timer) = 0;
  virtual bool DeleteTimer(unsigned int id) = 0;

  virtual std::vector<RecordingInfo> GetRecordings() = 0;
  virtual bool DeleteRecording(const std::string& id, std::string& error) = 0;
  virtual int64_t RecordingSize(const std::string& id) = 0;
  virtual bool DriveSpace(uint64_t& total, uint64_t& used) = 0;
  virtual std::unique_ptr<MediaReader> OpenRecording(const std::string& id) = 0;

  // Live TV goes to sink until the handle is destroyed. The sink's OnEnd() marks the end.
  virtual std::unique_ptr<LiveHandle> OpenLive(int channelUid, std::shared_ptr<IStreamSink> sink,
                                               std::string& error) = 0;
};

struct BackendSettings
{
  std::string manualHost;
  std::string guideFile;
  std::string recordingsFolder;
  bool hideDrm = true;
};

// How the backend reaches the outside world. The add-on goes through Kodi; the service has its own.
struct BackendIo
{
  // Https, and for the add-on any path Kodi can open.
  UrlFetcher fetch;
  // Modification time of the guide file, 0 when missing.
  std::function<time_t(const std::string& path)> modified;
};

// The tuner, guide, timers and recordings, all in this process.
class Backend : public IBackend
{
public:
  Backend(IBackendHost& host, BackendSettings settings, BackendIo io, std::string stateFile);
  ~Backend() override;

  void Start() override;
  void Stop() override;
  void Refresh() override;

  bool Connected() const override { return m_connected; }
  std::string Name() const override { return "Direct Tuner PVR"; }
  std::string Version() const override;
  std::string Hostname() const override;
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

  // For the recorder service.
  const BackendSettings& Settings() const { return m_settings; }
  DeviceInfo Device() const;
  int ActiveRecordings() const;
  int ActiveTuners() const { return m_pool.ActiveCount(); }
  // The start of the next recording window, 0 when nothing is scheduled.
  time_t NextRecordingStart() const;
  bool IsRecording(const std::string& recordingId) const;

private:
  // The scheduler's view of this backend.
  class SchedulerHost : public ISchedulerHost
  {
  public:
    explicit SchedulerHost(Backend& owner) : m_owner(owner) {}
    void NotifyUser(bool isError, const std::string& text) override;
    void TimersChanged() override;
    void RecordingsChanged() override;
    bool LookupChannel(int uid, Channel& out) override;

  private:
    Backend& m_owner;
  };

  bool ConnectDevice();
  void Run();
  void RefreshDeviceGuide();
  bool RefreshGuideFile(bool force);

  IBackendHost& m_host;
  BackendSettings m_settings;
  BackendIo m_io;
  std::string m_stateFile;

  DeviceInfo m_device;
  std::atomic<bool> m_connected{false};
  mutable std::mutex m_deviceMutex;
  std::vector<Channel> m_channels;
  std::map<int, std::string> m_icons;

  TunerPool m_pool;
  GuideStore m_guide;
  RecordingStore m_store;
  SchedulerHost m_schedulerHost{*this};
  std::unique_ptr<Scheduler> m_scheduler;

  std::thread m_thread;
  std::mutex m_runMutex;
  std::condition_variable m_runWake;
  bool m_stop = false;
  bool m_refreshNow = false;
  time_t m_guideFileTime = 0;
};

} // namespace dt
