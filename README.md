# Direct Tuner Recorder

A background recording service for **SiliconDust HDHomeRun tuners**, used with the Kodi add-on
[Direct Tuner PVR](https://github.com/ferrellsl/Direct-Tuner-PVR). It records whether or not Kodi is
running, and every Kodi in the house shares its timers, recordings and tuners. You don't need NextPVR or
a SiliconDust DVR subscription.

> **Only for SiliconDust HDHomeRun tuners.** It doesn't work with any other brand of tuner.
>
> **Install it on one computer only.** Pick one computer that's usually on, such as a home server or
> desktop. Every Kodi on your network uses that one recorder; don't install it on the others.

**This is a pre-release for testing.** Please [report problems](#reporting-a-problem).

## What it does

- **Records with Kodi closed.** Timers run on the recorder's computer, and it wakes that computer from
  sleep in time to record.
- **One DVR for the whole house.** Schedule a show on any Kodi and every Kodi sees the timer. Every Kodi
  can play every recording, even while it's still recording.
- **Shares the tuners fairly.** Live TV on every Kodi goes through the recorder, so it knows who's using
  each tuner. Two people watching the same channel share one tuner, and a recording can take a tuner back
  from live TV so a show isn't missed when every tuner is in use.

There's no window or web page to learn. You schedule, watch and delete in Kodi as usual, and the
recorder does the work in the background.

## What you need

- A SiliconDust HDHomeRun tuner on your network.
- One 64-bit Windows 10 or 11 computer to run the recorder, ideally one that's usually on. It can be
  asleep: the recorder wakes it to record.
- [Direct Tuner PVR](https://github.com/ferrellsl/Direct-Tuner-PVR) 21.2.0 or later in each Kodi. It's
  also included in the [AC-4 build of Kodi](https://github.com/ferrellsl/kodi-ac4/releases).
- Enough disk space for recordings. An hour of HD is about 3 to 7 GB.

## Install

On the one computer that will record:

1. Download `DirectTunerRecorder-1.0.0-setup.exe` from the
   [Releases page](https://github.com/ferrellsl/Direct-Tuner-Recorder/releases) and run it.
2. **Recordings folder:** choose where recordings go. The default is
   `C:\Users\Public\Videos\Kodi Recordings`. A local drive is simplest; a network share such as
   `\\nas\TV` also works.
3. **Network folder account:** only asked if you chose a network share. Enter a Windows account that
   can write to the share, usually your own, and its password. Windows stores the password for the
   service; the recorder never sees it again.
4. **Guide:** optionally choose an XMLTV guide file, for example one made by
   [OneGuide](https://github.com/ferrellsl/OneGuide). Leave it empty to use the tuner's own guide,
   which covers about 2 days.

The installer starts the recorder right away and sets it to start with Windows. It also:

- Allows the recorder through Windows Firewall on port 8866, from your local network only.
- Turns on wake timers in the current power plan, so the recorder can wake the computer to record.

## Connect Kodi

Nothing to do in most homes. Each Kodi with Direct Tuner PVR 21.2.0 or later finds the recorder by
itself the next time Kodi starts, and shows a notice that it's using it. Timers that Kodi had move to
the recorder automatically.

If a Kodi doesn't find it, for example because it's on another subnet: in Kodi, go to **Add-ons** >
**My add-ons** > **PVR clients** > **Direct Tuner PVR** > **Configure**, set **Direct Tuner Recorder**
to **Use this address**, and enter the recorder computer's IP address, such as `192.168.1.20`.

To stop a Kodi from using the recorder, set that option to **Don't use one**. That Kodi then records on
its own again, only while it's open.

## Check that it's working

- **The log:** Start menu > **Direct Tuner Recorder** > **Recorder log**. It shows the tuner it found,
  the guide it loaded, and every recording, live TV session and wake timer.
- **In a browser:** open `http://<recorder computer>:8866/api/info`. It shows the recorder's version,
  tuner and recordings folder, and how many recordings and tuners are in use.
- **In Windows:** the service is called **Direct Tuner Recorder** in Services.

## Settings

The installer writes the settings to `C:\ProgramData\Direct Tuner Recorder\recorder.json`. Start menu >
**Direct Tuner Recorder** > **Recorder settings** shows them. To change them, open the file in Notepad
run as administrator, or simply run the installer again, which asks for the folder and guide file. Restart
the service in Services after editing the file.

| Setting | What it does |
|---|---|
| `recordingsFolder` | Where recordings are saved. |
| `guideFile` | An optional XMLTV guide file. It's used for every channel it covers, and the tuner's guide fills in the rest. ATSC 3.0 channels numbered 100 and up also match that number minus 100. |
| `tunerAddress` | The tuner's IP address. Only needed if it isn't found automatically, for example on another subnet. |
| `hideCopyProtected` | Hides DRM channels, which can't play. Default `true`. |
| `port` | The port Kodi connects to. Default `8866`. If you change it, enter `address:port` in each Kodi. |
| `allowNetwork` | `false` lets only Kodi on the same computer use the recorder. Default `true`. |

Timers, the log and the recorded-episode history are kept in the same folder.

## Recordings

Each recording is a `.ts` file with a `.json` file of the same name beside it, in a folder named after
the show. The `.ts` files play in any video player.

Recordings that a Kodi made on its own before you installed the recorder stay where they were. To see
them in Kodi again, move their `.ts` and `.json` files into the recorder's recordings folder.

## Update or uninstall

To update, run the new installer. It keeps your settings, timers and recordings.

To uninstall, use **Apps** in Windows Settings. It removes the service and its firewall rules. Your
recordings, settings and timers stay where they are.

## Known limits

- **SiliconDust HDHomeRun tuners only.** Other tuners aren't supported.
- **One tuner device.** If you have more than one HDHomeRun, set `tunerAddress` to the one to use.
- **One recorder per tuner.** Two recorders using the same tuner would compete for it, like two
  separate DVRs.
- **Windows only for now.** A Linux version is planned.
- **Copy-protected (DRM) channels** can't be recorded or played.
- **AC-4 sound** on ATSC 3.0 channels needs the [AC-4 build of Kodi](https://github.com/ferrellsl/kodi-ac4/releases).
  The recorder saves the sound either way.
- **The wake timer** needs the computer's firmware to support waking from sleep, which most do. It
  can't wake a computer that's shut down.
- **The first time a Kodi connects,** its pending timers move to the recorder, but its list of
  already-recorded episodes doesn't, so a "new episodes only" series may record one repeat.

## Reporting a problem

Please [open an issue](https://github.com/ferrellsl/Direct-Tuner-Recorder/issues) with what you did,
what you expected and what happened, and the relevant part of `recorder.log`. The log doesn't
contain your tuner's DeviceAuth code or any passwords, but check it for anything else you'd rather keep
private, such as folder names, before sharing it.

## Build from source

You need Visual Studio 2022 with C++, CMake, Ninja, and Inno Setup 6 for the installer.

```
powershell tools\build.ps1 -Installer
```

The program is `build\DirectTunerRecorder.exe` and the installer goes in `dist`. To try it without
installing, run `DirectTunerRecorder.exe --console --data <folder>`, which runs it in the window and
keeps its settings and log in that folder.

`src/core` is shared with [Direct Tuner PVR](https://github.com/ferrellsl/Direct-Tuner-PVR) and has no
Kodi code in it; `src/service` is the recorder itself.

## License and credits

GPL-2.0-or-later. See [LICENSE.md](LICENSE.md).

Direct Tuner Recorder is an independent project. It isn't affiliated with or endorsed by SiliconDust,
Team Kodi or NextPVR. HDHomeRun is a trademark of SiliconDust USA Inc., and Kodi is a trademark of the
XBMC Foundation.
