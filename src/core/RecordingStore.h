/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace dt
{

struct RecordingInfo
{
  std::string id; // path below the recordings folder, with / and no extension
  std::string title;
  std::string episodeTitle;
  std::string plot;
  std::string icon;
  std::string channelName;
  std::string genre;
  std::string firstAired;
  std::string episodeKey;
  std::string status = "recording"; // recording, complete, partial or failed
  std::string note;                 // why it's partial or failed
  int channelUid = 0;
  time_t start = 0;
  int duration = 0;
  int season = -1;
  int episode = -1;
  unsigned int epgUid = 0;
  unsigned int ruleId = 0;
  int64_t size = 0;
};

// Each recording is a .ts file with a .json file beside it, so the recordings folder
// describes itself and survives reinstalling Kodi.
class RecordingStore
{
public:
  void SetFolder(const std::string& folder);
  std::string Folder() const;

  std::vector<RecordingInfo> List() const;
  bool Find(const std::string& id, RecordingInfo& out) const;
  bool Delete(const std::string& id);

  // Chooses the id and creates the folder for a new recording.
  bool Create(RecordingInfo& info);
  bool Save(const RecordingInfo& info);
  std::string MediaPath(const std::string& id) const;

  // A recording still marked "recording" at startup was cut off by a crash.
  int RecoverInterrupted();
  // Oldest first, leaving the newest keep recordings of the rule. Returns how many it deleted.
  int Prune(unsigned int ruleId, int keep);
  bool DriveSpace(uint64_t& total, uint64_t& used) const;

private:
  std::string SidecarPath(const std::string& id) const;

  mutable std::mutex m_mutex;
  std::string m_folder;
};

} // namespace dt
