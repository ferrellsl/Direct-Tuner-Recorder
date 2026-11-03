/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

// Direct Tuner Recorder: records from an HDHomeRun whether or not Kodi is running, and serves
// every Kodi in the house. Runs as a Windows service, or with --console in a window for testing.

#include "../core/Common.h"
#include "Recorder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <ntsecapi.h>
#else
#include <csignal>
#endif

namespace
{

constexpr const char* kServiceName = "DirectTunerRecorder";
constexpr int64_t kMaxLogBytes = 10 * 1024 * 1024;
// Wake this long before a recording starts, so the network and the tuner are ready.
constexpr int kWakeEarlySeconds = 120;

std::string g_dataFolder;
std::mutex g_logMutex;
bool g_console = false;

std::mutex g_stopMutex;
std::condition_variable g_stopWake;
bool g_stop = false;
std::atomic<bool> g_resume{false};

void RequestStop()
{
  std::lock_guard<std::mutex> lock(g_stopMutex);
  g_stop = true;
  g_stopWake.notify_all();
}

void LogToFile(dt::LogLevel level, const std::string& text)
{
  static const char* names[] = {"debug", "info", "warning", "error"};
  if (level == dt::LogLevel::Debug)
    return;

  const time_t now = std::time(nullptr);
  std::tm parts{};
#ifdef _WIN32
  localtime_s(&parts, &now);
#else
  localtime_r(&now, &parts);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &parts);
  const std::string line =
      dt::Format("%s %-7s %s\n", stamp, names[static_cast<int>(level)], text.c_str());

  std::lock_guard<std::mutex> lock(g_logMutex);
  if (g_console)
    std::fputs(line.c_str(), stdout);
  const std::string path = dt::JoinPath(g_dataFolder, "recorder.log");
  if (dt::FileSize(path) > kMaxLogBytes)
  {
    const std::string old = dt::JoinPath(g_dataFolder, "recorder.old.log");
    dt::RemovePath(old);
    dt::RenamePath(path, old);
  }
  if (std::FILE* file = dt::OpenFileShared(path, "ab"))
  {
    std::fputs(line.c_str(), file);
    std::fclose(file);
  }
}

std::string EnvOr(const char* name, const std::string& fallback)
{
  const char* value = std::getenv(name);
  return value && *value ? value : fallback;
}

std::string DefaultDataFolder()
{
#ifdef _WIN32
  return dt::JoinPath(EnvOr("ProgramData", "C:\\ProgramData"), "Direct Tuner Recorder");
#else
  return "/var/lib/directtuner-recorder";
#endif
}

std::string DefaultRecordingsFolder()
{
#ifdef _WIN32
  return dt::JoinPath(dt::JoinPath(EnvOr("PUBLIC", "C:\\Users\\Public"), "Videos"), "Kodi Recordings");
#else
  return "/var/lib/directtuner-recorder/recordings";
#endif
}

// Keeps the computer awake while it's recording or relaying live TV, and sets a wake timer for
// the next recording so a sleeping computer still makes it.
class PowerKeeper
{
public:
  explicit PowerKeeper(dt::Recorder& recorder) : m_recorder(recorder)
  {
#ifdef _WIN32
    m_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
#endif
  }

  ~PowerKeeper()
  {
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS);
    if (m_timer)
    {
      CancelWaitableTimer(m_timer);
      CloseHandle(m_timer);
    }
#endif
  }

  // Called every few seconds from the main thread, which owns the execution state.
  void Update()
  {
#ifdef _WIN32
    const bool busy = m_recorder.Busy();
    if (busy != m_busy)
    {
      m_busy = busy;
      SetThreadExecutionState(busy ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
    }

    const time_t next = m_recorder.NextRecordingStart();
    if (next != m_wakeAt && m_timer)
    {
      m_wakeAt = next;
      if (next == 0)
      {
        CancelWaitableTimer(m_timer);
        return;
      }
      // FILETIME counts 100 ns steps from 1601, in UTC.
      const long long when = (static_cast<long long>(next) - kWakeEarlySeconds) * 10000000LL +
                             116444736000000000LL;
      LARGE_INTEGER due;
      due.QuadPart = when;
      if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, TRUE))
      {
        const time_t wake = next - kWakeEarlySeconds;
        char text[64];
        std::tm parts{};
        localtime_s(&parts, &wake);
        std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &parts);
        dt::Log(dt::LogLevel::Info, "Wake timer set for %s", text);
      }
      else
      {
        dt::Log(dt::LogLevel::Warning, "Can't set a wake timer (error %lu)", GetLastError());
      }
    }
#endif
  }

