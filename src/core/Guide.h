/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "Device.h"

#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dt
{

struct GuideEvent
{
  time_t start = 0;
  time_t end = 0;
  std::string title;
  std::string episodeTitle;
  std::string plot;
  std::string icon;
  std::string genre;      // text as the source gave it
  std::string programId;  // series or episode ID from the source
  std::string seriesId;
  std::string firstAired; // YYYY-MM-DD
  int season = -1;
  int episode = -1;
  bool isNew = false;

  // Unique within a channel and stable across guide refreshes.
  unsigned int Uid() const { return static_cast<unsigned int>(start); }
  // Identifies the episode, to skip repeats. Empty when the source doesn't say.
  std::string EpisodeKey() const;
};

using ChannelEvents = std::map<int, std::vector<GuideEvent>>;

// Fetches a URL, which may be https. The add-on supplies this through Kodi.
using UrlFetcher = std::function<bool(const std::string& url, std::string& body)>;

// One page of SiliconDust's guide.php reply. Channel icons go into icons, keyed by channel uid.
bool ParseDeviceGuide(const std::string& json, ChannelEvents& events, std::map<int, std::string>& icons);
// Reads the whole guide: pages are 24 hours and the service answers about 2 days ahead.
bool FetchDeviceGuide(const DeviceInfo& device, const UrlFetcher& fetch, ChannelEvents& events,
                      std::map<int, std::string>& icons);

struct XmltvStats
{
  int programmes = 0;
  int channelsMatched = 0;
};

// Matches channels by a number in a display-name or the channel id, then number minus 100, which
// is how ATSC 3.0 channels appear on this tuner.
bool ParseXmltv(const std::string& xml, const std::vector<Channel>& lineup, ChannelEvents& events,
                XmltvStats* stats = nullptr);

class GuideStore
{
public:
  void SetDeviceGuide(ChannelEvents events);
  void SetXmltv(ChannelEvents events);

  std::vector<GuideEvent> GetEvents(int channelUid, time_t start, time_t end) const;
  // Every channel's merged guide, for matching series rules.
  ChannelEvents Snapshot() const;
  bool FindEvent(int channelUid, unsigned int eventUid, GuideEvent& out) const;
  size_t EventCount() const;

private:
  std::vector<GuideEvent> MergedLocked(int channelUid) const;

  mutable std::mutex m_mutex;
  ChannelEvents m_device;
  ChannelEvents m_xmltv;
};

} // namespace dt
