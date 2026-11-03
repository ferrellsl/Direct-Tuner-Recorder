/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Scheduler.h"

#include "Common.h"
#include "Json.h"
#include "Wire.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace dt
{
namespace
{

constexpr int kRetrySeconds = 5;
constexpr int kKeepFailedDays = 7;
constexpr time_t kRebuildSeconds = 600;
constexpr int kManualRepeatDays = 7;

std::tm LocalParts(time_t time)
{
  std::tm parts{};
#ifdef _WIN32
  localtime_s(&parts, &time);
#else
  localtime_r(&time, &parts);
#endif
  return parts;
}

// Monday is bit 0, as in Kodi's weekday mask.
unsigned int WeekdayBit(time_t time)
{
  const int weekday = LocalParts(time).tm_wday;
  return 1u << ((weekday + 6) % 7);
}

int MinuteOfDay(time_t time)
{
  const std::tm parts = LocalParts(time);
  return parts.tm_hour * 60 + parts.tm_min;
}

std::string AiringKey(int channelUid, time_t start)
{
  return Format("%d:%lld", channelUid, static_cast<long long>(start));
}

} // namespace

std::vector<Timer> BuildChildren(const Timer& rule,
                                 const ChannelEvents& guide,
                                 const std::set<std::string>& recordedEpisodes,
                                 time_t now)
{
  std::vector<Timer> children;
  const auto makeChild = [&](int channelUid, time_t start, time_t end) {
    Timer child;
    child.kind = TimerKind::Child;
    child.parentId = rule.id;
    child.channelUid = channelUid;
    child.start = start;
    child.end = end;
    child.marginBefore = rule.marginBefore;
    child.marginAfter = rule.marginAfter;
    child.title = rule.title;
    child.enabled = rule.enabled;
    return child;
  };

  if (rule.manual)
  {
    // The clock time comes from the rule's own start and end.
    const time_t length = rule.end - rule.start;
    time_t start = rule.start;
    while (start + length < now)
      start += 86400;
    for (int day = 0; day < kManualRepeatDays; ++day, start += 86400)
    {
      if ((rule.weekdays & WeekdayBit(start)) == 0 || start < rule.firstDay)
        continue;
      if (rule.skipped.count(AiringKey(rule.channelUid, start)) == 0)
        children.push_back(makeChild(rule.channelUid, start, start + length));
    }
    return children;
  }

  const std::string wanted = ToLower(rule.search.empty() ? rule.title : rule.search);
  std::set<std::string> chosenEpisodes;
  std::vector<std::pair<int, const GuideEvent*>> matches;

  for (const auto& channel : guide)
  {
    if (rule.channelUid != 0 && rule.channelUid != channel.first)
      continue;
    for (const GuideEvent& event : channel.second)
    {
      if (event.end <= now || ToLower(event.title) != wanted)
        continue;
      if (event.start < rule.firstDay || (rule.weekdays & WeekdayBit(event.start)) == 0)
        continue;
      if (!rule.startAnyTime && MinuteOfDay(event.start) < MinuteOfDay(rule.start))
        continue;
      if (!rule.endAnyTime && MinuteOfDay(event.end) > MinuteOfDay(rule.end))
        continue;
      if (rule.skipped.count(AiringKey(channel.first, event.start)))
        continue;
      matches.emplace_back(channel.first, &event);
    }
  }

  std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
    return a.second->start != b.second->start ? a.second->start < b.second->start
                                              : a.first < b.first;
  });

  for (const auto& match : matches)
  {
    const GuideEvent& event = *match.second;
    const std::string key = event.EpisodeKey();
    if (rule.newOnly)
    {
      if (!event.isNew || (!key.empty() && recordedEpisodes.count(key)))
        continue;
      // One copy when an episode airs more than once.
      if (!key.empty() && !chosenEpisodes.insert(key).second)
        continue;
    }
    Timer child = makeChild(match.first, event.start, event.end);
    child.title = event.title;
    child.epgUid = event.Uid();
    child.event = event;
    children.push_back(std::move(child));
  }
  return children;
}