private:
  dt::Recorder& m_recorder;
  bool m_busy = false;
  time_t m_wakeAt = -1;
#ifdef _WIN32
  HANDLE m_timer = nullptr;
#endif
};

// Runs until asked to stop. reportRunning tells the service manager once it's up.
int Run(void (*reportRunning)())
{
  dt::CreateDirectories(g_dataFolder);
  dt::SetLogSink(LogToFile);

  dt::RecorderConfig config =
      dt::RecorderConfig::Load(dt::JoinPath(g_dataFolder, "recorder.json"), DefaultRecordingsFolder());
  dt::Recorder recorder(config, g_dataFolder);
  std::string error;
  if (!recorder.Start(error))
  {
    dt::Log(dt::LogLevel::Error, "%s", error.c_str());
    return 1;
  }
  if (reportRunning)
    reportRunning();

  PowerKeeper power(recorder);
  std::unique_lock<std::mutex> lock(g_stopMutex);
  while (!g_stop)
  {
    lock.unlock();
    if (g_resume.exchange(false))
    {
      dt::Log(dt::LogLevel::Info, "The computer woke up");
      recorder.Resume();
    }
    power.Update();
    lock.lock();
    g_stopWake.wait_for(lock, std::chrono::seconds(5));
  }
  lock.unlock();

  dt::Log(dt::LogLevel::Info, "Stopping");
  recorder.Stop();
  dt::Log(dt::LogLevel::Info, "Stopped");
  dt::SetLogSink(nullptr);
  return 0;
}

#ifdef _WIN32

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};

