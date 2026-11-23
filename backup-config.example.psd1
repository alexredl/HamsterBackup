@{
    # Copy this file to backup-config.psd1 and adjust it.

    # NAS share the backup goes to. Reserve this IP for the NAS in your router,
    # otherwise it may change after a reboot.
    TargetRoot = '\\192.168.1.50\Backup'

    # Base folder on the laptop
    SourceRoot = 'C:\Users\YourName'

    # Source = folder name on the laptop (inside SourceRoot)
    # Target = folder name on the NAS (directly inside TargetRoot)
    Folders = @(
        @{ Source = 'Documents';  Target = 'Documents' }
        @{ Source = 'Pictures';   Target = 'Pictures' }
        @{ Source = 'Music';      Target = 'Music' }
        @{ Source = 'Videos';     Target = 'Videos' }
    )

    # Never copied (wildcards allowed). desktop.ini only renames folders in Explorer.
    ExcludeFiles = @('desktop.ini', 'Thumbs.db', '~$*', '*.tmp')
    # lost+found: a Linux file-system folder; Linux-based NAS systems refuse to create one inside a share.
    ExcludeDirs  = @('$RECYCLE.BIN', 'lost+found')

    # Automatic run: once per day, not before this time. If the NAS isn't reachable
    # then, it keeps trying (every 30 min) until a backup succeeded that day.
    DailyAfter = '16:00'

    # Copy threads. 1 = live progress in Hamster (also for single huge files).
    # More (e.g. 8) is faster for many small files, but progress then only updates in batches.
    Threads = 1

    # How often (days) the daily run checks for files that exist only on the NAS
    OrphanReportEveryDays = 7

    # Show a warning if there was no successful backup for this many days
    WarnAfterDays = 3

    # Delete log files in .logs older than this many days (backups themselves are never deleted)
    KeepLogsDays = 180
}
