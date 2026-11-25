# Hamster Backup

Daily backup of selected user folders to a NAS, with a small tray app that shows the status.
Uses only Windows built-ins for the backup itself: robocopy, PowerShell and Task Scheduler.

The hamster hoards: **nothing on the NAS is ever deleted automatically.**

## How it works

- **`Backup.ps1`** (the engine) copies only new and changed files. Before a changed file is
  overwritten, the old version moves to `.versionen\<date_time>\...` on the NAS.
- Files you delete or move on the laptop **stay on the NAS**. Once a week the engine lists them;
  you decide in the app what happens (keep forever / move to old versions / delete / ask later).
- **`HamsterBackup.exe`** (the tray app, next to `Backup.ps1`) shows the status, starts backups, pauses them, and
  manages the scheduled task "Hamster Backup". The task checks every 30 minutes (and at logon) whether
  a backup is due: one backup per day at or after `DailyAfter`; a missed or failed backup (laptop off,
  away from home, NAS unreachable) is caught up as soon as possible, at any time of day.

## What ends up on the NAS

```
<share>\
  Documents\  Pictures\  ...         same structure as on the laptop, plain files
  .versionen\<date_time>\...         old versions of changed files
  .logs\                             one log per run
  .behalten.txt                      NAS-only items you decided to keep (never asked again)
```

## Setup

1. Copy `backup-config.example.psd1` to `backup-config.psd1` and fill in the NAS share, your user
   folder and the folders to back up.
2. Open the share once in Explorer and save the NAS password ("Remember my credentials").
   The backup uses that saved login.
3. Start `HamsterBackup.exe` (from the release zip, or build it - see below) and click **Backup now** for the first
   run. A first full backup can take many hours: plug in power, ideally use a cable.
4. The app sets up the automatic backup and starts with Windows (both can be changed under
   **Schedule**; autostart also appears in Task Manager > Startup apps and can be switched there).
   Changes to `backup-config.psd1` are picked up automatically, no restart needed.

## Files

| File | What it does |
|---|---|
| `HamsterBackup.exe` | the tray app (built from `app\`, not in git) |
| `Backup.ps1` | the engine (robocopy, versions, NAS-only report, status for the app) |
| `backup-config.psd1` | your settings (not in git; see the example file) |
| `app\` | source of the tray app (`hamster.cpp`, `hamster.rc`, icons, `build.cmd`) |
| `Backup-now.cmd` / `Dry-run.cmd` / `Review.cmd` | run the engine in a console window |
| `state\` | status, history and NAS-only list written by the engine (not in git) |
| `logs\` | local logs, e.g. when the NAS was unreachable (not in git) |

## Building the app

Needs the free Microsoft C++ Build Tools (workload "Desktop development with C++").
From a "x64 Native Tools Command Prompt":

```
app\build.cmd
```

The exe is written to the base folder (`HamsterBackup.exe`, next to `Backup.ps1`). Exit Hamster from
the tray menu before rebuilding, otherwise the file is in use.

## Notes

- Files that are open in another program during a backup are skipped and retried the next day.
- A Linux NAS cannot store NTFS "alternate data streams" (data hidden inside another file, e.g.
  created when a program saves to a name containing ":"). Such files fail with error 665 in the
  robocopy log; save that data as normal files instead.
- The engine keeps the PC awake while it copies (closing the lid still sends it to sleep).
- To pause backups for a while, use **Pause** in the app; it resumes by itself.
- If the NAS password changes, open the share in Explorer once and save the new password.