void SetState(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0)
{
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = exitCode;
  g_status.dwWaitHint = waitHint;
  g_status.dwControlsAccepted =
      state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT : 0;
  SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI ServiceControl(DWORD control, DWORD eventType, LPVOID, LPVOID)
{
  switch (control)
  {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      SetState(SERVICE_STOP_PENDING, NO_ERROR, 30000);
      RequestStop();
      return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT:
      if (eventType == PBT_APMRESUMEAUTOMATIC)
        g_resume = true;
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
  g_statusHandle = RegisterServiceCtrlHandlerExW(L"DirectTunerRecorder", ServiceControl, nullptr);
  if (!g_statusHandle)
    return;
  SetState(SERVICE_START_PENDING, NO_ERROR, 30000);
  const int result = Run([] { SetState(SERVICE_RUNNING); });
  SetState(SERVICE_STOPPED, result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

BOOL WINAPI ConsoleControl(DWORD)
{
  RequestStop();
  return TRUE;
}

std::wstring Widen(const std::string& text)
{
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring out(static_cast<size_t>(size > 0 ? size - 1 : 0), L'\0');
  if (size > 1)
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
  return out;
}

// A service running as a user needs the "Log on as a service" right, which sc.exe doesn't grant.
bool GrantServiceLogon(const std::wstring& account)
{
  BYTE sid[SECURITY_MAX_SID_SIZE];
  DWORD sidSize = sizeof(sid);
  wchar_t domain[256];
  DWORD domainSize = 256;
  SID_NAME_USE use;
  if (!LookupAccountNameW(nullptr, account.c_str(), sid, &sidSize, domain, &domainSize, &use))
  {
    std::fprintf(stderr, "Unknown account (error %lu)\n", GetLastError());
    return false;
  }
  LSA_OBJECT_ATTRIBUTES attributes{};
  LSA_HANDLE policy = nullptr;
  if (LsaOpenPolicy(nullptr, &attributes, POLICY_CREATE_ACCOUNT | POLICY_LOOKUP_NAMES, &policy) != 0)
    return false;
  wchar_t right[] = L"SeServiceLogonRight";
  LSA_UNICODE_STRING name{static_cast<USHORT>(wcslen(right) * sizeof(wchar_t)),
                          static_cast<USHORT>(sizeof(right)), right};
  const NTSTATUS status = LsaAddAccountRights(policy, sid, &name, 1);
  LsaClose(policy);
  return status == 0;
}

// Creates or updates the service and starts it. An empty account means Local System.
int Install(const std::string& account, const std::string& password)
{
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::wstring command = L"\"" + std::wstring(exe) + L"\"";
  std::wstring user;
  if (!account.empty())
  {
    user = Widen(account);
    if (user.find(L'\\') == std::wstring::npos && user.find(L'@') == std::wstring::npos)
      user = L".\\" + user;
    if (!GrantServiceLogon(user))
    {
      std::fprintf(stderr, "Couldn't let %s run services\n", account.c_str());
      return 1;
    }
  }
  const std::wstring pass = Widen(password);

  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
  if (!manager)
  {
    std::fprintf(stderr, "Run this as administrator (error %lu)\n", GetLastError());
    return 1;
  }
  SC_HANDLE service = OpenServiceW(manager, L"DirectTunerRecorder", SERVICE_ALL_ACCESS);
  const wchar_t* startName = user.empty() ? nullptr : user.c_str();
  const wchar_t* startPass = user.empty() ? nullptr : pass.c_str();
  if (service)
  {
    if (!ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                              command.c_str(), nullptr, nullptr, nullptr,
                              startName ? startName : L"LocalSystem", startName ? startPass : L"",
                              L"Direct Tuner Recorder"))
    {
      std::fprintf(stderr, "Couldn't update the service (error %lu)\n", GetLastError());
      CloseServiceHandle(service);
      CloseServiceHandle(manager);
      return 1;
    }
  }
  else
  {
    service = CreateServiceW(manager, L"DirectTunerRecorder", L"Direct Tuner Recorder", SERVICE_ALL_ACCESS,
                             SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                             command.c_str(), nullptr, nullptr, nullptr, startName, startPass);
    if (!service)
    {
      std::fprintf(stderr, "Couldn't create the service (error %lu)\n", GetLastError());
      CloseServiceHandle(manager);
      return 1;
    }
  }

  wchar_t text[] = L"Records TV from an HDHomeRun tuner for Kodi's Direct Tuner PVR, even when Kodi is closed.";
  SERVICE_DESCRIPTIONW description{text};
  ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
  SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};
  ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
  // Restart after a crash: a minute later, three times a day at most.
  SC_ACTION actions[3] = {{SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 60000}};
  SERVICE_FAILURE_ACTIONSW failure{86400, nullptr, nullptr, 3, actions};
  ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

  const bool started = StartServiceW(service, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
  if (!started)
    std::fprintf(stderr, "The service was installed but didn't start (error %lu)\n", GetLastError());
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return started ? 0 : 1;
}

// Stops the service and waits, so its files can be replaced. remove deletes it too.
int StopService(bool remove)
{
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
  if (!manager)
    return 1;
  SC_HANDLE service = OpenServiceW(manager, L"DirectTunerRecorder", SERVICE_ALL_ACCESS);
  if (!service)
  {
    CloseServiceHandle(manager);
    return 0;
  }
  SERVICE_STATUS status{};
  ControlService(service, SERVICE_CONTROL_STOP, &status);
  for (int i = 0; i < 60 && QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED; ++i)
    Sleep(500);
  if (remove)
    DeleteService(service);
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return 0;
}

#else

void OnSignal(int)
{
  g_stop = true;
  g_stopWake.notify_all();
}

#endif

void Usage()
{
  std::puts("Direct Tuner Recorder records from an HDHomeRun tuner for Kodi's Direct Tuner PVR.\n"
            "\n"
            "  --console        run in this window instead of as a service (Ctrl+C stops)\n"
            "  --data FOLDER    where settings, timers and the log go\n"
            "  --install [--account NAME --password PASSWORD]\n"
            "                   install and start the Windows service, as Local System or this account\n"
            "  --stop           stop the service\n"
            "  --uninstall      stop and remove the service\n"
            "\n"
            "Settings are in recorder.json in the data folder.");
}

} // namespace

int main(int argc, char** argv)
{
  g_dataFolder = DefaultDataFolder();
  std::string action;
  std::string account;
  std::string password;
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--console")
      g_console = true;
    else if (arg == "--data" && i + 1 < argc)
      g_dataFolder = argv[++i];
    else if (arg == "--install" || arg == "--uninstall" || arg == "--stop")
      action = arg;
    else if (arg == "--account" && i + 1 < argc)
      account = argv[++i];
    else if (arg == "--password" && i + 1 < argc)
      password = argv[++i];
    else
    {
      Usage();
      return arg == "--help" || arg == "-h" ? 0 : 2;
    }
  }

#ifdef _WIN32
  if (action == "--install")
    return Install(account, password);
  if (action == "--stop")
    return StopService(false);
  if (action == "--uninstall")
    return StopService(true);
#endif

#ifdef _WIN32
  if (!g_console)
  {
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(L"DirectTunerRecorder"), ServiceMain}, {nullptr, nullptr}};
    if (StartServiceCtrlDispatcherW(table))
      return 0;
    if (GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
      return 1;
    // Started from a command prompt rather than by Windows: run here.
    g_console = true;
  }
  SetConsoleCtrlHandler(ConsoleControl, TRUE);
  (void)kServiceName;
#else
  g_console = true;
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  (void)kServiceName;
#endif
  return Run(nullptr);
}
