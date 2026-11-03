/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Pump.h"

#include "Common.h"

#include <algorithm>
#include <chrono>
#include <cstring>


namespace dt
{
namespace
{

constexpr size_t kPacketSize = 188;
constexpr size_t kPumpBuffer = kPacketSize * 2048;
constexpr size_t kFlushBytes = kPacketSize * 256;
constexpr int kTuneTimeoutMs = 13000;
constexpr int kStallSeconds = 8;
constexpr size_t kMaxQueuedBytes = 256 * 1024 * 1024; // about 2 minutes of the busiest channel
constexpr auto kSlowDelivery = std::chrono::milliseconds(750);
// Keeps the reader clear of the part of the ring that's about to be overwritten.
constexpr int64_t kTailMargin = 8 * 1024 * 1024;

} // namespace

std::string TuneResultText(TuneResult result)
{
  switch (result)
  {
    case TuneResult::Ok:
      return "OK";
    case TuneResult::AllTunersBusy:
      return "All tuners are in use";
    case TuneResult::NoSignal:
      return "No signal on this channel";
    case TuneResult::ChannelNotFound:
      return "The tuner doesn't have this channel";
    case TuneResult::NetworkError:
      return "Can't reach the tuner";
    case TuneResult::TunerError:
      return "The tuner refused the stream";
  }
  return "Unknown error";
}

StreamPump::StreamPump(Channel channel) : m_channel(std::move(channel))
{
}

StreamPump::~StreamPump()
{
  Stop();
}

TuneResult StreamPump::Connect()
{
  if (!m_stream.Open(m_channel.url, kTuneTimeoutMs))
    return TuneResult::NetworkError;
  if (m_stream.Status() == 200)
    return TuneResult::Ok;

  const int error = m_stream.DeviceError();
  const int status = m_stream.Status();
  m_stream.Close();
  Log(LogLevel::Warning, "Tuner answered %d (error %d) for channel %s", status, error,
      m_channel.guideNumber.c_str());
  if (error == 805 || status == 403)
    return TuneResult::AllTunersBusy;
  if (error == 807 || error == 806)
    return TuneResult::NoSignal;
  if (status == 404)
    return TuneResult::ChannelNotFound;
  return TuneResult::TunerError;
}

TuneResult StreamPump::Start()
{
  const TuneResult result = Connect();
  if (result != TuneResult::Ok)
    return result;
  m_running = true;
  m_deliverThread = std::thread([this] { Deliver(); });
  m_thread = std::thread([this] { Run(); });
  return result;
}

void StreamPump::Stop()
{
  m_stop = true;
  m_stream.Close();
  if (m_thread.joinable())
    m_thread.join();
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_readerDone = true;
    m_queueReady.notify_all();
  }
  if (m_deliverThread.joinable())
    m_deliverThread.join();
  m_running = false;
}

void StreamPump::AddSink(std::shared_ptr<IStreamSink> sink)
{
  std::lock_guard<std::mutex> lock(m_sinkMutex);
  m_sinks.push_back(std::move(sink));
}

void StreamPump::RemoveSink(const std::shared_ptr<IStreamSink>& sink)
{
  std::lock_guard<std::mutex> lock(m_sinkMutex);
  m_sinks.erase(std::remove(m_sinks.begin(), m_sinks.end(), sink), m_sinks.end());
}

void StreamPump::Emit(const uint8_t* data, size_t size)
{
  m_bytes += size;
  std::lock_guard<std::mutex> lock(m_queueMutex);
  if (m_queuedBytes + size > kMaxQueuedBytes)
  {
    if (!m_dropping)
    {
      Log(LogLevel::Error, "Writing can't keep up on channel %s: dropping stream data",
          m_channel.guideNumber.c_str());
      m_dropping = true;
      m_queue.emplace_back(); // the sinks hear about the gap
    }
    return;
  }
  m_dropping = false;
  m_queue.emplace_back(data, data + size);
  m_queuedBytes += size;
  m_queueReady.notify_one();
}

void StreamPump::NotifyGap()
{
  std::lock_guard<std::mutex> lock(m_queueMutex);
  m_queue.emplace_back();
  m_queueReady.notify_one();
}

void StreamPump::Deliver()
{
  while (true)
  {
    std::vector<uint8_t> block;
    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      m_queueReady.wait(lock, [this] { return !m_queue.empty() || m_readerDone; });
      if (m_queue.empty())
        return;
      block = std::move(m_queue.front());
      m_queue.pop_front();
      m_queuedBytes -= block.size();
    }

    const auto started = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_sinkMutex);
    if (block.empty())
    {
      ++m_gaps;
      for (const auto& sink : m_sinks)
        sink->OnGap();
      continue;
    }

    for (auto it = m_sinks.begin(); it != m_sinks.end();)
    {
      if ((*it)->OnData(block.data(), block.size()))
        ++it;
      else
        it = m_sinks.erase(it);
    }

    const auto taken = std::chrono::steady_clock::now() - started;
    if (taken > kSlowDelivery)
      Log(LogLevel::Warning, "Disk write stalled for %d ms on channel %s",
          static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(taken).count()),
          m_channel.guideNumber.c_str());
  }
}

