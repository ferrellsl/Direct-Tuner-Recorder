/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Wire.h"

namespace dt
{

Json EventToJson(const GuideEvent& event)
{
  Json json = Json::Object();
  json.Set("start", static_cast<long long>(event.start));
  json.Set("end", static_cast<long long>(event.end));
  json.Set("title", event.title);
  json.Set("episodeTitle", event.episodeTitle);
  json.Set("plot", event.plot);
  json.Set("icon", event.icon);
  json.Set("genre", event.genre);
  json.Set("programId", event.programId);
  json.Set("seriesId", event.seriesId);
  json.Set("firstAired", event.firstAired);
  json.Set("season", event.season);
  json.Set("episode", event.episode);
  json.Set("isNew", event.isNew);
  return json;
}

GuideEvent EventFromJson(const Json& json)
{
  GuideEvent event;
  event.start = static_cast<time_t>(json["start"].AsInt64());
  event.end = static_cast<time_t>(json["end"].AsInt64());
  event.title = json["title"].AsString();
  event.episodeTitle = json["episodeTitle"].AsString();
  event.plot = json["plot"].AsString();
  event.icon = json["icon"].AsString();
  event.genre = json["genre"].AsString();
  event.programId = json["programId"].AsString();
  event.seriesId = json["seriesId"].AsString();
  event.firstAired = json["firstAired"].AsString();
  event.season = json["season"].AsInt(-1);
  event.episode = json["episode"].AsInt(-1);
  event.isNew = json["isNew"].AsBool();
  return event;
}

Json TimerToJson(const Timer& timer, bool withState)
{
  Json json = Json::Object();
  json.Set("id", timer.id);
  json.Set("kind", static_cast<int>(timer.kind));
  json.Set("error", timer.state == TimerState::Error);
  if (withState)
    json.Set("state", static_cast<int>(timer.state));
  json.Set("enabled", timer.enabled);
  json.Set("manual", timer.manual);
  json.Set("channelUid", timer.channelUid);
  json.Set("start", static_cast<long long>(timer.start));
  json.Set("end", static_cast<long long>(timer.end));
  json.Set("marginBefore", timer.marginBefore);
  json.Set("marginAfter", timer.marginAfter);
  json.Set("title", timer.title);
  json.Set("note", timer.note);
  json.Set("epgUid", timer.epgUid);
  json.Set("event", EventToJson(timer.event));
  json.Set("parentId", timer.parentId);
  json.Set("search", timer.search);
  json.Set("weekdays", timer.weekdays);
  json.Set("firstDay", static_cast<long long>(timer.firstDay));
  json.Set("startAnyTime", timer.startAnyTime);
  json.Set("endAnyTime", timer.endAnyTime);
  json.Set("newOnly", timer.newOnly);
  json.Set("keep", timer.keep);
  Json skipped = Json::Array();
  for (const std::string& key : timer.skipped)
    skipped.Push(key);
  json.Set("skipped", std::move(skipped));
  return json;
}

Timer TimerFromJson(const Json& json)
{
  Timer timer;
  timer.id = static_cast<unsigned int>(json["id"].AsNumber());
  timer.kind = static_cast<TimerKind>(json["kind"].AsInt());
  if (json.Has("state"))
    timer.state = static_cast<TimerState>(json["state"].AsInt());
  else
    timer.state = json["error"].AsBool() ? TimerState::Error : TimerState::Scheduled;
  timer.enabled = json["enabled"].AsBool(true);
  timer.manual = json["manual"].AsBool();
  timer.channelUid = json["channelUid"].AsInt();
  timer.start = static_cast<time_t>(json["start"].AsInt64());
  timer.end = static_cast<time_t>(json["end"].AsInt64());
  timer.marginBefore = json["marginBefore"].AsInt();
  timer.marginAfter = json["marginAfter"].AsInt();
  timer.title = json["title"].AsString();
  timer.note = json["note"].AsString();
  timer.epgUid = static_cast<unsigned int>(json["epgUid"].AsNumber());
  timer.event = EventFromJson(json["event"]);
  timer.parentId = static_cast<unsigned int>(json["parentId"].AsNumber());
  timer.search = json["search"].AsString();
  timer.weekdays = static_cast<unsigned int>(json["weekdays"].AsNumber(0x7F));
  timer.firstDay = static_cast<time_t>(json["firstDay"].AsInt64());
  timer.startAnyTime = json["startAnyTime"].AsBool(true);
  timer.endAnyTime = json["endAnyTime"].AsBool(true);
  timer.newOnly = json["newOnly"].AsBool();
  timer.keep = json["keep"].AsInt();
  for (const Json& key : json["skipped"].Items())
    timer.skipped.insert(key.AsString());
  return timer;
}

Json RecordingToJson(const RecordingInfo& info, bool withId)
{
  Json json = Json::Object();
  if (withId)
  {
    json.Set("id", info.id);
    json.Set("size", static_cast<long long>(info.size));
  }
  json.Set("title", info.title);
  json.Set("episodeTitle", info.episodeTitle);
  json.Set("plot", info.plot);
  json.Set("icon", info.icon);
  json.Set("channelName", info.channelName);
  json.Set("genre", info.genre);
  json.Set("firstAired", info.firstAired);
  json.Set("episodeKey", info.episodeKey);
  json.Set("status", info.status);
  json.Set("note", info.note);
  json.Set("channelUid", info.channelUid);
  json.Set("start", static_cast<long long>(info.start));
  json.Set("duration", info.duration);
  json.Set("season", info.season);
  json.Set("episode", info.episode);
  json.Set("epgUid", info.epgUid);
  json.Set("ruleId", info.ruleId);
  return json;
}

RecordingInfo RecordingFromJson(const Json& json)
{
  RecordingInfo info;
  info.id = json["id"].AsString();
  info.size = json["size"].AsInt64();
  info.title = json["title"].AsString();
  info.episodeTitle = json["episodeTitle"].AsString();
  info.plot = json["plot"].AsString();
  info.icon = json["icon"].AsString();
  info.channelName = json["channelName"].AsString();
  info.genre = json["genre"].AsString();
  info.firstAired = json["firstAired"].AsString();
  info.episodeKey = json["episodeKey"].AsString();
  info.status = json["status"].AsString("complete");
  info.note = json["note"].AsString();
  info.channelUid = json["channelUid"].AsInt();
  info.start = static_cast<time_t>(json["start"].AsInt64());
  info.duration = json["duration"].AsInt();
  info.season = json["season"].AsInt(-1);
  info.episode = json["episode"].AsInt(-1);
  info.epgUid = static_cast<unsigned int>(json["epgUid"].AsNumber());
  info.ruleId = static_cast<unsigned int>(json["ruleId"].AsNumber());
  return info;
}

Json ChannelToJson(const Channel& channel)
{
  Json json = Json::Object();
  json.Set("uid", channel.uid);
  json.Set("guideNumber", channel.guideNumber);
  json.Set("name", channel.name);
  json.Set("videoCodec", channel.videoCodec);
  json.Set("audioCodec", channel.audioCodec);
  json.Set("icon", channel.icon);
  json.Set("major", channel.major);
  json.Set("minor", channel.minor);
  json.Set("hd", channel.hd);
  json.Set("drm", channel.drm);
  return json;
}

Channel ChannelFromJson(const Json& json)
{
  Channel channel;
  channel.uid = json["uid"].AsInt();
  channel.guideNumber = json["guideNumber"].AsString();
  channel.name = json["name"].AsString();
  channel.videoCodec = json["videoCodec"].AsString();
  channel.audioCodec = json["audioCodec"].AsString();
  channel.icon = json["icon"].AsString();
  channel.major = json["major"].AsInt();
  channel.minor = json["minor"].AsInt();
  channel.hd = json["hd"].AsBool();
  channel.drm = json["drm"].AsBool();
  return channel;
}

} // namespace dt