void MarkConflicts(std::vector<Timer>& timers, int tunerCount)
{
  std::vector<Timer*> order;
  for (Timer& timer : timers)
  {
    if (timer.state == TimerState::Conflict)
      timer.state = TimerState::Scheduled;
    if (timer.kind != TimerKind::Series && timer.enabled &&
        (timer.state == TimerState::Scheduled || timer.state == TimerState::Recording))
      order.push_back(&timer);
  }

  std::stable_sort(order.begin(), order.end(), [](const Timer* a, const Timer* b) {
    const bool aActive = a->state == TimerState::Recording;
    const bool bActive = b->state == TimerState::Recording;
    if (aActive != bActive)
      return aActive;
    return a->WindowStart() < b->WindowStart();
  });

  std::vector<const Timer*> accepted;
  for (Timer* timer : order)
  {
    std::vector<time_t> points{timer->WindowStart()};
    for (const Timer* other : accepted)
      if (other->WindowStart() > timer->WindowStart() && other->WindowStart() < timer->WindowEnd())
        points.push_back(other->WindowStart());

    bool fits = true;
    for (const time_t point : points)
    {
      std::set<int> channels;
      for (const Timer* other : accepted)
        if (other->WindowStart() <= point && point < other->WindowEnd())
          channels.insert(other->channelUid);
      if (channels.count(timer->channelUid) == 0 && static_cast<int>(channels.size()) >= tunerCount)
        fits = false;
    }

    if (fits)
    {
      accepted.push_back(timer);
    }
    else if (timer->state != TimerState::Recording)
    {
      timer->state = TimerState::Conflict;
      timer->note = "No free tuner: too many recordings overlap";
    }
  }
}

class Scheduler::FileSink : public IStreamSink
{
public:
  bool Open(const std::string& path)
  {
    m_file = OpenFileShared(path, "ab");
    return m_file != nullptr;
  }
  ~FileSink() override { Close(); }

  bool OnData(const uint8_t* data, size_t size) override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file)
      return false;
    if (std::fwrite(data, 1, size, m_file) != size)
    {
      m_failed = true;
      return false;
    }
    m_bytes += size;
    // A reader may be playing this file while it grows.
    std::fflush(m_file);
    return true;
  }
  void OnGap() override { ++m_gaps; }
  void Close()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file)
      std::fclose(m_file);
    m_file = nullptr;
  }

  bool Failed() const { return m_failed; }
  int Gaps() const { return m_gaps; }
  uint64_t Bytes() const { return m_bytes; }

private:
  std::mutex m_mutex;
  std::FILE* m_file = nullptr;
  std::atomic<bool> m_failed{false};
  std::atomic<int> m_gaps{0};
  std::atomic<uint64_t> m_bytes{0};
};

struct Scheduler::Active
{
  unsigned int timerId = 0;
  Timer timer;
  std::string recordingId;
  std::thread thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> shutdown{false};
  std::atomic<bool> finished{false};
  std::mutex mutex;
  std::condition_variable wake;
};

Scheduler::Scheduler(TunerPool& pool, RecordingStore& store, GuideStore& guide, ISchedulerHost& host)
  : m_pool(pool), m_store(store), m_guide(guide), m_host(host)
{
}

Scheduler::~Scheduler()
{
  Stop();
}

void Scheduler::Start(const std::string& stateFile)
{
  m_stateFile = stateFile;
  Load();

  const int interrupted = m_store.RecoverInterrupted();
  if (interrupted > 0)
    m_host.NotifyUser(true, Format("%d recording(s) were cut off when Kodi last stopped", interrupted));

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop = false;
    RebuildSeriesLocked(std::time(nullptr));
    UpdateConflictsLocked();
  }
  m_thread = std::thread([this] { Run(); });
}

void Scheduler::Stop()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop = true;
    m_wake.notify_all();
  }
  if (m_thread.joinable())
    m_thread.join();

  std::map<unsigned int, std::shared_ptr<Active>> active;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    active = m_active;
  }
  for (const auto& item : active)
  {
    item.second->shutdown = true;
    item.second->stop = true;
    item.second->wake.notify_all();
  }
  for (const auto& item : active)
    if (item.second->thread.joinable())
      item.second->thread.join();

  std::lock_guard<std::mutex> lock(m_mutex);
  m_active.clear();
  if (m_dirty)
    SaveLocked();
}