void StreamPump::Run()
{
  std::vector<uint8_t> buffer(kPumpBuffer);
  size_t fill = 0;
  auto lastData = std::chrono::steady_clock::now();

  const auto flush = [&] {
    size_t start = 0;
    while (start + kPacketSize < fill &&
           !(buffer[start] == 0x47 && buffer[start + kPacketSize] == 0x47))
      ++start;
    const size_t whole = (fill - start) / kPacketSize * kPacketSize;
    if (whole)
      Emit(buffer.data() + start, whole);
    fill -= start + whole;
    std::memmove(buffer.data(), buffer.data() + start + whole, fill);
  };

  while (!m_stop)
  {
    const int count = m_stream.Read(buffer.data() + fill, buffer.size() - fill, 250);
    const auto now = std::chrono::steady_clock::now();
    if (count > 0)
    {
      fill += static_cast<size_t>(count);
      lastData = now;
      if (fill >= kFlushBytes)
        flush();
      continue;
    }

    if (fill >= kPacketSize * 2)
      flush();

    if (count == 0 && now - lastData < std::chrono::seconds(kStallSeconds))
      continue;
    if (m_stop)
      break;

    Log(LogLevel::Warning, "Stream for channel %s dropped, reconnecting",
        m_channel.guideNumber.c_str());
    m_reconnecting = true;
    NotifyGap();
    fill = 0;
    while (!m_stop && Connect() != TuneResult::Ok)
    {
      for (int i = 0; i < 20 && !m_stop; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    m_reconnecting = false;
    lastData = std::chrono::steady_clock::now();
  }

  m_stream.Close();
  m_running = false;
}

TunerLease::TunerLease(TunerPool& pool, std::shared_ptr<StreamPump> pump, TunerUseKind kind)
  : m_pool(pool), m_pump(std::move(pump)), m_kind(kind)
{
}

TunerLease::~TunerLease()
{
  m_pool.Release(m_pump, m_kind);
}

void TunerPool::SetTunerCount(int count)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_tunerCount = std::max(count, 1);
}

int TunerPool::TunerCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_tunerCount;
}

void TunerPool::SetPreemptHandler(PreemptHandler handler)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_preemptHandler = std::move(handler);
}

int TunerPool::ActiveCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<int>(m_slots.size());
}

bool TunerPool::PreemptLive(std::unique_lock<std::mutex>& lock)
{
  for (auto it = m_slots.begin(); it != m_slots.end(); ++it)
  {
    if (it->second.recordings > 0 || it->second.starting)
      continue;

    auto pump = it->second.pump;
    const Channel channel = pump->GetChannel();
    const PreemptHandler handler = m_preemptHandler;
    m_slots.erase(it);
    Log(LogLevel::Info, "Taking the tuner from live TV on %s for a recording",
        channel.guideNumber.c_str());
    lock.unlock();
    pump->Stop();
    if (handler)
      handler(channel);
    lock.lock();
    return true;
  }
  return false;
}

std::unique_ptr<TunerLease> TunerPool::Acquire(const Channel& channel,
                                               TunerUseKind kind,
                                               TuneResult& result)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto share = [&](Slot& slot) {
    (kind == TunerUseKind::Live ? slot.live : slot.recordings)++;
    result = TuneResult::Ok;
    return std::make_unique<TunerLease>(*this, slot.pump, kind);
  };

  while (true)
  {
    auto existing = m_slots.find(channel.uid);
    if (existing != m_slots.end())
    {
      if (existing->second.starting)
      {
        m_changed.wait(lock);
        continue;
      }
      return share(existing->second);
    }

    if (static_cast<int>(m_slots.size()) >= m_tunerCount)
    {
      if (kind != TunerUseKind::Recording || !PreemptLive(lock))
      {
        result = TuneResult::AllTunersBusy;
        return nullptr;
      }
      continue;
    }

    Slot& slot = m_slots[channel.uid];
    slot.pump = std::make_shared<StreamPump>(channel);
    slot.starting = true;
    auto pump = slot.pump;
    lock.unlock();
    result = pump->Start();
    lock.lock();

    auto started = m_slots.find(channel.uid);
    if (result == TuneResult::Ok)
    {
      started->second.starting = false;
      m_changed.notify_all();
      return share(started->second);
    }

    m_slots.erase(started);
    m_changed.notify_all();

    // Something outside this add-on holds the tuners. Live TV may be the way to free one.
    if (result == TuneResult::AllTunersBusy && kind == TunerUseKind::Recording && PreemptLive(lock))
      continue;
    return nullptr;
  }
}

