/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <string>
#include <vector>

namespace dt
{

struct DeviceInfo
{
  std::string id;
  std::string model;
  std::string friendlyName;
  std::string firmware;
  std::string baseUrl;
  std::string lineupUrl;
  std::string auth; // never log this
  int tunerCount = 0;
};

struct Channel
{
  int uid = 0; // stable across restarts: see ChannelUid()
  std::string guideNumber;
  std::string name;
  std::string url;
  std::string videoCodec;
  std::string audioCodec;
  std::string icon;
  int major = 0;
  int minor = 0;
  bool hd = false;
  bool drm = false;
};

struct TunerUse
{
  std::string resource;
  std::string channel; // empty when the tuner is idle
  std::string targetIp;
  int signalStrength = 0;
  int signalQuality = 0;
  int symbolQuality = 0;
};

// "103.1" gives 1030001. Timers and recordings keep this, so it must never change.
int ChannelUid(const std::string& guideNumber, int* major = nullptr, int* minor = nullptr);

bool FetchDevice(const std::string& host, DeviceInfo& out);
bool ParseLineup(const std::string& json, bool hideDrm, std::vector<Channel>& out);
bool FetchLineup(const DeviceInfo& device, bool hideDrm, std::vector<Channel>& out);
bool FetchTunerStatus(const DeviceInfo& device, std::vector<TunerUse>& out);

// A manual address wins; otherwise the broadcast. Fills in error text for the user on failure.
bool FindDevice(const std::string& manualHost, DeviceInfo& out, std::string& error);

} // namespace dt
