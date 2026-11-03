/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Recorder.h"

#include "../core/Common.h"
#include "../core/Net.h"
#include "../core/Wire.h"
#include "Fetch.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dt
{
namespace
{

constexpr size_t kMaxEvents = 500;
constexpr int kMaxEventWaitSeconds = 60;
// A viewer that falls this far behind is cut off rather than holding memory without limit.
constexpr size_t kMaxRelayBytes = 64 * 1024 * 1024;

std::string ComputerName()
{
  char name[256] = {};
  if (gethostname(name, sizeof(name)) != 0)
    return "this computer";
  return name;
}

bool IsPrivateAddress(const std::string& ip)
{
  in_addr address{};
  if (inet_pton(AF_INET, ip.c_str(), &address) != 1)
    return false;
  const uint32_t a = ntohl(address.s_addr);
  return (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) || (a >> 16) == (192u << 8 | 168) ||
         (a >> 16) == (169u << 8 | 254) || (a >> 22) == (100u << 2 | 1) || (a >> 24) == 127;
}

// Parses "bytes=N-" from a Range header. -1 when there's none.
int64_t RangeStart(const std::string& header)
{
  if (header.compare(0, 6, "bytes=") != 0)
    return -1;
  return std::atoll(header.c_str() + 6);
}

} // namespace

// Live TV for one viewer. The tuner's delivery thread drops data in here and never waits on the
// network; the viewer's connection thread sends it.
class LiveRelay : public IStreamSink
{
public:
  explicit LiveRelay(int channelUid) : m_channelUid(channelUid) {}

  bool OnData(const uint8_t* data, size_t size) override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ended)
      return false;
    if (m_bytes + size > kMaxRelayBytes)
    {
      Log(LogLevel::Warning, "A viewer fell too far behind on channel %d; ending its stream", m_channelUid);
      m_ended = true;
      m_ready.notify_all();
      return false;
    }
    m_queue.emplace_back(data, data + size);
    m_bytes += size;
    m_ready.notify_all();
    return true;
  }

  void OnEnd() override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ended = true;
    m_ready.notify_all();
  }

  // Waits for data. False once the stream has ended and everything has been taken.
  bool Take(std::vector<uint8_t>& out)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_ready.wait_for(lock, std::chrono::seconds(1), [this] { return !m_queue.empty() || m_ended; });
    if (m_queue.empty())
      return !m_ended;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    m_bytes -= out.size();
    return true;
  }

  int ChannelUid() const { return m_channelUid; }

private:
  int m_channelUid;
  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::deque<std::vector<uint8_t>> m_queue;
  size_t m_bytes = 0;
  bool m_ended = false;
};

RecorderConfig RecorderConfig::Load(const std::string& path, const std::string& defaultRecordings)
{
  RecorderConfig config;
  std::string text;
  Json json;
  if (ReadWholeFile(path, text))
    Json::Parse(text, json);

  config.backend.recordingsFolder = Trim(json["recordingsFolder"].AsString());
  if (config.backend.recordingsFolder.empty())
    config.backend.recordingsFolder = defaultRecordings;
  config.backend.manualHost = Trim(json["tunerAddress"].AsString());
  config.backend.guideFile = Trim(json["guideFile"].AsString());
  config.backend.hideDrm = json["hideCopyProtected"].AsBool(true);
  config.port = json["port"].AsInt(8866);
  config.allowNetwork = json["allowNetwork"].AsBool(true);

  Json out = Json::Object();
  out.Set("recordingsFolder", config.backend.recordingsFolder);
  out.Set("tunerAddress", config.backend.manualHost);
  out.Set("guideFile", config.backend.guideFile);
  out.Set("hideCopyProtected", config.backend.hideDrm);
  out.Set("port", config.port);
  out.Set("allowNetwork", config.allowNetwork);
  if (out.Dump() != json.Dump())
    WriteFileAtomic(path, out.Dump());
  return config;
}

Recorder::Recorder(RecorderConfig config, std::string dataFolder)
  : m_config(std::move(config)), m_dataFolder(std::move(dataFolder))
{
}

Recorder::~Recorder()
{
  Stop();
}