void TunerPool::Release(const std::shared_ptr<StreamPump>& pump, TunerUseKind kind)
{
  std::shared_ptr<StreamPump> stopping;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_slots.find(pump->GetChannel().uid);
    if (it == m_slots.end() || it->second.pump != pump)
      return; // already taken by a recording
    (kind == TunerUseKind::Live ? it->second.live : it->second.recordings)--;
    if (it->second.live > 0 || it->second.recordings > 0)
      return;
    stopping = it->second.pump;
    m_slots.erase(it);
    m_changed.notify_all();
  }
  stopping->Stop();
}

TimeshiftBuffer::TimeshiftBuffer(std::string path, int64_t capacityBytes)
  : m_path(std::move(path)),
    m_capacity(std::max<int64_t>(capacityBytes, 64 * 1024 * 1024) / 188 * 188)
{
}

TimeshiftBuffer::~TimeshiftBuffer()
{
  if (m_writer)
    std::fclose(m_writer);
  if (m_reader)
    std::fclose(m_reader);
  RemovePath(m_path);
}

bool TimeshiftBuffer::Open()
{
  m_writer = OpenFileShared(m_path, "wb");
  m_reader = OpenFileShared(m_path, "rb");
  if (!m_writer || !m_reader)
    return false;
  std::setvbuf(m_writer, nullptr, _IONBF, 0);
  std::setvbuf(m_reader, nullptr, _IONBF, 0);
  m_start = std::time(nullptr);
  return true;
}

bool TimeshiftBuffer::OnData(const uint8_t* data, size_t size)
{
  int64_t head;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    head = m_head;
  }

  size_t done = 0;
  while (done < size)
  {
    const int64_t offset = (head + static_cast<int64_t>(done)) % m_capacity;
    const size_t count = std::min<size_t>(size - done, static_cast<size_t>(m_capacity - offset));
    if (SeekFile(m_writer, offset, SEEK_SET) != 0 ||
        std::fwrite(data + done, 1, count, m_writer) != count)
    {
      Log(LogLevel::Error, "Can't write the timeshift file (disk full?)");
      m_failed = true;
      MarkEnded();
      return false;
    }
    done += count;
  }

  const time_t now = std::time(nullptr);
  std::lock_guard<std::mutex> lock(m_mutex);
  m_head += static_cast<int64_t>(size);
  if (m_index.empty() || m_index.back().second != now)
    m_index.emplace_back(m_head, now);
  const int64_t oldest = OldestLocked();
  while (m_index.size() > 1 && m_index[1].first <= oldest)
    m_index.pop_front();
  m_data.notify_all();
  return true;
}

int64_t TimeshiftBuffer::OldestLocked() const
{
  const int64_t oldest = m_head - m_capacity + kTailMargin;
  return oldest <= 0 ? 0 : oldest / 188 * 188;
}

int TimeshiftBuffer::Read(uint8_t* buffer, size_t size, int timeoutMs)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (true)
  {
    const int64_t oldest = OldestLocked();
    if (m_read < oldest)
      m_read = oldest; // paused longer than the buffer holds
    if (m_read < m_head)
      break;
    if (m_ended || m_interrupted)
      return -1;
    if (m_data.wait_until(lock, deadline) == std::cv_status::timeout)
      return 0;
  }

  const int64_t offset = m_read % m_capacity;
  const size_t count = static_cast<size_t>(std::min<int64_t>(
      {static_cast<int64_t>(size), m_head - m_read, m_capacity - offset}));
  lock.unlock();

  if (SeekFile(m_reader, offset, SEEK_SET) != 0)
    return -1;
  const size_t got = std::fread(buffer, 1, count, m_reader);

  lock.lock();
  m_read += static_cast<int64_t>(got);
  return static_cast<int>(got);
}

int64_t TimeshiftBuffer::Seek(int64_t position)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_read = std::clamp(position, OldestLocked(), m_head) / 188 * 188;
  return m_read;
}

int64_t TimeshiftBuffer::Position() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_read;
}

int64_t TimeshiftBuffer::Length() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_head;
}

int64_t TimeshiftBuffer::OldestPosition() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return OldestLocked();
}

void TimeshiftBuffer::MarkEnded()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_ended = true;
  m_data.notify_all();
}

void TimeshiftBuffer::Interrupt()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_interrupted = true;
  m_data.notify_all();
}

TimeshiftBuffer::Times TimeshiftBuffer::GetTimes() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  Times times;
  times.start = m_start;
  times.newest = m_index.empty() ? m_start : m_index.back().second;
  times.oldest = m_index.empty() || OldestLocked() == 0 ? m_start : m_index.front().second;
  return times;
}

int64_t TimeshiftBuffer::SecondsBehind() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  for (auto it = m_index.rbegin(); it != m_index.rend(); ++it)
    if (it->first <= m_read)
      return static_cast<int64_t>(m_index.back().second - it->second);
  return m_index.empty() ? 0 : static_cast<int64_t>(m_index.back().second - m_index.front().second);
}

} // namespace dt
