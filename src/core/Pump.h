/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "Device.h"
#include "Net.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dt
{

enum class TuneResult
{
  Ok,
  AllTunersBusy,
  NoSignal,
  ChannelNotFound,
  NetworkError,
  TunerError
};

std::string TuneResultText(TuneResult result);

class IStreamSink
{
public:
  virtual ~IStreamSink() = default;
  // Always whole 188-byte transport packets. Returning false detaches the sink.
  virtual bool OnData(const uint8_t* data, size_t size) = 0;
  // Stream data was lost at this point.
  virtual void OnGap() {}
  // No more data will come, for example because a recording took the tuner.
  virtual void OnEnd() {}
};

// One tuner's stream, fanned out to any number of sinks. Reconnects when the stream drops.
class StreamPump
{
public:
  explicit StreamPump(Channel channel);
  ~StreamPump();

  TuneResult Start();
  void Stop();

  void AddSink(std::shared_ptr<IStreamSink> sink);
  void RemoveSink(const std::shared_ptr<IStreamSink>& sink);

  const Channel& GetChannel() const { return m_channel; }
  bool Running() const { return m_running; }
  // Set while the stream is down and the pump is trying to get it back.
  bool Reconnecting() const { return m_reconnecting; }
  int GapCount() const { return m_gaps; }
  uint64_t Bytes() const { return m_bytes; }

private:
  TuneResult Connect();
  void Run();
  void Deliver();
  void Emit(const uint8_t* data, size_t size);
  void NotifyGap();

  Channel m_channel;
  HttpStream m_stream;
  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_reconnecting{false};
  std::atomic<int> m_gaps{0};
  std::atomic<uint64_t> m_bytes{0};
  std::mutex m_sinkMutex;
  std::vector<std::shared_ptr<IStreamSink>> m_sinks;

  // The network reader never waits on a disk: a second thread delivers from this queue.
  std::thread m_deliverThread;
  std::mutex m_queueMutex;
  std::condition_variable m_queueReady;
  std::deque<std::vector<uint8_t>> m_queue; // an empty block marks a gap
  size_t m_queuedBytes = 0;
  bool m_readerDone = false;
  bool m_dropping = false;
};

enum class TunerUseKind
{
  Live,
  Recording
};

class TunerPool;

// Holds a share of a tuner until destroyed.
class TunerLease
{
public:
  TunerLease(TunerPool& pool, std::shared_ptr<StreamPump> pump, TunerUseKind kind);
  ~TunerLease();
  StreamPump& Pump() { return *m_pump; }

private:
  TunerPool& m_pool;
  std::shared_ptr<StreamPump> m_pump;
  TunerUseKind m_kind;
};

// Decides who gets a tuner. Watching and recording one channel share one stream, and a recording
// that has no tuner takes one back from live TV, never the reverse.
class TunerPool
{
public:
  using PreemptHandler = std::function<void(const Channel&)>;

  void SetTunerCount(int count);
  int TunerCount() const;
  void SetPreemptHandler(PreemptHandler handler);

  std::unique_ptr<TunerLease> Acquire(const Channel& channel, TunerUseKind kind, TuneResult& result);
  int ActiveCount() const;

private:
  friend class TunerLease;

  struct Slot
  {
    std::shared_ptr<StreamPump> pump;
    int live = 0;
    int recordings = 0;
    bool starting = false;
  };

  void Release(const std::shared_ptr<StreamPump>& pump, TunerUseKind kind);
  bool PreemptLive(std::unique_lock<std::mutex>& lock);

  mutable std::mutex m_mutex;
  std::condition_variable m_changed;
  std::map<int, Slot> m_slots;
  int m_tunerCount = 1;
  PreemptHandler m_preemptHandler;
};

// A fixed-size ring on disk that live TV plays from. Writing goes on while the reader is paused,
// so timeshift is simply the reader sitting behind the writer.
class TimeshiftBuffer : public IStreamSink
{
public:
  struct Times
  {
    time_t start = 0;
    time_t oldest = 0;
    time_t newest = 0;
  };

  TimeshiftBuffer(std::string path, int64_t capacityBytes);
  ~TimeshiftBuffer() override;

  bool Open();
  bool OnData(const uint8_t* data, size_t size) override;
  void OnEnd() override { MarkEnded(); }

  // Blocks up to timeoutMs for data. 0 on timeout, -1 after MarkEnded() with nothing left.
  int Read(uint8_t* buffer, size_t size, int timeoutMs);
  int64_t Seek(int64_t position);
  int64_t Position() const;
  int64_t Length() const;
  int64_t OldestPosition() const;
  void MarkEnded();
  void Interrupt();
  Times GetTimes() const;
  // Seconds behind the live edge.
  int64_t SecondsBehind() const;
  bool IsFailed() const { return m_failed; }

private:
  int64_t OldestLocked() const;

  std::string m_path;
  int64_t m_capacity;
  std::FILE* m_writer = nullptr;
  std::FILE* m_reader = nullptr;
  mutable std::mutex m_mutex;
  std::condition_variable m_data;
  int64_t m_head = 0;
  int64_t m_read = 0;
  bool m_ended = false;
  bool m_interrupted = false;
  std::atomic<bool> m_failed{false};
  time_t m_start = 0;
  std::deque<std::pair<int64_t, time_t>> m_index;
};

} // namespace dt
