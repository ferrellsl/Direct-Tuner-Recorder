/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Common.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <mutex>

#include <cstring>

#ifdef _WIN32
#include <filesystem>
#include <share.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace dt
{
namespace
{

std::mutex g_logMutex;
LogSink g_logSink;

std::string FormatV(const char* format, va_list args)
{
  va_list copy;
  va_copy(copy, args);
  const int size = std::vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (size <= 0)
    return {};
  std::string text(static_cast<size_t>(size) + 1, '\0');
  std::vsnprintf(text.data(), text.size(), format, args);
  text.resize(static_cast<size_t>(size));
  return text;
}

} // namespace

void SetLogSink(LogSink sink)
{
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_logSink = std::move(sink);
}

void Log(LogLevel level, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  const std::string text = FormatV(format, args);
  va_end(args);

  std::lock_guard<std::mutex> lock(g_logMutex);
  if (g_logSink)
    g_logSink(level, text);
}

std::string Format(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  std::string text = FormatV(format, args);
  va_end(args);
  return text;
}

std::string ToLower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string Trim(const std::string& text)
{
  size_t first = 0;
  size_t last = text.size();
  while (first < last && std::isspace(static_cast<unsigned char>(text[first])))
    ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
    --last;
  return text.substr(first, last - first);
}

std::string UrlEncode(const std::string& text)
{
  std::string out;
  for (const char c : text)
  {
    const auto byte = static_cast<unsigned char>(c);
    if (std::isalnum(byte) || c == '-' || c == '_' || c == '.' || c == '~')
      out.push_back(c);
    else
      out += Format("%%%02X", byte);
  }
  return out;
}

std::string UrlDecode(const std::string& text)
{
  std::string out;
  for (size_t i = 0; i < text.size(); ++i)
  {
    if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(text[i + 2])))
    {
      out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    }
    else
    {
      out.push_back(text[i] == '+' ? ' ' : text[i]);
    }
  }
  return out;
}

#ifdef _WIN32
namespace
{
std::filesystem::path WidePath(const std::string& path)
{
  return std::filesystem::path(std::u8string(path.begin(), path.end()));
}
} // namespace

std::string JoinPath(const std::string& folder, const std::string& name)
{
  if (folder.empty())
    return name;
  const char last = folder.back();
  return last == '/' || last == '\\' ? folder + name : folder + "\\" + name;
}

