/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "RemoteBackend.h"

#include "Common.h"
#include "Net.h"
#include "Wire.h"

#include <chrono>
#include <cstring>

namespace dt
{
namespace
{

constexpr int kEventWaitSeconds = 20;
constexpr int kRetrySeconds = 5;
// Tuning can take 10 seconds, and AC-4 channels longer before data flows.
constexpr int kLiveOpenTimeoutMs = 25000;
constexpr int kPacket = 188;

// Live TV from the service, delivered to the sink in whole transport packets.
class RemoteLive : public LiveHandle
{
public:
  RemoteLive(std::unique_ptr<HttpStream> stream, std::shared_ptr<IStreamSink> sink)
    : m_stream(std::move(stream)), m_sink(std::move(sink))
  {
    m_thread = std::thread([this] { Run(); });
  }

  ~RemoteLive() override
  {
    m_stop = true;
    m_stream->Close();
    if (m_thread.joinable())
      m_thread.join();
  }

private:
  void Run()
  {
    std::vector<uint8_t> buffer(kPacket * 1024);
    size_t held = 0;
    while (!m_stop)
    {
      const int count = m_stream->Read(buffer.data() + held, buffer.size() - held, 1000);
      if (count < 0)
        break;
      held += static_cast<size_t>(count);
      const size_t whole = held / kPacket * kPacket;
      if (whole > 0)
      {
        if (!m_sink->OnData(buffer.data(), whole))
          break;
        std::memmove(buffer.data(), buffer.data() + whole, held - whole);
        held -= whole;
      }
    }
    if (!m_stop)
      m_sink->OnEnd();
  }

  std::unique_ptr<HttpStream> m_stream;
  std::shared_ptr<IStreamSink> m_sink;
  std::thread m_thread;
  std::atomic<bool> m_stop{false};
};

// A recording from the service. Each seek starts a new request at that byte; the service keeps
// the reply open while the recording is still growing.
class RemoteReader : public MediaReader
{
public:
  RemoteReader(RemoteBackend& backend, std::string url, std::string id)
    : m_backend(backend), m_url(std::move(url)), m_id(std::move(id))
  {
  }

  int Read(uint8_t* buffer, size_t size) override
  {
    if (!m_open)
    {
      const std::string range = Format("Range: bytes=%lld-\r\n", static_cast<long long>(m_position));
      if (!m_stream.Open(m_url, 15000, "GET", range))
        return -1;
      if (m_stream.Status() == 416)
        return 0; // at the end already
      if (m_stream.Status() != 200 && m_stream.Status() != 206)
        return -1;
      m_open = true;
    }
    for (int attempt = 0; attempt < 30; ++attempt)
    {
      const int count = m_stream.Read(buffer, size, 1000);
      if (count > 0)
      {
        m_position += count;
        return count;
      }
      if (count < 0)
        return 0;
    }
    return 0;
  }

  int64_t Seek(int64_t position, int whence) override
  {
    int64_t target = position;
    if (whence == SEEK_CUR)
      target = m_position + position;
    else if (whence == SEEK_END)
      target = Length() + position;
    if (target < 0)
      return -1;
    if (target != m_position)
    {
      m_stream.Close();
      m_open = false;
      m_position = target;
    }
    return m_position;
  }