Timer* Scheduler::FindLocked(unsigned int id)
{
  for (Timer& timer : m_timers)
    if (timer.id == id)
      return &timer;
  return nullptr;
}

void Scheduler::Wake()
{
  m_wake.notify_all();
}

unsigned int Scheduler::AddTimer(Timer timer, std::string* error)
{
  const time_t now = std::time(nullptr);
  if (timer.kind == TimerKind::Once && timer.WindowEnd() <= now)
  {
    if (error)
      *error = "That time has already passed";
    return 0;
  }
  if (m_store.Folder().empty())
  {
    if (error)
      *error = "Choose a recordings folder in the add-on settings first";
    return 0;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  timer.id = m_nextId++;
  timer.state = TimerState::Scheduled;
  m_timers.push_back(std::move(timer));
  RebuildSeriesLocked(now);
  UpdateConflictsLocked();
  SaveLocked();
  m_notifyTimers = true;
  Wake();
  return m_nextId - 1;
}

bool Scheduler::UpdateTimer(const Timer& update)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  Timer* timer = FindLocked(update.id);
  if (!timer)
    return false;

  const TimerState state = timer->state;
  const std::set<std::string> skipped = timer->skipped;
  const unsigned int parent = timer->parentId;
  const GuideEvent event = timer->event;
  *timer = update;
  timer->state = state == TimerState::Recording ? state : TimerState::Scheduled;
  timer->note.clear();
  timer->skipped = skipped;
  timer->parentId = parent;
  if (timer->event.title.empty())
    timer->event = event;

  auto active = m_active.find(update.id);
  if (active != m_active.end())
    active->second->wake.notify_all();

  if (timer->kind == TimerKind::Series)
  {
    // Upcoming airings follow the rule again; anything recording carries on.
    m_timers.erase(std::remove_if(m_timers.begin(), m_timers.end(),
                                  [&](const Timer& child) {
                                    return child.parentId == update.id &&
                                           child.state != TimerState::Recording;
                                  }),
                   m_timers.end());
    RebuildSeriesLocked(std::time(nullptr));
  }
  UpdateConflictsLocked();
  SaveLocked();
  m_notifyTimers = true;
  Wake();
  return true;
}

bool Scheduler::DeleteTimer(unsigned int id)
{
  std::vector<std::shared_ptr<Active>> stopping;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    Timer* timer = FindLocked(id);
    if (!timer)
      return false;

    if (timer->kind == TimerKind::Child)
    {
      // Without this the next guide refresh would add the airing right back.
      if (Timer* rule = FindLocked(timer->parentId))
        rule->skipped.insert(AiringKey(timer->channelUid, timer->start));
    }

    const auto remove = [&](unsigned int timerId) {
      auto active = m_active.find(timerId);
      if (active != m_active.end())
        stopping.push_back(active->second);
    };
    remove(id);
    const bool isSeries = timer->kind == TimerKind::Series;
    for (const Timer& child : m_timers)
      if (isSeries && child.parentId == id)
        remove(child.id);

    m_timers.erase(std::remove_if(m_timers.begin(), m_timers.end(),
                                  [&](const Timer& item) {
                                    return item.id == id || (isSeries && item.parentId == id);
                                  }),
                   m_timers.end());
    UpdateConflictsLocked();
    SaveLocked();
    m_notifyTimers = true;
    Wake();
  }

  for (const auto& active : stopping)
  {
    active->stop = true;
    active->wake.notify_all();
  }
  return true;
}

std::vector<Timer> Scheduler::GetTimers() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_timers;
}

void Scheduler::GuideChanged()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  RebuildSeriesLocked(std::time(nullptr));
  UpdateConflictsLocked();
  SaveLocked();
  m_notifyTimers = true;
  Wake();
}

void Scheduler::TunerCountChanged()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  UpdateConflictsLocked();
  m_notifyTimers = true;
  Wake();
}

