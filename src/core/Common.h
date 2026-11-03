/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace dt
{

enum class LogLevel
{
  Debug,
  Info,
  Warning,
  Error
};

using LogSink = std::function<void(LogLevel, const std::string&)>;

// The core has no Kodi types: the add-on or a test program installs the log sink.
void SetLogSink(LogSink sink);
void Log(LogLevel level, const char* format, ...);

std::string Format(const char* format, ...);
std::string ToLower(std::string text);
std::string Trim(const std::string& text);
std::string UrlEncode(const std::string& text);
std::string UrlDecode(const std::string& text);

// Paths are UTF-8 strings throughout, as they come from Kodi. These wrap the file system because
// std::filesystem is missing from the Android NDK that Kodi 21 builds with.
std::string JoinPath(const std::string& folder, const std::string& name);
std::string ParentPath(const std::string& path);
bool CreateDirectories(const std::string& path);
// Removes a file or an empty folder.
bool RemovePath(const std::string& path);
bool RenamePath(const std::string& from, const std::string& to);
bool IsDirectory(const std::string& path);
bool IsEmptyDirectory(const std::string& path);
time_t ModifiedTime(const std::string& path); // 0 when missing
bool DiskSpace(const std::string& path, uint64_t& total, uint64_t& available);
// Files below root with this extension, as paths relative to root with / between folders.
std::vector<std::string> ListFiles(const std::string& root, const std::string& extension);

std::FILE* OpenFileShared(const std::string& path, const char* mode);
// 64-bit offsets everywhere, including 32-bit Android before API 24.
int SeekFile(std::FILE* file, int64_t offset, int whence);
int64_t TellFile(std::FILE* file);
int64_t FileSize(const std::string& path);
bool ReadWholeFile(const std::string& path, std::string& out);
// Writes through a temporary file, so a crash can't leave half a file behind.
bool WriteFileAtomic(const std::string& path, const std::string& text);

// A file name made safe for every file system we might record to.
std::string SafeFileName(const std::string& name);

} // namespace dt
