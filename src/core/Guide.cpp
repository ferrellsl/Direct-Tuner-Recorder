/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Guide.h"

#include "Common.h"
#include "Json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace dt
{
namespace
{

time_t TimeFromUtc(int year, int month, int day, int hour, int minute, int second)
{
  // Days from civil, valid for the Gregorian calendar.
  year -= month <= 2;
  const long era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153u * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = era * 146097LL + static_cast<long long>(doe) - 719468;
  return static_cast<time_t>(days * 86400 + hour * 3600 + minute * 60 + second);
}

std::string DateText(time_t time)
{
  std::tm parts{};
#ifdef _WIN32
  gmtime_s(&parts, &time);
#else
  gmtime_r(&time, &parts);
#endif
  char text[16];
  std::strftime(text, sizeof(text), "%Y-%m-%d", &parts);
  return text;
}

void ParseEpisodeCode(const std::string& code, int& season, int& episode)
{
  int first = -1;
  int second = -1;
  if (std::sscanf(code.c_str(), "S%dE%d", &first, &second) == 2 ||
      std::sscanf(code.c_str(), "s%de%d", &first, &second) == 2)
  {
    season = first;
    episode = second;
  }
  else if (std::sscanf(code.c_str(), "EP%d", &second) == 1)
  {
    episode = second;
  }
}

void AppendCodePoint(std::string& out, unsigned int code)
{
  if (code < 0x80)
  {
    out.push_back(static_cast<char>(code));
  }
  else if (code < 0x800)
  {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
  else if (code < 0x10000)
  {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
  else
  {
    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

std::string DecodeXmlText(const char* start, const char* end, bool latin1)
{
  std::string out;
  out.reserve(static_cast<size_t>(end - start));
  for (const char* p = start; p < end; ++p)
  {
    if (*p == '&')
    {
      const char* semi = static_cast<const char*>(std::memchr(p, ';', static_cast<size_t>(end - p)));
      if (semi)
      {
        const std::string name(p + 1, semi);
        bool known = true;
        if (name == "amp")
          out.push_back('&');
        else if (name == "lt")
          out.push_back('<');
        else if (name == "gt")
          out.push_back('>');
        else if (name == "quot")
          out.push_back('"');
        else if (name == "apos")
          out.push_back('\'');
        else if (name.size() > 2 && name[0] == '#' && (name[1] == 'x' || name[1] == 'X'))
          AppendCodePoint(out, static_cast<unsigned int>(std::strtoul(name.c_str() + 2, nullptr, 16)));
        else if (name.size() > 1 && name[0] == '#')
          AppendCodePoint(out, static_cast<unsigned int>(std::strtoul(name.c_str() + 1, nullptr, 10)));
        else
          known = false;
        if (known)
        {
          p = semi;
          continue;
        }
      }
    }
    const auto u = static_cast<unsigned char>(*p);
    if (latin1 && u >= 0x80)
      AppendCodePoint(out, u);
    else
      out.push_back(*p);
  }
  return out;
}

// A forward-only reader for the simple shape of XMLTV files.
class XmlReader
{
public:
  struct Tag
  {
    std::string name;
    const char* attrStart = nullptr;
    const char* attrEnd = nullptr;
    bool selfClosed = false;
    bool closing = false;
  };

  XmlReader(const std::string& text) : m_pos(text.data()), m_end(text.data() + text.size()) {}

  bool Next(Tag& tag, const char** textStart = nullptr, const char** textEnd = nullptr)
  {
    while (true)
    {
      const char* open = static_cast<const char*>(std::memchr(m_pos, '<', static_cast<size_t>(m_end - m_pos)));
      if (!open)
        return false;
      if (textStart)
      {
        *textStart = m_pos;
        *textEnd = open;
      }
      if (open + 1 >= m_end)
        return false;
      if (open[1] == '!' || open[1] == '?')
      {
        const char* close = static_cast<const char*>(std::memchr(open, '>', static_cast<size_t>(m_end - open)));
        if (!close)
          return false;
        m_pos = close + 1;
        continue;
      }

      const char* close = static_cast<const char*>(std::memchr(open, '>', static_cast<size_t>(m_end - open)));
      if (!close)
        return false;
      m_pos = close + 1;

      const char* cursor = open + 1;
      tag.closing = *cursor == '/';
      if (tag.closing)
        ++cursor;
      const char* nameStart = cursor;
      while (cursor < close && !std::isspace(static_cast<unsigned char>(*cursor)) && *cursor != '/')
        ++cursor;
      tag.name.assign(nameStart, cursor);
      tag.attrStart = cursor;
      tag.attrEnd = close;
      tag.selfClosed = close[-1] == '/';
      return true;
    }
  }

private:
  const char* m_pos;
  const char* m_end;
};

std::string Attribute(const XmlReader::Tag& tag, const char* name, bool latin1)
{
  const std::string key = std::string(name) + "=";
  const char* p = tag.attrStart;
  while (p < tag.attrEnd)
  {
    const char* found = std::search(p, tag.attrEnd, key.begin(), key.end());
    if (found == tag.attrEnd || found + key.size() >= tag.attrEnd)
      return {};
    const bool wholeWord = found == tag.attrStart || std::isspace(static_cast<unsigned char>(found[-1]));
    const char quote = found[key.size()];
    if (wholeWord && (quote == '"' || quote == '\''))
    {
      const char* start = found + key.size() + 1;
      const char* end = std::find(start, tag.attrEnd, quote);
      return DecodeXmlText(start, end, latin1);
    }
    p = found + key.size();
  }
  return {};
}

time_t ParseXmltvTime(const std::string& text)
{
  int year, month, day, hour, minute, second;
  if (std::sscanf(text.c_str(), "%4d%2d%2d%2d%2d%2d", &year, &month, &day, &hour, &minute, &second) != 6)
    return 0;
  time_t utc = TimeFromUtc(year, month, day, hour, minute, second);
  const size_t space = text.find_first_of("+-", 14);
  if (space != std::string::npos)
  {
    const int zone = std::atoi(text.c_str() + space + 1);
    const int seconds = (zone / 100) * 3600 + (zone % 100) * 60;
    utc += text[space] == '-' ? seconds : -seconds;
  }
  return utc;
}

// The number at the start of a display-name, as in "3.1 KTVKDT" or a bare "3.1".
bool LeadingChannelNumber(const std::string& text, std::string& number)
{
  int major = 0;
  int minor = 0;
  int consumed = 0;
  if (std::sscanf(text.c_str(), "%d.%d%n", &major, &minor, &consumed) != 2)
    return false;
  if (static_cast<size_t>(consumed) < text.size() &&
      !std::isspace(static_cast<unsigned char>(text[static_cast<size_t>(consumed)])))
    return false;
  number = Format("%d.%d", major, minor);
  return true;
}

std::string CleanTitle(std::string title)
{
  // zap2xml marks new shows with a trailing " *". The file also has <new />.
  title = Trim(title);
  if (title.size() > 2 && title.compare(title.size() - 2, 2, " *") == 0)
    title.resize(title.size() - 2);
  return title;
}

void SortAndClean(std::vector<GuideEvent>& events)
{
  std::sort(events.begin(), events.end(),
            [](const GuideEvent& a, const GuideEvent& b) { return a.start < b.start; });
  events.erase(std::unique(events.begin(), events.end(),
                           [](const GuideEvent& a, const GuideEvent& b) { return a.start == b.start; }),
               events.end());
}

} // namespace

std::string GuideEvent::EpisodeKey() const
{
  const std::string lower = ToLower(title);
  if (season >= 0 && episode >= 0)
    return lower + Format("|s%de%d", season, episode);
  if (!programId.empty() && programId.find(".0000") == std::string::npos)
    return lower + "|" + programId;
  if (!episodeTitle.empty())
    return lower + "|" + ToLower(episodeTitle);
  if (!firstAired.empty())
    return lower + "|" + firstAired;
  return {};
}

bool ParseDeviceGuide(const std::string& text, ChannelEvents& events, std::map<int, std::string>& icons)
{
  Json json;
  if (!Json::Parse(text, json) || !json.IsArray())
    return false;

  for (const Json& channel : json.Items())
  {
    const int uid = ChannelUid(channel["GuideNumber"].AsString());
    if (uid == 0)
      continue;
    const std::string icon = channel["ImageURL"].AsString();
    if (!icon.empty())
      icons[uid] = icon;

    auto& list = events[uid];
    for (const Json& item : channel["Guide"].Items())
    {
      GuideEvent event;
      event.start = static_cast<time_t>(item["StartTime"].AsInt64());
      event.end = static_cast<time_t>(item["EndTime"].AsInt64());
      event.title = item["Title"].AsString();
      if (event.start == 0 || event.end <= event.start || event.title.empty())
        continue;
      event.episodeTitle = item["EpisodeTitle"].AsString();
      event.plot = item["Synopsis"].AsString();
      event.icon = item["ImageURL"].AsString();
      event.seriesId = item["SeriesID"].AsString();
      ParseEpisodeCode(item["EpisodeNumber"].AsString(), event.season, event.episode);

      const auto aired = static_cast<time_t>(item["OriginalAirdate"].AsInt64());
      if (aired > 0)
      {
        event.firstAired = DateText(aired);
        event.isNew = aired + 48 * 60 * 60 > event.start;
      }
      if (item["First"].AsBool())
        event.isNew = true;
      if (!item["Filter"].Items().empty())
        event.genre = item["Filter"].Items()[0].AsString();
      list.push_back(std::move(event));
    }
  }
  return true;
}

bool FetchDeviceGuide(const DeviceInfo& device, const UrlFetcher& fetch, ChannelEvents& events,
                      std::map<int, std::string>& icons)
{
  if (device.auth.empty())
    return false;

  std::string auth;
  for (const char c : device.auth)
  {
    if (std::isalnum(static_cast<unsigned char>(c)))
      auth.push_back(c);
    else
      auth += Format("%%%02X", static_cast<unsigned char>(c));
  }

  const time_t now = std::time(nullptr);
  bool any = false;
  for (int day = 0; day < 3; ++day)
  {
    const std::string url = Format("https://api.hdhomerun.com/api/guide.php?DeviceAuth=%s&Start=%lld&Duration=24",
                                   auth.c_str(), static_cast<long long>(now + day * 86400));
    std::string body;
    // Past the end of the guide the service answers 400, which isn't a failure.
    if (!fetch(url, body) || !ParseDeviceGuide(body, events, icons))
      break;
    any = true;
  }
  for (auto& item : events)
    SortAndClean(item.second);
  return any;
}

bool ParseXmltv(const std::string& xml, const std::vector<Channel>& lineup, ChannelEvents& events,
                XmltvStats* stats)
{
  if (xml.find("<tv") == std::string::npos)
    return false;

  // The zap2xml file declares ISO-8859-1.
  const size_t declarationEnd = xml.find("?>");
  const bool latin1 = declarationEnd != std::string::npos &&
                      ToLower(xml.substr(0, declarationEnd)).find("iso-8859-1") != std::string::npos;

  std::map<std::string, std::vector<std::string>> numbersBySourceId;

  XmlReader reader(xml);
  XmlReader::Tag tag;
  const char* textStart = nullptr;
  const char* textEnd = nullptr;

  std::string sourceId;
  std::map<std::string, std::vector<GuideEvent>> bySource;
  GuideEvent event;
  std::string eventSource;
  bool inProgramme = false;
  bool wholeSeasonEpisode = false;

  while (reader.Next(tag, &textStart, &textEnd))
  {
    if (tag.closing)
    {
      const std::string text = textStart ? Trim(DecodeXmlText(textStart, textEnd, latin1)) : std::string();
      if (tag.name == "channel")
      {
        sourceId.clear();
      }
      else if (tag.name == "display-name" && !sourceId.empty())
      {
        std::string number;
        if (LeadingChannelNumber(text, number))
          numbersBySourceId[sourceId].push_back(number);
      }
      else if (tag.name == "programme")
      {
        if (inProgramme && event.start != 0 && event.end > event.start && !event.title.empty())
          bySource[eventSource].push_back(event);
        inProgramme = false;
      }
      else if (inProgramme)
      {
        if (tag.name == "title")
          event.title = CleanTitle(text);
        else if (tag.name == "sub-title")
          event.episodeTitle = text;
        else if (tag.name == "desc")
          event.plot = text;
        else if (tag.name == "category" && event.genre.empty())
          event.genre = text;
        else if (tag.name == "date" && text.size() >= 8)
          event.firstAired = text.substr(0, 4) + "-" + text.substr(4, 2) + "-" + text.substr(6, 2);
      }
      continue;
    }

    if (tag.name == "channel")
    {
      sourceId = Attribute(tag, "id", latin1);
      std::string number;
      if (LeadingChannelNumber(sourceId, number))
        numbersBySourceId[sourceId].push_back(number);
    }
    else if (tag.name == "programme")
    {
      event = GuideEvent();
      event.start = ParseXmltvTime(Attribute(tag, "start", latin1));
      event.end = ParseXmltvTime(Attribute(tag, "stop", latin1));
      eventSource = Attribute(tag, "channel", latin1);
      inProgramme = !tag.selfClosed;
      wholeSeasonEpisode = false;
    }
    else if (inProgramme)
    {
      if (tag.name == "new")
      {
        event.isNew = true;
      }
      else if (tag.name == "icon" && event.icon.empty())
      {
        event.icon = Attribute(tag, "src", latin1);
      }
      else if (tag.name == "episode-num" && !tag.selfClosed)
      {
        const std::string system = Attribute(tag, "system", latin1);
        XmlReader::Tag closing;
        const char* start = nullptr;
        const char* end = nullptr;
        if (reader.Next(closing, &start, &end))
        {
          const std::string text = Trim(DecodeXmlText(start, end, latin1));
          if (system == "onscreen" || system == "SxxExx")
          {
            ParseEpisodeCode(text, event.season, event.episode);
            wholeSeasonEpisode = event.season >= 0;
          }
          else if (system == "xmltv_ns" && !wholeSeasonEpisode)
          {
            int season = -1;
            int episode = -1;
            if (std::sscanf(text.c_str(), "%d.%d", &season, &episode) == 2)
            {
              event.season = season + 1;
              event.episode = episode + 1;
            }
          }
          else if (system == "dd_progid")
          {
            event.programId = text;
          }
        }
      }
    }
  }

  std::map<std::string, std::string> sourceFor;
  for (const auto& source : numbersBySourceId)
    for (const std::string& number : source.second)
      sourceFor.emplace(number, source.first);

  int matched = 0;
  int programmes = 0;
  for (const Channel& channel : lineup)
  {
    auto source = sourceFor.find(channel.guideNumber);
    if (source == sourceFor.end() && channel.major >= 100)
      source = sourceFor.find(Format("%d.%d", channel.major - 100, channel.minor));
    if (source == sourceFor.end())
      continue;
    auto found = bySource.find(source->second);
    if (found == bySource.end() || found->second.empty())
      continue;
    auto& list = events[channel.uid];
    list = found->second;
    SortAndClean(list);
    ++matched;
    programmes += static_cast<int>(list.size());
  }

  if (stats)
  {
    stats->channelsMatched = matched;
    stats->programmes = programmes;
  }
  return true;
}

void GuideStore::SetDeviceGuide(ChannelEvents events)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_device = std::move(events);
}

void GuideStore::SetXmltv(ChannelEvents events)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_xmltv = std::move(events);
}

std::vector<GuideEvent> GuideStore::MergedLocked(int channelUid) const
{
  std::vector<GuideEvent> merged;
  const auto file = m_xmltv.find(channelUid);
  time_t fileEnd = 0;
  if (file != m_xmltv.end() && !file->second.empty())
  {
    merged = file->second;
    fileEnd = merged.back().end;
  }

  // The file wins while it lasts, so a stale file still leaves a guide after it runs out.
  const auto device = m_device.find(channelUid);
  if (device != m_device.end())
    for (const GuideEvent& event : device->second)
      if (event.start >= fileEnd)
        merged.push_back(event);
  return merged;
}

std::vector<GuideEvent> GuideStore::GetEvents(int channelUid, time_t start, time_t end) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<GuideEvent> events;
  for (GuideEvent& event : MergedLocked(channelUid))
    if (event.end > start && event.start < end)
      events.push_back(std::move(event));
  return events;
}

ChannelEvents GuideStore::Snapshot() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::set<int> channels;
  for (const auto& item : m_device)
    channels.insert(item.first);
  for (const auto& item : m_xmltv)
    channels.insert(item.first);

  ChannelEvents snapshot;
  for (const int uid : channels)
    snapshot[uid] = MergedLocked(uid);
  return snapshot;
}

bool GuideStore::FindEvent(int channelUid, unsigned int eventUid, GuideEvent& out) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  for (GuideEvent& event : MergedLocked(channelUid))
  {
    if (event.Uid() == eventUid)
    {
      out = std::move(event);
      return true;
    }
  }
  return false;
}

size_t GuideStore::EventCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto& item : m_device)
    count += item.second.size();
  for (const auto& item : m_xmltv)
    count += item.second.size();
  return count;
}

} // namespace dt