std::string ParentPath(const std::string& path)
{
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool CreateDirectories(const std::string& path)
{
  std::error_code error;
  std::filesystem::create_directories(WidePath(path), error);
  return !error && std::filesystem::is_directory(WidePath(path), error);
}

bool RemovePath(const std::string& path)
{
  std::error_code error;
  return std::filesystem::remove(WidePath(path), error) && !error;
}

bool RenamePath(const std::string& from, const std::string& to)
{
  // Unlike rename(), this replaces an existing file on Windows.
  std::error_code error;
  std::filesystem::rename(WidePath(from), WidePath(to), error);
  return !error;
}

bool IsDirectory(const std::string& path)
{
  std::error_code error;
  return std::filesystem::is_directory(WidePath(path), error);
}

bool IsEmptyDirectory(const std::string& path)
{
  std::error_code error;
  return std::filesystem::is_empty(WidePath(path), error) && !error;
}

time_t ModifiedTime(const std::string& path)
{
  struct _stat64 info;
  const std::wstring wide = WidePath(path).wstring();
  return _wstat64(wide.c_str(), &info) == 0 ? static_cast<time_t>(info.st_mtime) : 0;
}

bool DiskSpace(const std::string& path, uint64_t& total, uint64_t& available)
{
  std::error_code error;
  const std::filesystem::space_info space = std::filesystem::space(WidePath(path), error);
  total = space.capacity;
  available = space.available;
  return !error;
}

std::vector<std::string> ListFiles(const std::string& root, const std::string& extension)
{
  std::vector<std::string> files;
  std::error_code error;
  const std::filesystem::path base = WidePath(root);
  auto it = std::filesystem::recursive_directory_iterator(
      base, std::filesystem::directory_options::skip_permission_denied, error);
  for (; !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
  {
    if (it->is_regular_file(error) && it->path().extension() == WidePath(extension))
    {
      const std::u8string relative = it->path().lexically_relative(base).generic_u8string();
      files.emplace_back(relative.begin(), relative.end());
    }
  }
  return files;
}

std::FILE* OpenFileShared(const std::string& path, const char* mode)
{
  const std::wstring widePath = WidePath(path).wstring();
  const std::wstring wideMode(mode, mode + std::char_traits<char>::length(mode));
  return _wfsopen(widePath.c_str(), wideMode.c_str(), _SH_DENYNO);
}

int SeekFile(std::FILE* file, int64_t offset, int whence)
{
  return _fseeki64(file, offset, whence);
}

int64_t TellFile(std::FILE* file)
{
  return _ftelli64(file);
}

int64_t FileSize(const std::string& path)
{
  struct _stat64 info;
  const std::wstring wide = WidePath(path).wstring();
  return _wstat64(wide.c_str(), &info) == 0 ? static_cast<int64_t>(info.st_size) : -1;
}

#else

// 32-bit Android before API 24 has no 64-bit stdio seeking, so files go unbuffered there and
// seeking moves the descriptor directly.
#if defined(__ANDROID__) && !defined(__LP64__) && __ANDROID_API__ < 24
#define DT_DESCRIPTOR_SEEK 1
#endif

std::string JoinPath(const std::string& folder, const std::string& name)
{
  if (folder.empty())
    return name;
  return folder.back() == '/' ? folder + name : folder + "/" + name;
}

std::string ParentPath(const std::string& path)
{
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return {};
  return slash == 0 ? std::string("/") : path.substr(0, slash);
}

bool CreateDirectories(const std::string& path)
{
  if (path.empty())
    return false;
  if (IsDirectory(path))
    return true;
  const std::string parent = ParentPath(path);
  if (!parent.empty() && parent != path)
    CreateDirectories(parent);
  return mkdir(path.c_str(), 0775) == 0 || IsDirectory(path);
}

bool RemovePath(const std::string& path)
{
  return std::remove(path.c_str()) == 0;
}

bool RenamePath(const std::string& from, const std::string& to)
{
  return std::rename(from.c_str(), to.c_str()) == 0;
}

bool IsDirectory(const std::string& path)
{
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsEmptyDirectory(const std::string& path)
{
  DIR* dir = opendir(path.c_str());
  if (!dir)
    return false;
  bool empty = true;
  while (dirent* entry = readdir(dir))
  {
    if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
    {
      empty = false;
      break;
    }
  }
  closedir(dir);
  return empty;
}

time_t ModifiedTime(const std::string& path)
{
  struct stat info;
  return stat(path.c_str(), &info) == 0 ? info.st_mtime : 0;
}

bool DiskSpace(const std::string& path, uint64_t& total, uint64_t& available)
{
  struct statvfs info;
  if (statvfs(path.c_str(), &info) != 0)
    return false;
  total = static_cast<uint64_t>(info.f_blocks) * info.f_frsize;
  available = static_cast<uint64_t>(info.f_bavail) * info.f_frsize;
  return true;
}

namespace
{
void ListInto(const std::string& root,
              const std::string& relative,
              const std::string& extension,
              int depth,
              std::vector<std::string>& files)
{
  if (depth > 16)
    return;
  DIR* dir = opendir(JoinPath(root, relative).c_str());
  if (!dir)
    return;
  while (dirent* entry = readdir(dir))
  {
    const std::string name = entry->d_name;
    if (name == "." || name == "..")
      continue;
    const std::string child = relative.empty() ? name : relative + "/" + name;
    if (IsDirectory(JoinPath(root, child)))
      ListInto(root, child, extension, depth + 1, files);
    else if (name.size() > extension.size() &&
             name.compare(name.size() - extension.size(), extension.size(), extension) == 0)
      files.push_back(child);
  }
  closedir(dir);
}
} // namespace

std::vector<std::string> ListFiles(const std::string& root, const std::string& extension)
{
  std::vector<std::string> files;
  ListInto(root, {}, extension, 0, files);
  return files;
}

std::FILE* OpenFileShared(const std::string& path, const char* mode)
{
  std::FILE* file = std::fopen(path.c_str(), mode);
#ifdef DT_DESCRIPTOR_SEEK
  if (file)
    std::setvbuf(file, nullptr, _IONBF, 0);
#endif
  return file;
}

int SeekFile(std::FILE* file, int64_t offset, int whence)
{
#ifdef DT_DESCRIPTOR_SEEK
  std::clearerr(file);
  return lseek64(fileno(file), offset, whence) < 0 ? -1 : 0;
#else
  return fseeko(file, static_cast<off_t>(offset), whence);
#endif
}

int64_t TellFile(std::FILE* file)
{
#ifdef DT_DESCRIPTOR_SEEK
  return lseek64(fileno(file), 0, SEEK_CUR);
#else
  return static_cast<int64_t>(ftello(file));
#endif
}

int64_t FileSize(const std::string& path)
{
#ifdef DT_DESCRIPTOR_SEEK
  struct stat64 info;
  return stat64(path.c_str(), &info) == 0 ? static_cast<int64_t>(info.st_size) : -1;
#else
  struct stat info;
  return stat(path.c_str(), &info) == 0 ? static_cast<int64_t>(info.st_size) : -1;
#endif
}

#endif

bool ReadWholeFile(const std::string& path, std::string& out)
{
  std::FILE* file = OpenFileShared(path, "rb");
  if (!file)
    return false;

  out.clear();
  char buffer[65536];
  size_t count;
  while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    out.append(buffer, count);
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

bool WriteFileAtomic(const std::string& path, const std::string& text)
{
  // On a fresh install Kodi hasn't created the add-on's data folder yet.
  CreateDirectories(ParentPath(path));

  const std::string temporary = path + ".tmp";
  std::FILE* file = OpenFileShared(temporary, "wb");
  if (!file)
    return false;

  const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  const bool closed = std::fclose(file) == 0;
  const bool moved = written && closed && RenamePath(temporary, path);
  if (!moved)
    RemovePath(temporary);
  return moved;
}

std::string SafeFileName(const std::string& name)
{
  std::string safe;
  safe.reserve(name.size());
  for (const char c : name)
  {
    const auto u = static_cast<unsigned char>(c);
    if (u < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos)
      safe.push_back('_');
    else
      safe.push_back(c);
  }
  safe = Trim(safe);
  while (!safe.empty() && safe.back() == '.')
    safe.pop_back();
  if (safe.size() > 120)
    safe.resize(120);
  return safe.empty() ? "Recording" : safe;
}

} // namespace dt