bool Recorder::Start(std::string& error)
{
  NetInit();
  BackendIo io;
  io.fetch = FetchUrl;
  m_backend = std::make_unique<Backend>(*this, m_config.backend, io, JoinPath(m_dataFolder, "timers.json"));
  m_backend->Start();

  if (!m_server.Start(
          m_config.port, [this](const HttpServerRequest& request, HttpServerResponse& response) {
            Handle(request, response);
          },
          [this](const std::string& ip) { return Allowed(ip); }))
  {
    error = Format("Can't listen on port %d. Is another copy of the recorder running?", m_config.port);
    m_backend->Stop();
    return false;
  }

  if (m_config.allowNetwork)
  {
    m_stopDiscovery = false;
    m_discovery = std::thread([this] { Discovery(); });
  }
  Log(LogLevel::Info, "Direct Tuner Recorder %s listening on port %d, recording to %s", kRecorderVersion,
      m_config.port, m_config.backend.recordingsFolder.c_str());
  return true;
}

void Recorder::Stop()
{
  if (!m_backend)
    return;
  {
    std::lock_guard<std::mutex> lock(m_eventMutex);
    m_stopping = true;
    m_eventReady.notify_all();
  }
  m_stopDiscovery = true;
  if (m_discoverySocket != -1)
  {
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(m_discoverySocket));
#else
    close(static_cast<int>(m_discoverySocket));
#endif
    m_discoverySocket = -1;
  }
  if (m_discovery.joinable())
    m_discovery.join();
  {
    std::lock_guard<std::mutex> lock(m_liveMutex);
    for (const auto& weak : m_relays)
      if (auto relay = weak.lock())
        relay->OnEnd();
  }
  m_server.Stop();
  m_backend->Stop();
  m_backend.reset();
}

void Recorder::Resume()
{
  if (m_backend)
    m_backend->Refresh();
}

bool Recorder::Busy() const
{
  return m_backend && (m_backend->ActiveRecordings() > 0 || m_backend->ActiveTuners() > 0);
}

time_t Recorder::NextRecordingStart() const
{
  return m_backend ? m_backend->NextRecordingStart() : 0;
}

bool Recorder::Allowed(const std::string& ip) const
{
  if (ip.rfind("127.", 0) == 0)
    return true;
  return m_config.allowNetwork && IsPrivateAddress(ip);
}

void Recorder::Push(const std::string& type, Json extra)
{
  extra.Set("type", type);
  std::lock_guard<std::mutex> lock(m_eventMutex);
  m_events.emplace_back(++m_seq, std::move(extra));
  while (m_events.size() > kMaxEvents)
    m_events.pop_front();
  m_eventReady.notify_all();
}

void Recorder::Notify(bool isError, const std::string& text)
{
  Log(isError ? LogLevel::Error : LogLevel::Info, "%s", text.c_str());
  Json extra = Json::Object();
  extra.Set("isError", isError);
  extra.Set("text", text);
  Push("notify", std::move(extra));
}

void Recorder::ConnectionChanged(bool connected, const std::string& text)
{
  Log(connected ? LogLevel::Info : LogLevel::Warning, "Tuner %s: %s", connected ? "connected" : "not connected",
      text.c_str());
  Push("channels");
}

void Recorder::LiveStopped(int channelUid, const std::string& reason)
{
  {
    std::lock_guard<std::mutex> lock(m_liveMutex);
    for (const auto& weak : m_relays)
      if (auto relay = weak.lock())
        if (relay->ChannelUid() == channelUid)
          relay->OnEnd();
  }
  Json extra = Json::Object();
  extra.Set("channelUid", channelUid);
  extra.Set("text", reason);
  Push("liveStopped", std::move(extra));
}