  int64_t Length() override { return m_backend.RecordingSize(m_id); }

private:
  RemoteBackend& m_backend;
  std::string m_url;
  std::string m_id;
  HttpStream m_stream;
  bool m_open = false;
  int64_t m_position = 0;
};

} // namespace

RemoteBackend::RemoteBackend(IBackendHost& host, std::string baseUrl)
  : m_host(host), m_baseUrl(std::move(baseUrl))
{
  while (!m_baseUrl.empty() && m_baseUrl.back() == '/')
    m_baseUrl.pop_back();
}

RemoteBackend::~RemoteBackend()
{
  Stop();
}

bool RemoteBackend::Probe(const std::string& baseUrl, std::string* name)
{
  int status = 0;
  std::string reply;
  Json json;
  if (!HttpRequest("GET", baseUrl + "/api/info", {}, 3000, status, reply) || status != 200 ||
      !Json::Parse(reply, json) || json["service"].AsString() != "directtuner-recorder")
    return false;
  if (name)
    *name = json["computer"].AsString();
  return true;
}

bool RemoteBackend::Call(const std::string& method, const std::string& path, const std::string& body,
                         Json& reply, std::string* error, int timeoutMs)
{
  int status = 0;
  std::string text;
  if (!HttpRequest(method, m_baseUrl + path, body, timeoutMs, status, text))
  {
    if (error)
      *error = "Can't reach the recorder at " + m_baseUrl;
    return false;
  }
  Json::Parse(text, reply);
  if (status < 200 || status >= 300)
  {
    if (error)
      *error = reply["error"].AsString(Format("The recorder answered %d", status));
    return false;
  }
  return true;
}

void RemoteBackend::Start()
{
  m_stop = false;
  if (LoadInfo() && LoadChannels())
    SetConnected(true, m_baseUrl);
  else
    SetConnected(false, "Can't reach the recorder at " + m_baseUrl);
  m_thread = std::thread([this] { Run(); });
}

void RemoteBackend::Stop()
{
  m_stop = true;
  if (m_thread.joinable())
    m_thread.join();
}

void RemoteBackend::Refresh()
{
  m_refresh = true;
}

bool RemoteBackend::LoadInfo()
{
  Json json;
  if (!Call("GET", "/api/info", {}, json))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  m_version = json["version"].AsString();
  m_tunerText = json["tuner"].AsString();
  return true;
}

bool RemoteBackend::LoadChannels()
{
  Json json;
  if (!Call("GET", "/api/channels", {}, json))
    return false;
  std::vector<Channel> channels;
  std::map<int, std::string> icons;
  for (const Json& item : json["channels"].Items())
  {
    Channel channel = ChannelFromJson(item);
    if (!item["logo"].AsString().empty())
      icons[channel.uid] = item["logo"].AsString();
    channels.push_back(std::move(channel));
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_channels = std::move(channels);
  m_icons = std::move(icons);
  return true;
}

void RemoteBackend::SetConnected(bool connected, const std::string& text)
{
  if (m_connected.exchange(connected) != connected || !connected)
    m_host.ConnectionChanged(connected, text);
}

void RemoteBackend::Run()
{
  long long seq = -1;
  while (!m_stop)
  {
    if (!m_connected || m_refresh.exchange(false))
    {
      if (!LoadInfo() || !LoadChannels())
      {
        SetConnected(false, "Can't reach the recorder at " + m_baseUrl);
        for (int i = 0; i < kRetrySeconds * 10 && !m_stop; ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      SetConnected(true, m_baseUrl);
      m_host.ChannelsChanged();
      m_host.GuideChanged();
      m_host.TimersChanged();
      m_host.RecordingsChanged();
    }

    // Waits on the service until something changes, or a timeout says nothing did.
    Json json;
    const std::string path = Format("/api/events?since=%lld&wait=%d", seq, kEventWaitSeconds);
    if (!Call("GET", path, {}, json, nullptr, (kEventWaitSeconds + 10) * 1000))
    {
      SetConnected(false, "Lost contact with the recorder at " + m_baseUrl);
      continue;
    }
    const long long latest = json["seq"].AsInt64();
    if (seq < 0)
    {
      seq = latest; // the first reply only says where we are
      continue;
    }
    seq = latest;
    for (const Json& event : json["events"].Items())
    {
      const std::string type = event["type"].AsString();
      if (type == "channels")
      {
        LoadChannels();
        m_host.ChannelsChanged();
      }
      else if (type == "guide")
        m_host.GuideChanged();
      else if (type == "timers")
        m_host.TimersChanged();
      else if (type == "recordings")
        m_host.RecordingsChanged();
      else if (type == "liveStopped")
        m_host.LiveStopped(event["channelUid"].AsInt(), event["text"].AsString());
      else if (type == "notify")
        m_host.Notify(event["isError"].AsBool(), event["text"].AsString());
    }
  }
}

std::string RemoteBackend::Version() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_version;
}

std::string RemoteBackend::ConnectionText() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_connected)
    return "Recorder not connected";
  return "Recorder " + m_baseUrl + (m_tunerText.empty() ? "" : ", " + m_tunerText);
}

std::vector<Channel> RemoteBackend::Channels() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_channels;
}

std::map<int, std::string> RemoteBackend::ChannelIcons() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_icons;
}

bool RemoteBackend::LookupChannel(int uid, Channel& out) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
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