bool Scheduler::IsRecording(const std::string& recordingId) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const auto& item : m_active)
    if (item.second->recordingId == recordingId && !item.second->finished)
      return true;
  return false;
}

int Scheduler::ActiveRecordings() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<int>(m_active.size());
}

void Scheduler::RebuildSeriesLocked(time_t now)
{
  const ChannelEvents guide = m_guide.Snapshot();
  std::vector<Timer> additions;

  for (const Timer& rule : m_timers)
  {
    if (rule.kind != TimerKind::Series)
      continue;

    std::vector<Timer> wanted = BuildChildren(rule, guide, m_recordedEpisodes, now);
    std::set<std::string> wantedKeys;
    for (const Timer& child : wanted)
      wantedKeys.insert(AiringKey(child.channelUid, child.start));

    // Drop airings the guide no longer lists, keeping anything that started or failed.
    m_timers.erase(
        std::remove_if(m_timers.begin(), m_timers.end(),
                       [&](const Timer& child) {
                         return child.parentId == rule.id && child.state != TimerState::Recording &&
                                child.state != TimerState::Error && child.WindowStart() > now &&
                                !rule.manual &&
                                wantedKeys.count(AiringKey(child.channelUid, child.start)) == 0;
                       }),
        m_timers.end());

    for (Timer& child : wanted)
    {
      const bool exists =
          std::any_of(m_timers.begin(), m_timers.end(), [&](const Timer& timer) {
            return timer.parentId == rule.id && timer.channelUid == child.channelUid &&
                   timer.start == child.start;
          });
      if (!exists && child.WindowEnd() > now)
        additions.push_back(std::move(child));
    }
  }

  for (Timer& child : additions)
  {
    child.id = m_nextId++;
    m_timers.push_back(std::move(child));
  }
}

void Scheduler::UpdateConflictsLocked()
{
  MarkConflicts(m_timers, m_pool.TunerCount());
  m_dirty = true;
}

void Scheduler::SaveLocked()
{
  if (m_stateFile.empty())
    return;

  Json root = Json::Object();
  root.Set("nextId", m_nextId);
  Json timers = Json::Array();
  for (const Timer& timer : m_timers)
    timers.Push(TimerToJson(timer));
  root.Set("timers", std::move(timers));
  Json history = Json::Array();
  for (const std::string& key : m_recordedEpisodes)
    history.Push(key);
  root.Set("recordedEpisodes", std::move(history));

  if (WriteFileAtomic(m_stateFile, root.Dump()))
    m_dirty = false;
  else
    Log(LogLevel::Error, "Can't save timers to %s", m_stateFile.c_str());
}

void Scheduler::Load()
{
  std::string text;
  Json root;
  if (!ReadWholeFile(m_stateFile, text) || !Json::Parse(text, root) || !root.IsObject())
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  m_nextId = static_cast<unsigned int>(root["nextId"].AsNumber(1));
  m_timers.clear();
  for (const Json& item : root["timers"].Items())
    m_timers.push_back(TimerFromJson(item));
  for (const Json& key : root["recordedEpisodes"].Items())
    m_recordedEpisodes.insert(key.AsString());
}

void Scheduler::Run()
{
  std::unique_lock<std::mutex> lock(m_mutex);
  time_t lastRebuild = std::time(nullptr);
  while (!m_stop)
  {
    const time_t now = std::time(nullptr);
    if (now - lastRebuild >= kRebuildSeconds)
    {
      lastRebuild = now;
      RebuildSeriesLocked(now);
      UpdateConflictsLocked();
      m_notifyTimers = true;
    }

    Tick(now);

    if (m_dirty)
      SaveLocked();

    if (m_notifyTimers)
    {
      m_notifyTimers = false;
      lock.unlock();
      m_host.TimersChanged();
      lock.lock();
      continue;
    }
    m_wake.wait_for(lock, std::chrono::seconds(1));
  }
}