void Recorder::Handle(const HttpServerRequest& request, HttpServerResponse& response)
{
  const std::string& path = request.path;
  const std::string& method = request.method;
  Backend& backend = *m_backend;

  // Splits "/api/a/b/c" after the prefix into its parts.
  const auto parts = [&](const std::string& prefix) {
    std::vector<std::string> out;
    size_t pos = prefix.size();
    while (pos < path.size())
    {
      size_t end = path.find('/', pos);
      if (end == std::string::npos)
        end = path.size();
      out.push_back(path.substr(pos, end - pos));
      pos = end + 1;
    }
    return out;
  };

  if (path == "/api/info")
  {
    Json json = Json::Object();
    json.Set("service", "directtuner-recorder");
    json.Set("version", kRecorderVersion);
    json.Set("computer", ComputerName());
    json.Set("connected", backend.Connected());
    json.Set("tuner", backend.ConnectionText());
    json.Set("recordingsFolder", m_config.backend.recordingsFolder);
    json.Set("activeRecordings", backend.ActiveRecordings());
    json.Set("activeTuners", backend.ActiveTuners());
    response.SendJson(200, json.Dump());
    return;
  }

  if (path == "/api/events")
  {
    ServeEvents(request, response);
    return;
  }

  if (path == "/api/channels")
  {
    const std::map<int, std::string> icons = backend.ChannelIcons();
    Json list = Json::Array();
    for (const Channel& channel : backend.Channels())
    {
      Json item = ChannelToJson(channel);
      const auto icon = icons.find(channel.uid);
      item.Set("logo", icon == icons.end() ? std::string() : icon->second);
      list.Push(std::move(item));
    }
    Json json = Json::Object();
    json.Set("channels", std::move(list));
    response.SendJson(200, json.Dump());
    return;
  }

  if (path.rfind("/api/signal/", 0) == 0)
  {
    SignalInfo signal;
    if (!backend.Signal(std::atoi(path.c_str() + 12), signal))
    {
      response.SendError(404, "Unknown channel");
      return;
    }
    Json json = Json::Object();
    json.Set("adapter", signal.adapter);
    json.Set("status", signal.status);
    json.Set("strength", signal.strength);
    json.Set("quality", signal.quality);
    response.SendJson(200, json.Dump());
    return;
  }

  if (path.rfind("/api/guide/", 0) == 0)
  {
    const std::vector<std::string> p = parts("/api/guide/");
    const int uid = p.empty() ? 0 : std::atoi(p[0].c_str());
    if (p.size() >= 2)
    {
      GuideEvent event;
      if (!backend.FindEvent(uid, static_cast<unsigned int>(std::strtoul(p[1].c_str(), nullptr, 10)), event))
        response.SendError(404, "No such program");
      else
        response.SendJson(200, EventToJson(event).Dump());
      return;
    }
    Json list = Json::Array();
    for (const GuideEvent& event : backend.GetEvents(uid, std::atoll(request.Param("start").c_str()),
                                                     std::atoll(request.Param("end").c_str())))
      list.Push(EventToJson(event));
    Json json = Json::Object();
    json.Set("events", std::move(list));
    response.SendJson(200, json.Dump());
    return;
  }

  if (path == "/api/timers" && method == "GET")
  {
    Json list = Json::Array();
    for (const Timer& timer : backend.GetTimers())
      list.Push(TimerToJson(timer, true));
    Json json = Json::Object();
    json.Set("timers", std::move(list));
    response.SendJson(200, json.Dump());
    return;
  }

  if (path == "/api/timers" && method == "POST")
  {
    Json body;
    if (!Json::Parse(request.body, body))
    {
      response.SendError(400, "The timer isn't valid JSON");
      return;
    }
    Timer timer = TimerFromJson(body);
    timer.id = 0;
    std::string error;
    const unsigned int id = backend.AddTimer(std::move(timer), error);
    if (id == 0)
    {
      response.SendError(400, error);
      return;
    }
    Log(LogLevel::Info, "Timer %u added from %s", id, request.remoteIp.c_str());
    Json json = Json::Object();
    json.Set("id", id);
    response.SendJson(200, json.Dump());
    return;
  }

  if (path.rfind("/api/timers/", 0) == 0)
  {
    const auto id = static_cast<unsigned int>(std::strtoul(path.c_str() + 12, nullptr, 10));
    bool ok = false;
    if (method == "PUT")
    {
      Json body;
      if (Json::Parse(request.body, body))
      {
        Timer timer = TimerFromJson(body);
        timer.id = id;
        ok = backend.UpdateTimer(timer);
      }
    }
    else if (method == "DELETE")
    {
      ok = backend.DeleteTimer(id);
    }
    if (ok)
      response.SendJson(200, "{}");
    else
      response.SendError(404, "No such timer");
    return;
  }

  if (path == "/api/recordings")
  {
    Json list = Json::Array();
    for (const RecordingInfo& info : backend.GetRecordings())
      list.Push(RecordingToJson(info, true));
    Json json = Json::Object();
    json.Set("recordings", std::move(list));
    response.SendJson(200, json.Dump());
    return;
  }

  if (path == "/api/recording/size")
  {
    Json json = Json::Object();
    json.Set("size", static_cast<long long>(backend.RecordingSize(request.Param("id"))));
    response.SendJson(200, json.Dump());
    return;
  }

  if (path == "/api/recording" && method == "DELETE")
  {
    std::string error;
    if (backend.DeleteRecording(request.Param("id"), error))
      response.SendJson(200, "{}");
    else
      response.SendError(409, error);
    return;
  }

  if (path == "/api/recording" && method == "GET")
  {
    ServeRecording(request, response);
    return;
  }

  if (path == "/api/drive")
  {
    uint64_t total = 0;
    uint64_t used = 0;
    if (!backend.DriveSpace(total, used))
    {
      response.SendError(503, "Can't read the recordings drive");
      return;
    }
    Json json = Json::Object();
    json.Set("total", static_cast<double>(total));
    json.Set("used", static_cast<double>(used));
    response.SendJson(200, json.Dump());
    return;
  }

  if (path.rfind("/api/live/", 0) == 0)
  {
    ServeLive(std::atoi(path.c_str() + 10), response);
    return;
  }

  response.SendError(404, "Not found");
}

