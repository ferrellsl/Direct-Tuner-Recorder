/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Device.h"

#include "Common.h"
#include "Json.h"
#include "Net.h"

#include <cstdio>
#include <set>

namespace dt
{

int ChannelUid(const std::string& guideNumber, int* major, int* minor)
{
  int first = 0;
  int second = 0;
  if (std::sscanf(guideNumber.c_str(), "%d.%d", &first, &second) < 1 || first < 0 || second < 0)
    return 0;
  if (major)
    *major = first;
  if (minor)
    *minor = second;
  return first * 10000 + second;
}

bool FetchDevice(const std::string& host, DeviceInfo& out)
{
  std::string base = host;
  if (base.rfind("http://", 0) != 0)
    base = "http://" + base;
  while (!base.empty() && base.back() == '/')
    base.pop_back();

  std::string body;
  int status = 0;
  if (!HttpGetText(base + "/discover.json", 4000, body, &status) || status != 200)
    return false;

  Json json;
  if (!Json::Parse(body, json) || !json.IsObject())
    return false;

  out.id = json["DeviceID"].AsString();
  out.model = json["ModelNumber"].AsString();
  out.friendlyName = json["FriendlyName"].AsString();
  out.firmware = json["FirmwareVersion"].AsString();
  out.baseUrl = json["BaseURL"].AsString(base);
  out.lineupUrl = json["LineupURL"].AsString(out.baseUrl + "/lineup.json");
  out.auth = json["DeviceAuth"].AsString();
  out.tunerCount = json["TunerCount"].AsInt();
  return out.tunerCount > 0;
}

bool ParseLineup(const std::string& text, bool hideDrm, std::vector<Channel>& out)
{
  Json json;
  if (!Json::Parse(text, json) || !json.IsArray())
    return false;

  out.clear();
  std::set<int> seen;
  for (const Json& item : json.Items())
  {
    Channel channel;
    channel.guideNumber = item["GuideNumber"].AsString();
    channel.name = item["GuideName"].AsString();
    channel.url = item["URL"].AsString();
    channel.videoCodec = item["VideoCodec"].AsString();
    channel.audioCodec = item["AudioCodec"].AsString();
    channel.hd = item["HD"].AsBool();
    channel.drm = item["DRM"].AsBool();
    channel.uid = ChannelUid(channel.guideNumber, &channel.major, &channel.minor);

    // The real lineup has an entry with no URL, which can't be tuned.
    if (channel.uid == 0 || channel.url.empty() || (channel.drm && hideDrm))
      continue;
    if (!seen.insert(channel.uid).second)
      continue;
    out.push_back(std::move(channel));
  }
  return true;
}

bool FetchLineup(const DeviceInfo& device, bool hideDrm, std::vector<Channel>& out)
{
  std::string body;
  int status = 0;
  if (!HttpGetText(device.lineupUrl, 8000, body, &status) || status != 200)
    return false;
  return ParseLineup(body, hideDrm, out);
}

bool FetchTunerStatus(const DeviceInfo& device, std::vector<TunerUse>& out)
{
  std::string body;
  int status = 0;
  if (!HttpGetText(device.baseUrl + "/status.json", 3000, body, &status) || status != 200)
    return false;

  Json json;
  if (!Json::Parse(body, json) || !json.IsArray())
    return false;

  out.clear();
  for (const Json& item : json.Items())
  {
    TunerUse use;
    use.resource = item["Resource"].AsString();
    if (use.resource.rfind("tuner", 0) != 0)
      continue;
    use.channel = item["VctNumber"].AsString();
    use.targetIp = item["TargetIP"].AsString();
    use.signalStrength = item["SignalStrengthPercent"].AsInt();
    use.signalQuality = item["SignalQualityPercent"].AsInt();
    use.symbolQuality = item["SymbolQualityPercent"].AsInt();
    out.push_back(std::move(use));
  }
  return true;
}

bool FindDevice(const std::string& manualHost, DeviceInfo& out, std::string& error)
{
  if (!manualHost.empty())
  {
    if (FetchDevice(manualHost, out))
      return true;
    Log(LogLevel::Warning, "No tuner answered at the manual address %s", manualHost.c_str());
  }

  const std::vector<std::string> hosts = DiscoverDevices(1200);
  for (const std::string& host : hosts)
  {
    if (FetchDevice(host, out))
    {
      if (hosts.size() > 1)
        Log(LogLevel::Warning, "%d tuners found: using %s. Set an address to choose another.",
            static_cast<int>(hosts.size()), host.c_str());
      return true;
    }
  }

  error = manualHost.empty()
              ? "No HDHomeRun tuner found on the network. If it is on another subnet, enter its "
                "IP address in the add-on settings."
              : "No tuner found, including at " + manualHost + ".";
  return false;
}

} // namespace dt