void Scheduler::Tick(time_t now)
{
  // Reap finished recordings.
  for (auto it = m_active.begin(); it != m_active.end();)
  {
    if (it->second->finished)
    {
      if (it->second->thread.joinable())
        it->second->thread.join();
      it = m_active.erase(it);
    }
    else
    {
      ++it;
    }
  }

  const time_t keepFailed = now - kKeepFailedDays * 86400;
  const size_t before = m_timers.size();
  m_timers.erase(std::remove_if(m_timers.begin(), m_timers.end(),
                                [&](const Timer& timer) {
                                  return timer.state == TimerState::Error && timer.end < keepFailed;
                                }),
                 m_timers.end());
  if (m_timers.size() != before)
    m_dirty = m_notifyTimers = true;

  std::vector<std::string> missed;
  for (Timer& timer : m_timers)
  {
    if (timer.kind == TimerKind::Series || timer.state == TimerState::Error ||
        m_active.count(timer.id))
      continue;

    if (!timer.enabled)
    {
      if (timer.state != TimerState::Disabled)
      {
        timer.state = TimerState::Disabled;
        m_notifyTimers = true;
      }
      continue;
    }
    if (timer.state == TimerState::Disabled)
    {
      timer.state = TimerState::Scheduled;
      UpdateConflictsLocked();
      m_notifyTimers = true;
    }

    if (timer.WindowEnd() <= now)
    {
      timer.state = TimerState::Error;
      timer.note = "Missed: Kodi wasn't running";
      missed.push_back(timer.title);
      m_dirty = m_notifyTimers = true;
      continue;
    }

    if (timer.WindowStart() > now)
      continue;

    auto active = std::make_shared<Active>();
    active->timerId = timer.id;
    active->timer = timer;
    timer.state = TimerState::Recording;
    timer.note.clear();
    m_active[timer.id] = active;
    m_notifyTimers = true;
    active->thread = std::thread([this, active] { RunRecording(active); });
  }

  for (const std::string& title : missed)
    m_host.NotifyUser(true, "Missed recording: " + title);
}

