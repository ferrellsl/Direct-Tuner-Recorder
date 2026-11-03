/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Backend.h"

#include "Common.h"

#include <algorithm>
#include <chrono>

namespace dt
{
namespace
{

constexpr int kGuideRefreshSeconds = 4 * 60 * 60;
constexpr int kLineupRefreshSeconds = 60 * 60;
constexpr int kFileCheckSeconds = 5 * 60;
constexpr int kReconnectSeconds = 30;

class LocalLive : public LiveHandle
{
public:
  LocalLive(std::unique_ptr<TunerLease> lease, std::shared_ptr<IStreamSink> sink)
    : m_lease(std::move(lease)), m_sink(std::move(sink))
  {
    m_lease->Pump().AddSink(m_sink);
  }
  ~LocalLive() override { m_lease->Pump().RemoveSink(m_sink); }

private:
  std::unique_ptr<TunerLease> m_lease;
  std::shared_ptr<IStreamSink> m_sink;
};

class LocalReader : public MediaReader
{
public:
  LocalReader(std::FILE* file, std::string path, std::function<bool()> recording)
    : m_file(file), m_path(std::move(path)), m_recording(std::move(recording))
  {
  }
  ~LocalReader() override { std::fclose(m_file); }

  int Read(uint8_t* buffer, size_t size) override
  {
    // A recording in progress keeps growing, so reaching its end means waiting.
    for (int attempt = 0; attempt < 100; ++attempt)
    {
      const size_t count = std::fread(buffer, 1, size, m_file);
      if (count > 0)
        return static_cast<int>(count);
      std::clearerr(m_file);
      if (!m_recording())
        return 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
  }

  int64_t Seek(int64_t position, int whence) override
  {
    if (SeekFile(m_file, position, whence) != 0)
      return -1;
    return TellFile(m_file);
  }

  int64_t Length() override { return FileSize(m_path); }

private:
  std::FILE* m_file;
  std::string m_path;
  std::function<bool()> m_recording;
};

} // namespace

Backend::Backend(IBackendHost& host, BackendSettings settings, BackendIo io, std::string stateFile)
  : m_host(host), m_settings(std::move(settings)), m_io(std::move(io)), m_stateFile(std::move(stateFile))
{
}

Backend::~Backend()
{
  Stop();
}

void Backend::Start()
{
  m_store.SetFolder(m_settings.recordingsFolder);
  if (!CreateDirectories(m_settings.recordingsFolder))
    m_host.Notify(true, "Can't create the recordings folder " + m_settings.recordingsFolder);

  m_pool.SetPreemptHandler([this](const Channel& channel) {
    m_host.LiveStopped(channel.uid, "Live TV stopped: a recording needed the tuner");
  });

  ConnectDevice();

  m_scheduler = std::make_unique<Scheduler>(m_pool, m_store, m_guide, m_schedulerHost);
  m_scheduler->Start(m_stateFile);

  {
    std::lock_guard<std::mutex> lock(m_runMutex);
    m_stop = false;
  }
  m_thread = std::thread([this] { Run(); });
}

void Backend::Stop()
{
  {
    std::lock_guard<std::mutex> lock(m_runMutex);
    m_stop = true;
    m_runWake.notify_all();
  }
  if (m_thread.joinable())
    m_thread.join();
  if (m_scheduler)
    m_scheduler->Stop();
}

void Backend::Refresh()
{
  std::lock_guard<std::mutex> lock(m_runMutex);
  m_refreshNow = true;
  m_runWake.notify_all();
}

std::string Backend::Version() const
{
  std::lock_guard<std::mutex> lock(m_deviceMutex);
  return m_device.firmware;
}

std::string Backend::Hostname() const
{
  std::lock_guard<std::mutex> lock(m_deviceMutex);
  return m_device.baseUrl;
}

std::string Backend::ConnectionText() const
{
  std::lock_guard<std::mutex> lock(m_deviceMutex);
  return m_connected ? m_device.friendlyName + " " + m_device.baseUrl : "Not connected";
}

DeviceInfo Backend::Device() const
{
  std::lock_guard<std::mutex> lock(m_deviceMutex);
  return m_device;
}

bool Backend::ConnectDevice()
{
  DeviceInfo device;
  std::string error;
  if (!FindDevice(m_settings.manualHost, device, error))
  {
    Log(LogLevel::Warning, "%s", error.c_str());
    m_connected = false;
    m_host.ConnectionChanged(false, error);
    return false;
  }

  std::vector<Channel> channels;
  if (!FetchLineup(device, m_settings.hideDrm, channels) || channels.empty())
  {
    error = "The tuner at " + device.baseUrl + " has no channels. Run a channel scan on the tuner.";
    Log(LogLevel::Warning, "%s", error.c_str());
    m_connected = false;
    m_host.ConnectionChanged(false, error);
    return false;
  }

  Log(LogLevel::Info, "Connected to %s %s at %s: %d tuners, %d channels", device.friendlyName.c_str(),
      device.id.c_str(), device.baseUrl.c_str(), device.tunerCount, static_cast<int>(channels.size()));

  {
    std::lock_guard<std::mutex> lock(m_deviceMutex);
    m_device = device;
    m_channels = std::move(channels);
  }
  m_pool.SetTunerCount(device.tunerCount);
  if (m_scheduler)
    m_scheduler->TunerCountChanged();
  m_connected = true;
  m_host.ConnectionChanged(true, device.baseUrl);
  return true;
}

void Backend::RefreshDeviceGuide()
{
  const DeviceInfo device = Device();
  ChannelEvents events;
  std::map<int, std::string> icons;
  if (!FetchDeviceGuide(device, m_io.fetch, events, icons))
  {
    Log(LogLevel::Warning, "Couldn't read the guide from SiliconDust");
    return;
  }

  size_t count = 0;
  for (const auto& item : events)
    count += item.second.size();
  Log(LogLevel::Info, "Device guide: %d channels, %d programs", static_cast<int>(events.size()),
      static_cast<int>(count));

  m_guide.SetDeviceGuide(std::move(events));
  bool newIcons;
  {
    std::lock_guard<std::mutex> lock(m_deviceMutex);
    newIcons = icons != m_icons;
    m_icons = std::move(icons);
  }
  if (newIcons)
    m_host.ChannelsChanged();
}

bool Backend::RefreshGuideFile(bool force)
{
  const std::string& path = m_settings.guideFile;
  if (path.empty())
    return false;

  const time_t modified = m_io.modified ? m_io.modified(path) : ModifiedTime(path);
  if (!force && modified == m_guideFileTime)
    return false;
  m_guideFileTime = modified;

  // Kodi's file layer can't open \\server\share paths, so plain paths are read directly.
  std::string text;
  const bool kodiPath = path.find("://") != std::string::npos;
  const bool opened = kodiPath && m_io.fetch ? m_io.fetch(path, text) : ReadWholeFile(path, text);
  if (!opened)
  {
    m_host.Notify(true, "Can't open the guide file " + path);
    return false;
  }

  const std::vector<Channel> channels = Channels();
  ChannelEvents events;
  XmltvStats stats;
  if (!ParseXmltv(text, channels, events, &stats))
  {
    m_host.Notify(true, "The guide file isn't valid XMLTV");
    return false;
  }

  Log(LogLevel::Info, "Guide file: %d of %d channels matched, %d programs", stats.channelsMatched,
      static_cast<int>(channels.size()), stats.programmes);
  m_guide.SetXmltv(std::move(events));
  return true;
}

void Backend::Run()
{
  time_t nextGuide = 0;
  time_t nextLineup = std::time(nullptr) + kLineupRefreshSeconds;
  time_t nextFile = 0;
  time_t nextConnect = 0;

  std::unique_lock<std::mutex> lock(m_runMutex);
  while (!m_stop)
  {
    const time_t now = std::time(nullptr);
    if (m_refreshNow)
    {
      m_refreshNow = false;
      nextGuide = nextFile = nextConnect = nextLineup = 0;
    }
    lock.unlock();

    bool changed = false;
    if (!m_connected && now >= nextConnect)
    {
      nextConnect = now + kReconnectSeconds;
      if (ConnectDevice())
      {
        m_host.ChannelsChanged();
        nextGuide = 0;
      }
    }

    if (m_connected)
    {
      if (now >= nextLineup)
      {
        nextLineup = now + kLineupRefreshSeconds;
        std::vector<Channel> channels;
        if (FetchLineup(Device(), m_settings.hideDrm, channels) && !channels.empty())
        {
          {
            std::lock_guard<std::mutex> deviceLock(m_deviceMutex);
            m_channels = std::move(channels);
          }
          m_host.ChannelsChanged();
        }
        else if (m_pool.ActiveCount() == 0)
        {
          m_connected = false;
          m_host.ConnectionChanged(false, "Lost contact with the tuner");
        }
      }
      if (now >= nextGuide)
      {
        nextGuide = now + kGuideRefreshSeconds;
        RefreshDeviceGuide();
        RefreshGuideFile(true);
        changed = true;
      }
      else if (now >= nextFile)
      {
        nextFile = now + kFileCheckSeconds;
        changed = RefreshGuideFile(false);
      }
    }

    if (changed)
    {
      m_host.GuideChanged();
      m_scheduler->GuideChanged();
    }

    lock.lock();
    m_runWake.wait_for(lock, std::chrono::seconds(5));
  }
}

std::vector<Channel> Backend::Channels() const
{
  std::lock_guard<std::mutex> lock(m_deviceMutex);
  return m_channels;
}

std::map<int, std::string> Backend::ChannelIcons() const
{
  std::lock_guard<std::mutex> lock(m_deviceMutex);
  return m_icons;
}

bool Backend::LookupChannel(int uid, Channel& out) const
{
  std::lock_guard<std::mutex> lock(m_deviceMutex);
  for (const Channel& channel : m_channels)
  {
    if (channel.uid == uid)
    {
      out = channel;
      return true;
    }
  }
  return false;
}

bool Backend::Signal(int channelUid, SignalInfo& out)
{
  const DeviceInfo device = Device();
  out.adapter = device.friendlyName;
  Channel channel;
  if (!LookupChannel(channelUid, channel))
    return false;

  std::vector<TunerUse> tuners;
  if (!FetchTunerStatus(device, tuners))
  {
    out.status = "Can't read tuner status";
    return true;
  }
  for (const TunerUse& tuner : tuners)
  {
    if (tuner.channel == channel.guideNumber)
    {
      out.status = tuner.resource;
      out.strength = tuner.signalStrength;
      out.quality = tuner.signalQuality;
      return true;
    }
  }
  return true;
}

std::vector<GuideEvent> Backend::GetEvents(int channelUid, time_t start, time_t end)
{
  return m_guide.GetEvents(channelUid, start, end);
}

bool Backend::FindEvent(int channelUid, unsigned int eventUid, GuideEvent& out)
{
  return m_guide.FindEvent(channelUid, eventUid, out);
}

std::vector<Timer> Backend::GetTimers()
{
  return m_scheduler->GetTimers();
}

unsigned int Backend::AddTimer(Timer timer, std::string& error)
{
  if (timer.epgUid != 0 && timer.channelUid != 0 && timer.event.title.empty())
    m_guide.FindEvent(timer.channelUid, timer.epgUid, timer.event);
  return m_scheduler->AddTimer(std::move(timer), &error);
}

bool Backend::UpdateTimer(const Timer& timer)
{
  Timer copy = timer;
  if (copy.epgUid != 0 && copy.channelUid != 0 && copy.event.title.empty())
    m_guide.FindEvent(copy.channelUid, copy.epgUid, copy.event);
  return m_scheduler->UpdateTimer(copy);
}

bool Backend::DeleteTimer(unsigned int id)
{
  return m_scheduler->DeleteTimer(id);
}

std::vector<RecordingInfo> Backend::GetRecordings()
{
  return m_store.List();
}

bool Backend::DeleteRecording(const std::string& id, std::string& error)
{
  if (m_scheduler->IsRecording(id))
  {
    error = "Stop the recording before deleting it";
    return false;
  }
  if (!m_store.Delete(id))
  {
    error = "Can't delete the recording";
    return false;
  }
  m_host.RecordingsChanged();
  return true;
}

int64_t Backend::RecordingSize(const std::string& id)
{
  return std::max<int64_t>(FileSize(m_store.MediaPath(id)), 0);
}

bool Backend::DriveSpace(uint64_t& total, uint64_t& used)
{
  return m_store.DriveSpace(total, used);
}

std::unique_ptr<MediaReader> Backend::OpenRecording(const std::string& id)
{
  const std::string path = m_store.MediaPath(id);
  std::FILE* file = OpenFileShared(path, "rb");
  if (!file)
    return nullptr;
  return std::make_unique<LocalReader>(file, path, [this, id] { return m_scheduler->IsRecording(id); });
}

std::unique_ptr<LiveHandle> Backend::OpenLive(int channelUid, std::shared_ptr<IStreamSink> sink,
                                              std::string& error)
{
  Channel channel;
  if (!LookupChannel(channelUid, channel))
  {
    error = "Unknown channel";
    return nullptr;
  }
  TuneResult result = TuneResult::Ok;
  auto lease = m_pool.Acquire(channel, TunerUseKind::Live, result);
  if (!lease)
  {
    error = channel.guideNumber + " " + channel.name + ": " + TuneResultText(result);
    return nullptr;
  }
  return std::make_unique<LocalLive>(std::move(lease), std::move(sink));
}

int Backend::ActiveRecordings() const
{
  return m_scheduler ? m_scheduler->ActiveRecordings() : 0;
}

bool Backend::IsRecording(const std::string& recordingId) const
{
  return m_scheduler && m_scheduler->IsRecording(recordingId);
}

time_t Backend::NextRecordingStart() const
{
  if (!m_scheduler)
    return 0;
  const time_t now = std::time(nullptr);
  time_t next = 0;
  for (const Timer& timer : m_scheduler->GetTimers())
  {
    if (timer.kind == TimerKind::Series || !timer.enabled || timer.state == TimerState::Recording)
      continue;
    const time_t start = timer.WindowStart();
    if (start > now && (next == 0 || start < next))
      next = start;
  }
  return next;
}

void Backend::SchedulerHost::NotifyUser(bool isError, const std::string& text)
{
  m_owner.m_host.Notify(isError, text);
}

void Backend::SchedulerHost::TimersChanged()
{
  m_owner.m_host.TimersChanged();
}

void Backend::SchedulerHost::RecordingsChanged()
{
  m_owner.m_host.RecordingsChanged();
}

bool Backend::SchedulerHost::LookupChannel(int uid, Channel& out)
{
  return m_owner.LookupChannel(uid, out);
}

} // namespace dt
