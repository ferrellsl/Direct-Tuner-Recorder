/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "Guide.h"
#include "Pump.h"
#include "RecordingStore.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace dt
{

enum class TimerKind
{
  Once,
  Series,
  Child
};

enum class TimerState
{
  Scheduled,
  Recording,
  Conflict,
  Disabled,
  Error
};

struct Timer
{
  unsigned int id = 0;
  TimerKind kind = TimerKind::Once;
  TimerState state = TimerState::Scheduled;
  bool enabled = true;
  bool manual = false;
  int channelUid = 0; // 0 on a series rule means any channel
  time_t start = 0;
  time_t end = 0;
  int marginBefore = 0; // minutes
  int marginAfter = 0;
  std::string title;
  std::string note; // what went wrong, shown to the user

  // From the guide, for a single airing.
  unsigned int epgUid = 0;
  GuideEvent event;

  // Child timers.
  unsigned int parentId = 0;

  // Series rules. A manual rule repeats a clock time instead of searching the guide.
  std::string search;
  unsigned int weekdays = 0x7F; // Monday is bit 0
  time_t firstDay = 0;
  bool startAnyTime = true;
  bool endAnyTime = true;
  bool newOnly = false;
  int keep = 0;                  // 0 keeps everything
  std::set<std::string> skipped; // airings the user removed, as "channel:start"

  time_t WindowStart() const { return start - marginBefore * 60; }
  time_t WindowEnd() const { return end + marginAfter * 60; }
};

// The airings a series rule wants, ignoring those already handled. Plain function for testing.
std::vector<Timer> BuildChildren(const Timer& rule,
                                 const ChannelEvents& guide,
                                 const std::set<std::string>& recordedEpisodes,
                                 time_t now);

// Sets Conflict on timers that can't get a tuner, and clears it on the rest. An overlap on one
// channel shares a tuner. Earlier timers win.
void MarkConflicts(std::vector<Timer>& timers, int tunerCount);

class ISchedulerHost
{
public:
  virtual ~ISchedulerHost() = default;
  virtual void NotifyUser(bool isError, const std::string& text) = 0;
  virtual void TimersChanged() = 0;
  virtual void RecordingsChanged() = 0;
  virtual bool LookupChannel(int uid, Channel& out) = 0;
};

class Scheduler
{
public:
  Scheduler(TunerPool& pool, RecordingStore& store, GuideStore& guide, ISchedulerHost& host);
  ~Scheduler();

  void Start(const std::string& stateFile);
  void Stop();

  unsigned int AddTimer(Timer timer, std::string* error = nullptr);
  bool UpdateTimer(const Timer& timer);
  bool DeleteTimer(unsigned int id);
  std::vector<Timer> GetTimers() const;

  // Rebuilds the airings of every series rule.
  void GuideChanged();
  void TunerCountChanged();
  bool IsRecording(const std::string& recordingId) const;
  int ActiveRecordings() const;

private:
  struct Active;
  class FileSink;

  void Run();
  void Tick(time_t now);
  void RunRecording(const std::shared_ptr<Active>& active);
  void Finish(const std::shared_ptr<Active>& active, const RecordingInfo& info, bool shutdown);
  void RebuildSeriesLocked(time_t now);
  void UpdateConflictsLocked();
  void SaveLocked();
  void Load();
  Timer* FindLocked(unsigned int id);
  void Wake();

  TunerPool& m_pool;
  RecordingStore& m_store;
  GuideStore& m_guide;
  ISchedulerHost& m_host;

  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::thread m_thread;
  bool m_stop = false;
  bool m_dirty = false;
  bool m_notifyTimers = false;
  std::string m_stateFile;
  unsigned int m_nextId = 1;
  std::vector<Timer> m_timers;
  std::set<std::string> m_recordedEpisodes;
  std::map<unsigned int, std::shared_ptr<Active>> m_active;
};

} // namespace dt