void Scheduler::RunRecording(const std::shared_ptr<Active>& active)
{
  Timer timer = active->timer;
  Channel channel;
  RecordingInfo info;
  info.title = timer.title;
  info.channelUid = timer.channelUid;
  info.start = std::time(nullptr);
  info.episodeTitle = timer.event.episodeTitle;
  info.plot = timer.event.plot;
  info.icon = timer.event.icon;
  info.genre = timer.event.genre;
  info.firstAired = timer.event.firstAired;
  info.season = timer.event.season;
  info.episode = timer.event.episode;
  info.epgUid = timer.epgUid;
  info.episodeKey = timer.event.EpisodeKey();
  info.ruleId = timer.parentId;

  const bool startedLate = info.start > timer.start + 30;
  std::string problem;

  auto sink = std::make_shared<FileSink>();
  if (!m_host.LookupChannel(timer.channelUid, channel))
    problem = "The channel is gone from the tuner";
  else
    info.channelName = channel.name;

  if (problem.empty() && (!m_store.Create(info) || !sink->Open(m_store.MediaPath(info.id))))
    problem = "Can't write to the recordings folder";

  if (!problem.empty())
  {
    Log(LogLevel::Error, "Recording %s failed: %s", timer.title.c_str(), problem.c_str());
    m_host.NotifyUser(true, timer.title + ": " + problem);
    info.status = "failed";
    info.note = problem;
    if (!info.id.empty())
      m_store.Delete(info.id);
    Finish(active, info, false);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    active->recordingId = info.id;
  }
  m_host.RecordingsChanged();
  m_host.NotifyUser(false, "Recording started: " + timer.title);
  Log(LogLevel::Info, "Recording %s on %s to %s", timer.title.c_str(), channel.guideNumber.c_str(),
      info.id.c_str());

  std::unique_ptr<TunerLease> lease;
  TuneResult lastResult = TuneResult::Ok;
  bool lostTime = startedLate;

  const auto detach = [&] {
    if (lease)
      lease->Pump().RemoveSink(sink);
    lease.reset();
  };

  while (!active->stop)
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (const Timer* current = FindLocked(active->timerId))
        timer = *current;
    }
    if (std::time(nullptr) >= timer.WindowEnd())
      break;

    if (sink->Failed())
    {
      problem = "The disk is full or the recordings folder failed";
      break;
    }

    if (lease && !lease->Pump().Running())
    {
      lostTime = true;
      detach();
    }

    if (!lease)
    {
      TuneResult result = TuneResult::Ok;
      lease = m_pool.Acquire(channel, TunerUseKind::Recording, result);
      if (lease)
      {
        lease->Pump().AddSink(sink);
        lastResult = TuneResult::Ok;
      }
      else
      {
        lostTime = true;
        if (result != lastResult)
        {
          lastResult = result;
          const std::string text = timer.title + ": " + TuneResultText(result) + ". Still trying.";
          Log(LogLevel::Warning, "%s", text.c_str());
          m_host.NotifyUser(true, text);
        }
        std::unique_lock<std::mutex> lock(active->mutex);
        active->wake.wait_for(lock, std::chrono::seconds(kRetrySeconds),
                              [&] { return active->stop.load(); });
        continue;
      }
    }

    std::unique_lock<std::mutex> lock(active->mutex);
    active->wake.wait_for(lock, std::chrono::milliseconds(250),
                          [&] { return active->stop.load(); });
  }

  detach();
  sink->Close();

  info.duration = static_cast<int>(std::time(nullptr) - info.start);
  if (sink->Bytes() == 0)
  {
    info.status = "failed";
    info.note = !problem.empty()                    ? problem
                : lastResult != TuneResult::Ok ? TuneResultText(lastResult)
                                               : "No data came from the tuner";
    m_store.Delete(info.id);
    m_host.NotifyUser(true, timer.title + ": recording failed. " + info.note);
  }
  else if (!problem.empty() || lostTime || sink->Gaps() > 0 || active->shutdown)
  {
    info.status = "partial";
    info.note = !problem.empty()    ? problem
                : active->shutdown  ? "Stopped when Kodi closed"
                : startedLate       ? "Started late"
                : sink->Gaps() > 0  ? "The stream dropped during recording"
                                    : "A tuner wasn't available for all of it";
    m_store.Save(info);
    if (!active->shutdown)
      m_host.NotifyUser(true, timer.title + ": recording is incomplete. " + info.note);
  }
  else
  {
    info.status = "complete";
    m_store.Save(info);
    m_host.NotifyUser(false, "Recording finished: " + timer.title);
  }

  Log(LogLevel::Info, "Recording %s ended: %s %llu bytes %s", timer.title.c_str(), info.status.c_str(),
      static_cast<unsigned long long>(sink->Bytes()), info.note.c_str());
  Finish(active, info, active->shutdown);
}

void Scheduler::Finish(const std::shared_ptr<Active>& active, const RecordingInfo& info, bool shutdown)
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    Timer* timer = FindLocked(active->timerId);
    const time_t now = std::time(nullptr);

    if (timer && shutdown && timer->WindowEnd() > now)
    {
      // Kodi closed part way through. If it comes back in time, recording carries on.
      timer->state = TimerState::Scheduled;
    }
    else if (timer && info.status == "failed")
    {
      timer->state = TimerState::Error;
      timer->note = info.note;
    }
    else if (timer)
    {
      unsigned int ruleId = timer->parentId;
      m_timers.erase(std::remove_if(m_timers.begin(), m_timers.end(),
                                    [&](const Timer& item) { return item.id == active->timerId; }),
                     m_timers.end());
      if (!info.episodeKey.empty() && info.status == "complete")
        m_recordedEpisodes.insert(info.episodeKey);
      if (const Timer* rule = FindLocked(ruleId))
      {
        const int pruned = m_store.Prune(ruleId, rule->keep);
        if (pruned > 0)
          Log(LogLevel::Info, "Deleted %d old recording(s) of %s", pruned, rule->title.c_str());
        RebuildSeriesLocked(now);
      }
    }
    UpdateConflictsLocked();
    m_notifyTimers = true;
    active->finished = true;
    Wake();
  }
  m_host.RecordingsChanged();
}

} // namespace dt