void Recorder::ServeEvents(const HttpServerRequest& request, HttpServerResponse& response)
{
  const long long since = std::atoll(request.Param("since").c_str());
  const int wait = std::min(std::max(std::atoi(request.Param("wait").c_str()), 0), kMaxEventWaitSeconds);

  Json list = Json::Array();
  long long seq;
  {
    std::unique_lock<std::mutex> lock(m_eventMutex);
    // A client that's new, or so far behind that events were dropped, starts from now.
    if (since >= 0 && since <= m_seq)
      m_eventReady.wait_for(lock, std::chrono::seconds(wait), [&] { return m_seq > since || m_stopping; });
    seq = m_seq;
    if (since >= 0)
      for (const auto& event : m_events)
        if (event.first > since)
          list.Push(event.second);
  }
  Json json = Json::Object();
  json.Set("seq", seq);
  json.Set("events", std::move(list));
  response.SendJson(200, json.Dump());
}

void Recorder::ServeLive(int channelUid, HttpServerResponse& response)
{
  auto relay = std::make_shared<LiveRelay>(channelUid);
  std::string error;
  auto handle = m_backend->OpenLive(channelUid, relay, error);
  if (!handle)
  {
    response.SendError(503, error);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(m_liveMutex);
    m_relays.erase(std::remove_if(m_relays.begin(), m_relays.end(),
                                  [](const std::weak_ptr<LiveRelay>& weak) { return weak.expired(); }),
                   m_relays.end());
    m_relays.push_back(relay);
  }

  Channel channel;
  m_backend->LookupChannel(channelUid, channel);
  Log(LogLevel::Info, "Live TV on %s %s started", channel.guideNumber.c_str(), channel.name.c_str());
  if (response.Begin(200, "video/mp2t"))
  {
    std::vector<uint8_t> block;
    while (relay->Take(block))
    {
      if (!block.empty() && !response.Write(block.data(), block.size()))
        break;
      block.clear();
    }
  }
  relay->OnEnd();
  handle.reset();
  Log(LogLevel::Info, "Live TV on %s %s ended", channel.guideNumber.c_str(), channel.name.c_str());
}

void Recorder::ServeRecording(const HttpServerRequest& request, HttpServerResponse& response)
{
  const std::string id = request.Param("id");
  auto reader = m_backend->OpenRecording(id);
  if (!reader)
  {
    response.SendError(404, "No such recording");
    return;
  }

  const int64_t start = RangeStart(request.Header("range"));
  if (start > 0)
  {
    if (start >= reader->Length() && !m_backend->IsRecording(id))
    {
      response.SendError(416, "Past the end of the recording");
      return;
    }
    reader->Seek(start, SEEK_SET);
  }
  // The length isn't sent: a recording in progress is still growing.
  if (!response.Begin(start >= 0 ? 206 : 200, "video/mp2t"))
    return;

  std::vector<uint8_t> buffer(188 * 1024);
  while (true)
  {
    const int count = reader->Read(buffer.data(), buffer.size());
    if (count > 0)
    {
      if (!response.Write(buffer.data(), static_cast<size_t>(count)))
        return;
      continue;
    }
    // The reader gives up after a short wait; carry on as long as the recording does.
    if (count < 0 || !m_backend->IsRecording(id))
      return;
  }
}

void Recorder::Discovery()
{
  const auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(static_cast<uint16_t>(kRecorderDiscoveryPort));
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
  {
    Log(LogLevel::Warning, "Can't answer discovery on UDP port %d; Kodi needs the recorder's address",
        kRecorderDiscoveryPort);
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return;
  }
  m_discoverySocket = static_cast<intptr_t>(s);

  const std::string request = kRecorderDiscoverRequest;
  const std::string reply = std::string(kRecorderDiscoverReply) + std::to_string(m_config.port);
  while (!m_stopDiscovery)
  {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval timeout{0, 500 * 1000};
    if (select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &timeout) <= 0)
      continue;
    char buffer[256];
    sockaddr_in from{};
    socklen_t size = sizeof(from);
    const int count = recvfrom(s, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &size);
    if (count <= 0 || std::string(buffer, static_cast<size_t>(count)) != request)
      continue;
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
    if (!Allowed(text))
      continue;
    sendto(s, reply.data(), static_cast<int>(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), size);
  }
}

} // namespace dt