bool RemoteBackend::Signal(int channelUid, SignalInfo& out)
{
  Json json;
  if (!Call("GET", Format("/api/signal/%d", channelUid), {}, json))
    return false;
  out.adapter = json["adapter"].AsString();
  out.status = json["status"].AsString();
  out.strength = json["strength"].AsInt();
  out.quality = json["quality"].AsInt();
  return true;
}

std::vector<GuideEvent> RemoteBackend::GetEvents(int channelUid, time_t start, time_t end)
{
  std::vector<GuideEvent> events;
  Json json;
  if (Call("GET",
           Format("/api/guide/%d?start=%lld&end=%lld", channelUid, static_cast<long long>(start),
                  static_cast<long long>(end)),
           {}, json, nullptr, 30000))
    for (const Json& item : json["events"].Items())
      events.push_back(EventFromJson(item));
  return events;
}

bool RemoteBackend::FindEvent(int channelUid, unsigned int eventUid, GuideEvent& out)
{
  Json json;
  if (!Call("GET", Format("/api/guide/%d/%u", channelUid, eventUid), {}, json))
    return false;
  out = EventFromJson(json);
  return true;
}

std::vector<Timer> RemoteBackend::GetTimers()
{
  std::vector<Timer> timers;
  Json json;
  if (Call("GET", "/api/timers", {}, json))
    for (const Json& item : json["timers"].Items())
      timers.push_back(TimerFromJson(item));
  return timers;
}

unsigned int RemoteBackend::AddTimer(Timer timer, std::string& error)
{
  Json json;
  if (!Call("POST", "/api/timers", TimerToJson(timer).Dump(), json, &error))
    return 0;
  return static_cast<unsigned int>(json["id"].AsNumber());
}

bool RemoteBackend::UpdateTimer(const Timer& timer)
{
  Json json;
  return Call("PUT", Format("/api/timers/%u", timer.id), TimerToJson(timer).Dump(), json);
}

bool RemoteBackend::DeleteTimer(unsigned int id)
{
  Json json;
  return Call("DELETE", Format("/api/timers/%u", id), {}, json);
}

std::vector<RecordingInfo> RemoteBackend::GetRecordings()
{
  std::vector<RecordingInfo> recordings;
  Json json;
  if (Call("GET", "/api/recordings", {}, json, nullptr, 30000))
    for (const Json& item : json["recordings"].Items())
      recordings.push_back(RecordingFromJson(item));
  return recordings;
}

bool RemoteBackend::DeleteRecording(const std::string& id, std::string& error)
{
  Json json;
  return Call("DELETE", "/api/recording?id=" + UrlEncode(id), {}, json, &error);
}

int64_t RemoteBackend::RecordingSize(const std::string& id)
{
  Json json;
  if (!Call("GET", "/api/recording/size?id=" + UrlEncode(id), {}, json))
    return 0;
  return json["size"].AsInt64();
}

bool RemoteBackend::DriveSpace(uint64_t& total, uint64_t& used)
{
  Json json;
  if (!Call("GET", "/api/drive", {}, json))
    return false;
  total = static_cast<uint64_t>(json["total"].AsNumber());
  used = static_cast<uint64_t>(json["used"].AsNumber());
  return true;
}

std::unique_ptr<MediaReader> RemoteBackend::OpenRecording(const std::string& id)
{
  return std::make_unique<RemoteReader>(*this, m_baseUrl + "/api/recording?id=" + UrlEncode(id), id);
}

std::unique_ptr<LiveHandle> RemoteBackend::OpenLive(int channelUid, std::shared_ptr<IStreamSink> sink,
                                                    std::string& error)
{
  auto stream = std::make_unique<HttpStream>();
  if (!stream->Open(m_baseUrl + Format("/api/live/%d", channelUid), kLiveOpenTimeoutMs))
  {
    error = "Can't reach the recorder at " + m_baseUrl;
    return nullptr;
  }
  if (stream->Status() != 200)
  {
    // The reply is a short JSON error.
    std::string text;
    uint8_t chunk[2048];
    int count;
    while ((count = stream->Read(chunk, sizeof(chunk), 2000)) > 0)
      text.append(reinterpret_cast<char*>(chunk), static_cast<size_t>(count));
    Json json;
    Json::Parse(text, json);
    error = json["error"].AsString(Format("The recorder answered %d", stream->Status()));
    return nullptr;
  }
  return std::make_unique<RemoteLive>(std::move(stream), std::move(sink));
}

} // namespace dt
