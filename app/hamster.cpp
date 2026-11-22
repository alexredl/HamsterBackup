// Hamster Backup - tray monitor and control panel for the Backup.ps1 engine.
//
// The engine (Backup.ps1, started by Task Scheduler) does the actual backup and reports to
//   state\status.json   - current run / last run / config     (written only by the engine)
//   state\history.csv   - one line per run
//   state\nas-only.json - files that exist only on the NAS   (engine; the app rewrites it only
//                         while holding the engine mutex, after Review actions)
// The app reads those files, shows them, and controls the engine:
//   Backup now  -> powershell Backup.ps1 -Scheduled -Force     (hidden, no window)
//   Stop        -> creates state\stop.request
//   Pause       -> writes state\paused-until.txt ("forever" or a local date/time)
//   Schedule    -> registers / enables / disables / removes the "Hamster Backup" task (COM)
//
// Rules: nothing that touches the NAS (\\server\share) runs on the UI thread except
// user-initiated Review actions. The only code that deletes anything on the NAS is
// "Delete from NAS" in Review, which runs only after an explicit confirmation.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <taskschd.h>
#include <dwmapi.h>
#include <uxtheme.h>
#define SECURITY_WIN32
#include <security.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <cstdarg>
#include <climits>
#include <cmath>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------ constants

static const wchar_t* kAppName       = L"Hamster";
static const wchar_t* kAppLongName   = L"Hamster Backup";
static const wchar_t* kWndClass      = L"HamsterBackup.MainWindow";
static const wchar_t* kAppMutex      = L"Local\\HamsterBackup.App";
static const wchar_t* kEngineMutex   = L"Local\\HamsterBackup.Engine";
static const wchar_t* kTaskName      = L"Hamster Backup";
static const wchar_t* kRegKey        = L"Software\\Hamster Backup";
static const wchar_t* kRunKey        = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue      = L"Hamster Backup";

enum {
    IDI_APP = 100, IDI_GREEN = 101, IDI_BLUE = 102, IDI_YELLOW = 103, IDI_RED = 104, IDI_GREY = 105
};
enum IconState { IS_GREEN = 0, IS_BLUE, IS_YELLOW, IS_RED, IS_GREY, IS_COUNT };
enum NasState { NAS_UNKNOWN = 0, NAS_OK, NAS_DOWN, NAS_NORESP, NAS_LOGIN };
enum TaskState { TS_UNKNOWN = 0, TS_MISSING, TS_ENABLED, TS_DISABLED, TS_ERROR };

#define WM_APP_TRAY   (WM_APP + 1)
#define WM_APP_NAS    (WM_APP + 2)
#define WM_APP_SHOW   (WM_APP + 3)

enum {
    TIMER_REFRESH = 1, TIMER_TASK = 2, TIMER_TRAYRETRY = 3
};

// controls
enum {
    IDC_TAB = 1000,
    // overview
    IDC_STATICON, IDC_HEADLINE, IDC_DETAIL, IDC_SEP1,
    IDC_L_NAS, IDC_NAS1, IDC_NAS2, IDC_NASBAR,
    IDC_L_AUTO, IDC_AUTO, IDC_BTN_PAUSE, IDC_BTN_SCHEDULE,
    IDC_L_NEXT, IDC_NEXT,
    IDC_L_LAST, IDC_LAST,
    IDC_L_NOW, IDC_NOW1, IDC_NOWBAR, IDC_NOWFILE, IDC_NOWHINT, IDC_BTN_STOP,
    IDC_REVIEWHINT, IDC_BTN_REVIEW,
    IDC_BTN_BACKUP, IDC_BTN_OPENNAS, IDC_BTN_OPENLOG, IDC_BTN_CONFIG,
    // review
    IDC_R_HEADER, IDC_R_LIST, IDC_R_KEEP, IDC_R_MOVE, IDC_R_DELETE, IDC_R_LATER, IDC_R_ALL, IDC_R_CHECK, IDC_R_INFO,
    // history
    IDC_H_LIST, IDC_H_OPENLOGS,
};

// menu commands
enum {
    CMD_OPEN = 2000, CMD_BACKUP, CMD_STOP, CMD_PAUSE1, CMD_PAUSE3, CMD_PAUSE7, CMD_PAUSEFOREVER, CMD_RESUME,
    CMD_OPENNAS, CMD_OPENLOG, CMD_EXIT,
    CMD_TASK_ON, CMD_TASK_OFF, CMD_TASK_INSTALL, CMD_TASK_REMOVE, CMD_AUTOSTART
};

// ------------------------------------------------------------------ small helpers

static std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, f, ap);
    va_end(ap);
    return buf;
}

static long long FileTimeToMs(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static long long NowMs() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return FileTimeToMs(ft);
}

static bool MsToLocal(long long ms, SYSTEMTIME& lt) {
    ULARGE_INTEGER u; u.QuadPart = (ULONGLONG)ms * 10000ULL + 116444736000000000ULL;
    FILETIME ft; ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) return false;
    return SystemTimeToTzSpecificLocalTime(NULL, &st, &lt) != 0;
}

static long long LocalToMs(const SYSTEMTIME& lt) {
    SYSTEMTIME st;
    if (!TzSpecificLocalTimeToSystemTime(NULL, &lt, &st)) return 0;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return FileTimeToMs(ft);
}

static long long LocalMidnight(long long ms) {
    SYSTEMTIME lt;
    if (!MsToLocal(ms, lt)) return ms;
    lt.wHour = lt.wMinute = lt.wSecond = lt.wMilliseconds = 0;
    return LocalToMs(lt);
}

static int DayDiff(long long later, long long earlier) {
    double d = (double)(LocalMidnight(later) - LocalMidnight(earlier)) / 86400000.0;
    return (int)std::lround(d);
}

static const wchar_t* kDayNames[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };

static std::wstring When(long long ms) {
    if (ms <= 0) return L"never";
    SYSTEMTIME lt;
    if (!MsToLocal(ms, lt)) return L"?";
    int d = DayDiff(NowMs(), ms);
    if (d == 0) return Fmt(L"today %02d:%02d", lt.wHour, lt.wMinute);
    if (d == 1) return Fmt(L"yesterday %02d:%02d", lt.wHour, lt.wMinute);
    if (d == -1) return Fmt(L"tomorrow %02d:%02d", lt.wHour, lt.wMinute);
    if (d > 1 && d < 7) return Fmt(L"%s %02d.%02d. %02d:%02d", kDayNames[lt.wDayOfWeek], lt.wDay, lt.wMonth, lt.wHour, lt.wMinute);
    return Fmt(L"%02d.%02d.%04d %02d:%02d", lt.wDay, lt.wMonth, lt.wYear, lt.wHour, lt.wMinute);
}

static std::wstring WhenShort(long long ms) {        // for "until ..." texts
    if (ms <= 0) return L"?";
    SYSTEMTIME lt;
    if (!MsToLocal(ms, lt)) return L"?";
    int d = DayDiff(ms, NowMs());
    if (d == 0) return Fmt(L"today %02d:%02d", lt.wHour, lt.wMinute);
    if (d == 1) return Fmt(L"tomorrow %02d:%02d", lt.wHour, lt.wMinute);
    if (d > 1 && d < 7) return Fmt(L"%s %02d:%02d", kDayNames[lt.wDayOfWeek], lt.wHour, lt.wMinute);
    return Fmt(L"%02d.%02d.%04d %02d:%02d", lt.wDay, lt.wMonth, lt.wYear, lt.wHour, lt.wMinute);
}

static std::wstring Size(long long b) {
    if (b < 0) b = 0;
    double v = (double)b;
    if (v >= 1099511627776.0) return Fmt(L"%.2f TB", v / 1099511627776.0);
    if (v >= 1073741824.0)    return Fmt(L"%.1f GB", v / 1073741824.0);
    if (v >= 1048576.0)       return Fmt(L"%.0f MB", v / 1048576.0);
    if (v >= 1024.0)          return Fmt(L"%.0f KB", v / 1024.0);
    return Fmt(L"%lld B", b);
}

static std::wstring Num(long long n) {
    bool neg = n < 0; if (neg) n = -n;
    std::wstring s = std::to_wstring(n), out;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) {
        out.insert(out.begin(), s[i]);
        if (++c % 3 == 0 && i > 0) out.insert(out.begin(), L',');
    }
    return neg ? L"-" + out : out;
}

static std::wstring Duration(long long ms) {
    if (ms < 0) ms = 0;
    long long s = ms / 1000;
    return Fmt(L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
}

static std::wstring Eta(long long seconds) {
    if (seconds < 60) return L"less than a minute left";
    long long m = (seconds + 59) / 60;
    if (m < 60) return Fmt(L"about %lld min left", m);
    long long h = m / 60; m %= 60;
    if (h < 10) return Fmt(L"about %lld h %lld min left", h, (m / 5) * 5);
    return Fmt(L"about %lld h left", h);
}

static bool ParseHHMM(const std::wstring& s, int& h, int& m) {
    h = 16; m = 0;
    int hh = 0, mm = 0;
    if (swscanf_s(s.c_str(), L"%d:%d", &hh, &mm) == 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) { h = hh; m = mm; return true; }
    return false;
}

static long long TodayAt(int h, int m) {
    SYSTEMTIME lt;
    MsToLocal(NowMs(), lt);
    lt.wHour = (WORD)h; lt.wMinute = (WORD)m; lt.wSecond = 0; lt.wMilliseconds = 0;
    return LocalToMs(lt);
}

static std::wstring LongPath(const std::wstring& p) {
    if (p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::wstring Utf8ToWide(const char* data, int len) {
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) { data += 3; len -= 3; }
    if (len <= 0) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, data, len, NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, len, &w[0], n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

// Reads a whole (small) text file. Shares read/write/delete so the engine can replace it meanwhile.
static bool ReadTextFile(const std::wstring& path, std::wstring& out, DWORD maxBytes = 16 * 1024 * 1024) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > maxBytes) { CloseHandle(h); return false; }
    std::string buf((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    BOOL ok = size.QuadPart == 0 || ReadFile(h, &buf[0], (DWORD)size.QuadPart, &read, NULL);
    CloseHandle(h);
    if (!ok) return false;
    out = Utf8ToWide(buf.data(), (int)read);
    return true;
}

static bool WriteTextFileAtomic(const std::wstring& path, const std::wstring& text) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string u = WideToUtf8(text);
    DWORD written = 0;
    BOOL ok = u.empty() || WriteFile(h, u.data(), (DWORD)u.size(), &written, NULL);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    for (int i = 0; i < 5; ++i) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        Sleep(60);
    }
    DeleteFileW(tmp.c_str());
    return false;
}

static bool FileExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
        case L'"': o += L"\\\""; break;
        case L'\\': o += L"\\\\"; break;
        case L'\n': o += L"\\n"; break;
        case L'\r': o += L"\\r"; break;
        case L'\t': o += L"\\t"; break;
        default:
            if (c < 0x20) o += Fmt(L"\\u%04x", (unsigned)c); else o += c;
        }
    }
    return o;
}

static std::wstring XmlEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
        case L'&': o += L"&amp;"; break;
        case L'<': o += L"&lt;"; break;
        case L'>': o += L"&gt;"; break;
        case L'"': o += L"&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

// ------------------------------------------------------------------ minimal JSON reader

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::vector<std::wstring> keys;     // Obj
    std::vector<JVal> vals;             // Obj values / Arr items

    const JVal& operator[](const wchar_t* key) const {
        static const JVal nil;
        if (type != Obj) return nil;
        for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == key) return vals[i];
        return nil;
    }
    long long I(long long def = 0) const { return type == Num ? (long long)std::llround(n) : def; }
    std::wstring S() const { return type == Str ? s : std::wstring(); }
    bool IsNull() const { return type == Null; }
};

struct JParser {
    const wchar_t* p;
    const wchar_t* end;
    void Ws() { while (p < end && (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')) ++p; }
    bool Str(std::wstring& out) {
        if (p >= end || *p != L'"') return false;
        ++p; out.clear();
        while (p < end) {
            wchar_t c = *p++;
            if (c == L'"') return true;
            if (c != L'\\') { out += c; continue; }
            if (p >= end) return false;
            wchar_t e = *p++;
            switch (e) {
            case L'"': out += L'"'; break;
            case L'\\': out += L'\\'; break;
            case L'/': out += L'/'; break;
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'n': out += L'\n'; break;
            case L'r': out += L'\r'; break;
            case L't': out += L'\t'; break;
            case L'u': {
                if (end - p < 4) return false;
                unsigned v = 0;
                for (int i = 0; i < 4; ++i) {
                    wchar_t h = *p++;
                    v <<= 4;
                    if (h >= L'0' && h <= L'9') v |= h - L'0';
                    else if (h >= L'a' && h <= L'f') v |= h - L'a' + 10;
                    else if (h >= L'A' && h <= L'F') v |= h - L'A' + 10;
                    else return false;
                }
                out += (wchar_t)v;       // surrogate pairs arrive as two \u escapes: fine for UTF-16
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Val(JVal& v, int depth) {
        if (depth > 40) return false;
        Ws();
        if (p >= end) return false;
        wchar_t c = *p;
        if (c == L'{') {
            v.type = JVal::Obj; ++p; Ws();
            if (p < end && *p == L'}') { ++p; return true; }
            for (;;) {
                Ws();
                std::wstring k;
                if (!Str(k)) return false;
                Ws();
                if (p >= end || *p != L':') return false;
                ++p;
                JVal x;
                if (!Val(x, depth + 1)) return false;
                v.keys.push_back(k);
                v.vals.push_back(std::move(x));
                Ws();
                if (p < end && *p == L',') { ++p; continue; }
                if (p < end && *p == L'}') { ++p; return true; }
                return false;
            }
        }
        if (c == L'[') {
            v.type = JVal::Arr; ++p; Ws();
            if (p < end && *p == L']') { ++p; return true; }
            for (;;) {
                JVal x;
                if (!Val(x, depth + 1)) return false;
                v.vals.push_back(std::move(x));
                Ws();
                if (p < end && *p == L',') { ++p; continue; }
                if (p < end && *p == L']') { ++p; return true; }
                return false;
            }
        }
        if (c == L'"') { v.type = JVal::Str; return Str(v.s); }
        if (end - p >= 4 && wcsncmp(p, L"true", 4) == 0) { v.type = JVal::Bool; v.b = true; p += 4; return true; }
        if (end - p >= 5 && wcsncmp(p, L"false", 5) == 0) { v.type = JVal::Bool; v.b = false; p += 5; return true; }
        if (end - p >= 4 && wcsncmp(p, L"null", 4) == 0) { v.type = JVal::Null; p += 4; return true; }
        const wchar_t* s = p;
        while (p < end && (iswdigit(*p) || *p == L'-' || *p == L'+' || *p == L'.' || *p == L'e' || *p == L'E')) ++p;
        if (p == s) return false;
        std::wstring num(s, p);
        v.type = JVal::Num;
        v.n = _wtof(num.c_str());
        return true;
    }
};

static bool ParseJson(const std::wstring& text, JVal& out) {
    JParser jp; jp.p = text.c_str(); jp.end = jp.p + text.size();
    JVal v;
    if (!jp.Val(v, 0)) return false;
    out = std::move(v);
    return true;
}

// ------------------------------------------------------------------ registry

static DWORD RegGetDword(const wchar_t* name, DWORD def) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS) return v;
    return def;
}
static void RegSetDword(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &v, sizeof(v));
}
static std::wstring RegGetStr(const wchar_t* key, const wchar_t* name) {
    wchar_t buf[1024]; DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS) return buf;
    return std::wstring();
}
static void RegSetStr(const wchar_t* key, const wchar_t* name, const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

// ------------------------------------------------------------------ global state

struct ReviewItem {
    std::wstring path, kind, localPath;
    long long files = 0, bytes = 0;
};

struct HistRow {
    std::wstring runId, mode, trigger, result, message;
    long long started = 0, ended = 0, files = 0, bytes = 0;
    int exitCode = 0, warnings = 0, errors = 0;
};

struct NasShared {
    CRITICAL_SECTION cs;
    std::wstring root;                  // \\server\share
    int state = NAS_UNKNOWN;
    unsigned long long freeBytes = 0, totalBytes = 0;
    long long since = 0;                // when the current state began
    long long lastDiskMs = 0;
    volatile LONG diskBusy = 0;
    HANDLE wake = NULL;
    volatile LONG quit = 0;
    volatile LONG visible = 0;
} gn;

struct Globals {
    HINSTANCE inst = NULL;
    HWND hwnd = NULL;
    UINT msgTaskbarCreated = 0;
    std::wstring dir, exePath, engine, stateDir, configPath;

    // config (from status.json, fallback: backup-config.psd1)
    std::wstring targetRoot, dailyAfter = L"16:00";
    int warnDays = 3;

    JVal status;
    bool statusOk = false;
    std::vector<ReviewItem> review;     // from nas-only.json
    long long reviewReportUtc = 0;
    std::set<std::wstring> askLater;    // hidden for this session
    std::vector<HistRow> history;

    bool engineRunning = false;
    long long launchPendingUntil = 0;
    int taskState = TS_UNKNOWN;
    long long taskNextRun = 0;
    std::wstring taskError;
    long long pausedUntil = 0;          // 0 = not paused, LLONG_MAX = until resumed

    UINT dpi = 96;
    HFONT fNormal = NULL, fBold = NULL, fHead = NULL;
    HICON trayIcon[IS_COUNT] = {};
    HICON bigIcon[IS_COUNT] = {};
    HICON appSmall = NULL, appBig = NULL;
    HBRUSH bgBrush = NULL;

    NOTIFYICONDATAW nid = {};
    bool trayAdded = false;
    int trayRetries = 0;
    int lastIcon = -1;
    std::wstring lastTip;

    int tab = 0;
    bool visible = false;
    IconState viewIcon = IS_GREY;
    COLORREF headColor = RGB(0, 0, 0);

    bool wasRunning = false;
    std::wstring prevRunTrigger;
    std::wstring reviewInfo;
    std::wstring reviewSig, historySig;     // rebuild the lists only when their data changed (keeps ticks/scroll)

    // theme
    bool dark = false;
    COLORREF cBg = RGB(255, 255, 255), cText = RGB(28, 28, 28), cSub = RGB(96, 96, 96), cLine = RGB(222, 222, 222), cAccent = RGB(0, 103, 192);
    HWND tooltip = NULL;

    // own tab strip (drawn in WM_PAINT)
    RECT tabRect[3] = {};
    std::wstring tabText[3] = { L"Overview", L"Review", L"History" };
} g;

static int S(int v) { return MulDiv(v, (int)g.dpi, 96); }

// ------------------------------------------------------------------ config and state files

static std::wstring PsdValue(const std::wstring& text, const wchar_t* key) {
    // finds  Key = 'value'  (single-quoted) in backup-config.psd1, skipping comment lines
    size_t pos = 0;
    std::wstring k = key;
    while ((pos = text.find(k, pos)) != std::wstring::npos) {
        size_t ls = text.rfind(L'\n', pos);
        ls = (ls == std::wstring::npos) ? 0 : ls + 1;
        std::wstring before = Trim(text.substr(ls, pos - ls));
        size_t eq = pos + k.size();
        while (eq < text.size() && (text[eq] == L' ' || text[eq] == L'\t')) ++eq;
        if (before.empty() && eq < text.size() && text[eq] == L'=') {
            size_t q1 = text.find_first_of(L"'\r\n", eq);
            if (q1 != std::wstring::npos && text[q1] == L'\'') {
                size_t q2 = text.find(L'\'', q1 + 1);
                if (q2 != std::wstring::npos) return text.substr(q1 + 1, q2 - q1 - 1);
            }
            // unquoted number
            size_t a = eq + 1;
            while (a < text.size() && (text[a] == L' ' || text[a] == L'\t')) ++a;
            size_t b = a;
            while (b < text.size() && iswdigit(text[b])) ++b;
            if (b > a) return text.substr(a, b - a);
        }
        pos += k.size();
    }
    return std::wstring();
}

static void LoadConfigFallback() {
    std::wstring t;
    if (!ReadTextFile(g.configPath, t)) return;
    std::wstring v = PsdValue(t, L"TargetRoot");
    if (!v.empty()) g.targetRoot = v;
    v = PsdValue(t, L"DailyAfter");
    if (!v.empty()) g.dailyAfter = v;
    v = PsdValue(t, L"WarnAfterDays");
    if (!v.empty()) g.warnDays = _wtoi(v.c_str());
}

static void ReadStatus() {
    std::wstring t;
    JVal v;
    if (ReadTextFile(g.stateDir + L"\\status.json", t) && ParseJson(t, v) && v.type == JVal::Obj) {
        g.status = std::move(v);
        g.statusOk = true;
    }
    if (g.statusOk) {
        const JVal& c = g.status[L"config"];
        if (!c[L"targetRoot"].S().empty()) g.targetRoot = c[L"targetRoot"].S();
        if (!c[L"dailyAfter"].S().empty()) g.dailyAfter = c[L"dailyAfter"].S();
        if (c[L"warnAfterDays"].I(0) > 0) g.warnDays = (int)c[L"warnAfterDays"].I();
    }
}

static void ReadNasOnly() {
    std::wstring t;
    JVal v;
    g.review.clear();
    g.reviewReportUtc = 0;
    if (!ReadTextFile(g.stateDir + L"\\nas-only.json", t) || !ParseJson(t, v)) return;
    g.reviewReportUtc = v[L"reportUtc"].I();
    const JVal& items = v[L"items"];
    for (const JVal& it : items.vals) {
        ReviewItem r;
        r.path = it[L"path"].S();
        r.kind = it[L"kind"].S();
        r.localPath = it[L"localPath"].S();
        r.files = it[L"files"].I();
        r.bytes = it[L"bytes"].I();
        if (!r.path.empty()) g.review.push_back(r);
    }
}

static int VisibleReviewCount() {
    int n = 0;
    for (const auto& r : g.review) if (!g.askLater.count(r.path)) ++n;
    return n;
}

static void ReadHistory() {
    std::wstring t;
    g.history.clear();
    if (!ReadTextFile(g.stateDir + L"\\history.csv", t)) return;
    size_t pos = 0;
    bool header = true;
    while (pos < t.size()) {
        size_t nl = t.find(L'\n', pos);
        std::wstring line = t.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = (nl == std::wstring::npos) ? t.size() : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        if (header) { header = false; continue; }
        std::vector<std::wstring> f;
        size_t a = 0;
        for (;;) {
            size_t b = line.find(L';', a);
            f.push_back(line.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
            if (b == std::wstring::npos) break;
            a = b + 1;
        }
        if (f.size() < 13) continue;        // partial line
        HistRow h;
        h.runId = f[0]; h.started = _wtoi64(f[1].c_str()); h.ended = _wtoi64(f[2].c_str());
        h.mode = f[3]; h.trigger = f[4]; h.result = f[5]; h.exitCode = _wtoi(f[6].c_str());
        h.files = _wtoi64(f[7].c_str()); h.bytes = _wtoi64(f[8].c_str());
        h.warnings = _wtoi(f[10].c_str()); h.errors = _wtoi(f[11].c_str());
        h.message = f[12];
        g.history.push_back(h);
    }
    std::reverse(g.history.begin(), g.history.end());   // newest first
    if (g.history.size() > 200) g.history.resize(200);
}

static void ReadPause() {
    std::wstring t;
    g.pausedUntil = 0;
    if (!ReadTextFile(g.stateDir + L"\\paused-until.txt", t)) return;
    t = Trim(t);
    if (_wcsicmp(t.c_str(), L"forever") == 0) { g.pausedUntil = LLONG_MAX; return; }
    SYSTEMTIME lt = {};
    int y, mo, d, h, mi, s = 0;
    if (swscanf_s(t.c_str(), L"%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) >= 5) {
        lt.wYear = (WORD)y; lt.wMonth = (WORD)mo; lt.wDay = (WORD)d; lt.wHour = (WORD)h; lt.wMinute = (WORD)mi; lt.wSecond = (WORD)s;
        long long ms = LocalToMs(lt);
        if (ms > NowMs()) g.pausedUntil = ms;
        else DeleteFileW((g.stateDir + L"\\paused-until.txt").c_str());   // pause is over
    }
}

static bool IsEngineRunning() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kEngineMutex);
    if (!h) return false;           // the engine creates it for the whole run
    CloseHandle(h);                 // never keep it open (would make a killed run's mutex "abandoned")
    return true;
}

// ------------------------------------------------------------------ NAS probe (worker thread)

static std::wstring HostOf(const std::wstring& root) {
    if (root.size() < 3 || root.compare(0, 2, L"\\\\") != 0) return std::wstring();
    size_t e = root.find(L'\\', 2);
    return root.substr(2, e == std::wstring::npos ? std::wstring::npos : e - 2);
}

static bool TcpProbe(const std::wstring& host, int timeoutMs) {
    ADDRINFOW hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = NULL;
    if (GetAddrInfoW(host.c_str(), L"445", &hints, &res) != 0) return false;
    bool ok = false;
    for (ADDRINFOW* a = res; a && !ok; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int r = connect(s, a->ai_addr, (int)a->ai_addrlen);
        if (r == 0) ok = true;
        else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wr, ex;
            FD_ZERO(&wr); FD_ZERO(&ex);
            FD_SET(s, &wr); FD_SET(s, &ex);
            timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
            if (select(0, NULL, &wr, &ex, &tv) > 0 && FD_ISSET(s, &wr)) {
                int err = 0, len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
                ok = (err == 0);
            }
        }
        closesocket(s);
    }
    FreeAddrInfoW(res);
    return ok;
}

struct DiskJob {
    std::wstring root;
    HANDLE done;
    volatile LONG refs;
    BOOL ok;
    DWORD err;
    unsigned long long freeBytes, totalBytes;
};

static void ReleaseJob(DiskJob* j) {
    if (InterlockedDecrement(&j->refs) == 0) { CloseHandle(j->done); delete j; }
}

static DWORD WINAPI DiskProc(LPVOID p) {
    DiskJob* j = (DiskJob*)p;
    ULARGE_INTEGER avail, total, freeAll;
    std::wstring r = j->root;
    if (!r.empty() && r.back() != L'\\') r += L'\\';
    j->ok = GetDiskFreeSpaceExW(r.c_str(), &avail, &total, &freeAll);
    j->err = j->ok ? 0 : GetLastError();
    if (j->ok) { j->freeBytes = avail.QuadPart; j->totalBytes = total.QuadPart; }
    SetEvent(j->done);
    InterlockedExchange(&gn.diskBusy, 0);
    ReleaseJob(j);
    return 0;
}

static DWORD WINAPI NasProc(LPVOID) {
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    while (!gn.quit) {
        std::wstring root;
        int prev;
        EnterCriticalSection(&gn.cs);
        root = gn.root; prev = gn.state;
        LeaveCriticalSection(&gn.cs);

        int st = NAS_UNKNOWN;
        unsigned long long fb = 0, tb = 0;
        bool haveSizes = false;
        std::wstring host = HostOf(root);
        if (!host.empty()) {
            if (!TcpProbe(host, 1500)) st = NAS_DOWN;
            else {
                long long now = NowMs();
                bool diskDue = gn.visible || prev != NAS_OK || now - gn.lastDiskMs > 10 * 60 * 1000;
                if (diskDue && InterlockedCompareExchange(&gn.diskBusy, 1, 0) == 0) {
                    DiskJob* j = new DiskJob();
                    j->root = root; j->done = CreateEventW(NULL, TRUE, FALSE, NULL); j->refs = 2;
                    j->ok = FALSE; j->err = 0; j->freeBytes = j->totalBytes = 0;
                    HANDLE th = CreateThread(NULL, 0, DiskProc, j, 0, NULL);
                    if (!th) { InterlockedExchange(&gn.diskBusy, 0); ReleaseJob(j); ReleaseJob(j); st = NAS_OK; }
                    else {
                        CloseHandle(th);
                        if (WaitForSingleObject(j->done, 5000) == WAIT_TIMEOUT) st = NAS_NORESP;
                        else if (j->ok) { st = NAS_OK; fb = j->freeBytes; tb = j->totalBytes; haveSizes = true; }
                        else if (j->err == ERROR_LOGON_FAILURE || j->err == ERROR_ACCESS_DENIED ||
                                 j->err == ERROR_INVALID_PASSWORD || j->err == ERROR_CANCELLED || j->err == 1244 /*NOT_AUTHENTICATED*/) st = NAS_LOGIN;
                        else st = NAS_DOWN;
                        gn.lastDiskMs = now;
                        ReleaseJob(j);
                    }
                }
                else if (gn.diskBusy) st = NAS_NORESP;
                else st = (prev == NAS_LOGIN) ? NAS_LOGIN : NAS_OK;
            }
        }
        EnterCriticalSection(&gn.cs);
        if (st != gn.state) { gn.state = st; gn.since = NowMs(); }
        if (haveSizes) { gn.freeBytes = fb; gn.totalBytes = tb; }
        LeaveCriticalSection(&gn.cs);
        if (g.hwnd) PostMessageW(g.hwnd, WM_APP_NAS, 0, 0);
        WaitForSingleObject(gn.wake, 30000);
    }
    WSACleanup();
    return 0;
}

static void WakeNas() { if (gn.wake) SetEvent(gn.wake); }

// ------------------------------------------------------------------ Task Scheduler (COM)

struct Bstr {
    BSTR b;
    explicit Bstr(const wchar_t* s) : b(SysAllocString(s)) {}
    explicit Bstr(const std::wstring& s) : b(SysAllocString(s.c_str())) {}
    ~Bstr() { SysFreeString(b); }
    operator BSTR() const { return b; }
};

static HRESULT GetRootFolder(ComPtr<ITaskFolder>& folder) {
    ComPtr<ITaskService> svc;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc));
    if (FAILED(hr)) return hr;
    VARIANT e; VariantInit(&e);
    hr = svc->Connect(e, e, e, e);
    if (FAILED(hr)) return hr;
    return svc->GetFolder(Bstr(L"\\"), &folder);
}

static int QueryTask(const wchar_t* name, long long* nextRun) {
    ComPtr<ITaskFolder> folder;
    if (FAILED(GetRootFolder(folder))) return TS_ERROR;
    ComPtr<IRegisteredTask> task;
    HRESULT hr = folder->GetTask(Bstr(name), &task);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) return TS_MISSING;
    if (FAILED(hr)) return TS_ERROR;
    VARIANT_BOOL en = VARIANT_FALSE;
    task->get_Enabled(&en);
    if (nextRun) {
        *nextRun = 0;
        DATE d = 0;
        if (SUCCEEDED(task->get_NextRunTime(&d)) && d > 1.0) {
            SYSTEMTIME lt;
            if (VariantTimeToSystemTime(d, &lt)) *nextRun = LocalToMs(lt);
        }
    }
    return en ? TS_ENABLED : TS_DISABLED;
}

static std::wstring CurrentUser() {
    wchar_t buf[512]; ULONG n = _countof(buf);
    if (GetUserNameExW(NameSamCompatible, buf, &n)) return buf;
    wchar_t user[256] = L"", dom[256] = L"";
    GetEnvironmentVariableW(L"USERNAME", user, 256);
    GetEnvironmentVariableW(L"USERDOMAIN", dom, 256);
    return std::wstring(dom) + L"\\" + user;
}

static std::wstring TaskXml(bool enabled) {
    int h, m;
    ParseHHMM(g.dailyAfter, h, m);
    int minutesLeft = 24 * 60 - (h * 60 + m);
    if (minutesLeft < 30) minutesLeft = 30;
    SYSTEMTIME lt; MsToLocal(NowMs(), lt);
    std::wstring start = Fmt(L"%04d-%02d-%02dT%02d:%02d:00", lt.wYear, lt.wMonth, lt.wDay, h, m);
    std::wstring dur = Fmt(L"PT%dH%dM", minutesLeft / 60, minutesLeft % 60);
    std::wstring user = XmlEscape(CurrentUser());
    std::wstring engineDir = XmlEscape(g.dir);
    std::wstring engine = XmlEscape(g.engine);
    std::wstring x;
    x += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n";
    x += L"<Task version=\"1.4\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n";
    x += L"  <RegistrationInfo><Description>Hamster Backup: daily backup to the NAS, once per day after ";
    x += XmlEscape(g.dailyAfter) + L" as soon as the NAS is reachable. Managed by the Hamster app.</Description></RegistrationInfo>\n";
    x += L"  <Triggers>\n";
    x += L"    <CalendarTrigger><StartBoundary>" + start + L"</StartBoundary>";
    x += L"<Repetition><Interval>PT30M</Interval><Duration>" + dur + L"</Duration><StopAtDurationEnd>false</StopAtDurationEnd></Repetition>";
    x += L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay></CalendarTrigger>\n";
    x += L"    <LogonTrigger><UserId>" + user + L"</UserId><Delay>PT2M</Delay></LogonTrigger>\n";
    x += L"  </Triggers>\n";
    x += L"  <Principals><Principal id=\"Author\"><UserId>" + user + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\n";
    x += L"  <Settings>\n";
    x += L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n";
    x += L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n";
    x += L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n";
    x += L"    <StartWhenAvailable>true</StartWhenAvailable>\n";
    x += L"    <RunOnlyIfNetworkAvailable>true</RunOnlyIfNetworkAvailable>\n";
    x += L"    <ExecutionTimeLimit>PT72H</ExecutionTimeLimit>\n";
    x += std::wstring(L"    <Enabled>") + (enabled ? L"true" : L"false") + L"</Enabled>\n";
    x += L"  </Settings>\n";
    x += L"  <Actions Context=\"Author\"><Exec>\n";
    x += L"    <Command>%SystemRoot%\\System32\\conhost.exe</Command>\n";
    x += L"    <Arguments>--headless %SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"";
    x += engine + L"\" -Scheduled</Arguments>\n";
    x += L"    <WorkingDirectory>" + engineDir + L"</WorkingDirectory>\n";
    x += L"  </Exec></Actions>\n";
    x += L"</Task>\n";
    return x;
}

static HRESULT RegisterTask(bool enabled) {
    ComPtr<ITaskFolder> folder;
    HRESULT hr = GetRootFolder(folder);
    if (FAILED(hr)) return hr;
    VARIANT e; VariantInit(&e);
    ComPtr<IRegisteredTask> reg;
    return folder->RegisterTask(Bstr(kTaskName), Bstr(TaskXml(enabled)), TASK_CREATE_OR_UPDATE, e, e,
                                TASK_LOGON_INTERACTIVE_TOKEN, e, &reg);
}

static HRESULT SetTaskEnabled(bool on) {
    ComPtr<ITaskFolder> folder;
    HRESULT hr = GetRootFolder(folder);
    if (FAILED(hr)) return hr;
    ComPtr<IRegisteredTask> task;
    hr = folder->GetTask(Bstr(kTaskName), &task);
    if (FAILED(hr)) return hr;
    return task->put_Enabled(on ? VARIANT_TRUE : VARIANT_FALSE);
}

static HRESULT DeleteTask(const wchar_t* name) {
    ComPtr<ITaskFolder> folder;
    HRESULT hr = GetRootFolder(folder);
    if (FAILED(hr)) return hr;
    return folder->DeleteTask(Bstr(name), 0);
}

static std::wstring HrText(HRESULT hr) {
    wchar_t* msg = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, (DWORD)hr, 0, (LPWSTR)&msg, 0, NULL);
    std::wstring s = msg ? Trim(msg) : Fmt(L"error 0x%08X", (unsigned)hr);
    if (msg) LocalFree(msg);
    return s;
}

// On start: keep the task definition current (e.g. after the folder was moved or DailyAfter changed).
// Keeps the on/off state; does nothing if the task is not set up.
static void UpdateTaskDefinition() {
    int cur = QueryTask(kTaskName, NULL);
    if (cur == TS_ENABLED || cur == TS_DISABLED) RegisterTask(cur == TS_ENABLED);
}

static void RefreshTask() {
    long long next = 0;
    g.taskState = QueryTask(kTaskName, &next);
    g.taskNextRun = next;
}

// ------------------------------------------------------------------ autostart

static bool AutostartWanted() { return RegGetDword(L"Autostart", 1) != 0; }

static void ApplyAutostart() {
    std::wstring want = L"\"" + g.exePath + L"\" /tray";
    if (AutostartWanted()) {
        if (RegGetStr(kRunKey, kRunValue) != want) RegSetStr(kRunKey, kRunValue, want);
    }
    else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
}

// ------------------------------------------------------------------ engine control

static bool LaunchEngine(const std::wstring& args) {
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    std::wstring ps = std::wstring(sysdir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring cmd = L"\"" + ps + L"\" -NoProfile -ExecutionPolicy Bypass -File \"" + g.engine + L"\" " + args;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    DWORD flags = CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
    BOOL ok = CreateProcessW(ps.c_str(), buf.data(), NULL, NULL, FALSE, flags, NULL, g.dir.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        ok = CreateProcessW(ps.c_str(), buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g.dir.c_str(), &si, &pi);
    }
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void SetPause(long long untilMs) {   // 0 = resume, LLONG_MAX = until resumed
    std::wstring p = g.stateDir + L"\\paused-until.txt";
    CreateDirectoryW(g.stateDir.c_str(), NULL);
    if (untilMs == 0) DeleteFileW(p.c_str());
    else if (untilMs == LLONG_MAX) WriteTextFileAtomic(p, L"forever");
    else {
        SYSTEMTIME lt; MsToLocal(untilMs, lt);
        WriteTextFileAtomic(p, Fmt(L"%04d-%02d-%02dT%02d:%02d:%02d", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond));
    }
}

// ------------------------------------------------------------------ status logic (one place)

struct View {
    IconState icon = IS_GREY;
    std::wstring headline, detail, tip;
};

static int NasStateNow(unsigned long long* fb = NULL, unsigned long long* tb = NULL, long long* since = NULL) {
    EnterCriticalSection(&gn.cs);
    int s = gn.state;
    if (fb) *fb = gn.freeBytes;
    if (tb) *tb = gn.totalBytes;
    if (since) *since = gn.since;
    LeaveCriticalSection(&gn.cs);
    return s;
}

static std::wstring NasReason() {
    long long since = 0;
    int s = NasStateNow(NULL, NULL, &since);
    switch (s) {
    case NAS_DOWN:   return since ? L"NAS not reachable since " + When(since) : L"NAS not reachable";
    case NAS_NORESP: return L"NAS is not responding";
    case NAS_LOGIN:  return L"NAS login missing in Windows";
    default: return std::wstring();
    }
}

static bool IsRunningNow() { return g.engineRunning; }

static View ComputeView() {
    View v;
    long long now = NowMs();
    const JVal& cur = g.status[L"current"];
    const JVal& last = g.status[L"lastRun"];
    long long lastSuccess = g.status[L"lastSuccessUtc"].I();
    int h, m;
    ParseHHMM(g.dailyAfter, h, m);
    long long dueAt = TodayAt(h, m);
    bool doneToday = lastSuccess > 0 && DayDiff(now, lastSuccess) == 0;
    std::wstring nasWhy = NasReason();

    // 1. running
    if (IsRunningNow()) {
        v.icon = IS_BLUE;
        if (cur.IsNull() || cur[L"state"].S() != L"running") {
            v.headline = L"Starting backup...";
        }
        else {
            std::wstring mode = cur[L"mode"].S();
            std::wstring phase = cur[L"phase"].S();
            std::wstring folder = cur[L"folder"].S();
            long long bp = cur[L"bytesPlanned"].I(), bd = cur[L"bytesDone"].I();
            int pct = bp > 0 ? (int)std::min<long long>(100, bd * 100 / bp) : 0;
            if (mode == L"report") v.headline = L"Checking for files no longer on this PC...";
            else if (phase == L"scanning") v.headline = L"Backing up - looking for changes in " + folder + L"...";
            else if (phase == L"orphan_report") v.headline = L"Backing up - checking for files no longer on this PC...";
            else v.headline = Fmt(L"Backing up - %s %d %%", folder.c_str(), pct);
            if (mode == L"dryrun") v.headline = L"Dry run: " + v.headline;
            long long hb = cur[L"heartbeatUtc"].I();
            if (hb > 0 && now - hb > 3 * 60 * 1000) v.detail = L"No progress since " + When(hb) + L" (the laptop may have been asleep).";
            else v.detail = L"Started " + When(cur[L"startedUtc"].I()) + L". You can keep working; progress is not lost if the laptop sleeps.";
        }
        v.tip = v.headline;
        return v;
    }
    if (g.launchPendingUntil > now) {
        v.icon = IS_BLUE;
        v.headline = L"Starting backup...";
        v.tip = v.headline;
        return v;
    }

    // 2. paused
    if (g.pausedUntil) {
        v.icon = IS_GREY;
        v.headline = g.pausedUntil == LLONG_MAX ? L"Paused until you resume" : L"Paused until " + WhenShort(g.pausedUntil);
        v.detail = L"Last backup: " + When(lastSuccess) + L". \"Backup now\" still works.";
        v.tip = v.headline;
        return v;
    }

    // 3. automatic backup off / missing
    if (g.taskState == TS_MISSING || g.taskState == TS_DISABLED) {
        v.icon = IS_RED;
        v.headline = g.taskState == TS_MISSING ? L"Automatic backup is not set up" : L"Automatic backup is off";
        v.detail = L"Last backup: " + When(lastSuccess) + L". Turn it on in the \"Automatic\" row below.";
        v.tip = v.headline;
        return v;
    }

    // 4. last run failed (and nothing succeeded after it)
    std::wstring lr = last[L"result"].S();
    long long lrEnd = last[L"endedUtc"].I();
    bool lastFailed = (lr == L"errors" || lr == L"failed") && lrEnd > lastSuccess;
    bool lastInterrupted = lr == L"interrupted" && lrEnd > lastSuccess;
    if (lastFailed) {
        v.icon = IS_RED;
        std::wstring msg = last[L"message"].S();
        v.headline = L"Last backup failed" + (msg.empty() ? std::wstring() : L" - " + msg);
        v.detail = L"Last successful backup: " + When(lastSuccess) + L". It retries automatically every 30 minutes.";
        v.tip = L"Last backup failed";
        return v;
    }

    // 5. too long without a backup
    int ageDays = lastSuccess > 0 ? DayDiff(now, lastSuccess) : 9999;
    if (lastSuccess > 0 && ageDays >= g.warnDays) {
        v.icon = IS_RED;
        v.headline = Fmt(L"No backup for %d days", ageDays);
        v.detail = (nasWhy.empty() ? std::wstring(L"") : nasWhy + L". ") + L"Last backup: " + When(lastSuccess) + L".";
        v.tip = v.headline + (nasWhy.empty() ? L"" : L" - " + nasWhy);
        return v;
    }

    // 6. interrupted: will retry
    if (lastInterrupted) {
        v.icon = IS_YELLOW;
        v.headline = L"Last backup was interrupted - it will retry automatically";
        v.detail = last[L"message"].S() + L". Last successful backup: " + When(lastSuccess) + L".";
        v.tip = L"Last backup was interrupted";
        return v;
    }

    // 7. OK (less than ~30 h old)
    if (lastSuccess > 0 && now - lastSuccess < 30LL * 3600 * 1000) {
        v.icon = IS_GREEN;
        v.headline = L"Backup OK - " + When(lastSuccess);
        if (lr == L"warnings") v.detail = last[L"message"].S() + L".";
        else v.detail = L"Everything was copied.";
        unsigned long long fb = 0, tb = 0;
        if (NasStateNow(&fb, &tb) == NAS_OK && tb > 0 && fb * 20 < tb) {
            v.icon = IS_YELLOW;
            v.detail = L"The NAS is almost full: only " + Size((long long)fb) + L" free.";
        }
        v.tip = v.headline;
        return v;
    }

    // 8. due but not done
    if (lastSuccess == 0) {
        v.icon = IS_GREY;
        v.headline = L"No backup yet";
        v.detail = L"Click \"Backup now\" to start the first one.";
        v.tip = v.headline;
        return v;
    }
    v.icon = IS_YELLOW;
    if (now >= dueAt && !doneToday) {
        v.headline = nasWhy.empty() ? std::wstring(L"Backup due - starts within 30 minutes") : L"Backup due - " + nasWhy;
    }
    else {
        v.headline = L"Last backup " + When(lastSuccess) + L" - next one today after " + g.dailyAfter;
    }
    v.detail = L"Last backup: " + When(lastSuccess) + L".";
    v.tip = v.headline;
    return v;
}

static COLORREF StateColor(IconState s) {
    if (g.dark) {
        switch (s) {
        case IS_GREEN:  return RGB(0x6C, 0xCB, 0x5F);
        case IS_BLUE:   return RGB(0x60, 0xAA, 0xFF);
        case IS_YELLOW: return RGB(0xF0, 0xC8, 0x50);
        case IS_RED:    return RGB(0xFF, 0x6E, 0x64);
        default:        return RGB(0xAA, 0xAA, 0xAA);
        }
    }
    switch (s) {
    case IS_GREEN:  return RGB(0x1E, 0x7A, 0x34);
    case IS_BLUE:   return RGB(0x1A, 0x5F, 0xB4);
    case IS_YELLOW: return RGB(0x8A, 0x63, 0x00);
    case IS_RED:    return RGB(0xB3, 0x26, 0x1E);
    default:        return RGB(0x55, 0x55, 0x55);
    }
}

// ------------------------------------------------------------------ theme (light / dark, follows Windows)

static bool WindowsUsesDarkApps() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz);
    return v == 0;
}

// Undocumented but stable uxtheme exports (Windows 10 1903+) that make menus follow dark mode.
static void SetMenuTheme() {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int (WINAPI* SetPreferredAppModeFn)(int);
    typedef void (WINAPI* FlushMenuThemesFn)();
    SetPreferredAppModeFn setMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemesFn flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(1 /*AllowDark: menus follow the Windows setting*/);
    if (flush) flush();
}

static HWND Item(int id);

static BOOL CALLBACK ThemeChildProc(HWND h, LPARAM) {
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    const wchar_t* explorer = g.dark ? L"DarkMode_Explorer" : L"Explorer";
    if (_wcsicmp(cls, L"Button") == 0) SetWindowTheme(h, explorer, NULL);
    else if (_wcsicmp(cls, WC_LISTVIEWW) == 0) {
        SetWindowTheme(h, explorer, NULL);
        HWND hdr = ListView_GetHeader(h);
        if (hdr) SetWindowTheme(hdr, g.dark ? L"DarkMode_ItemsView" : L"ItemsView", NULL);
        ListView_SetBkColor(h, g.cBg);
        ListView_SetTextBkColor(h, g.cBg);
        ListView_SetTextColor(h, g.cText);
    }
    else if (_wcsicmp(cls, TOOLTIPS_CLASSW) == 0) SetWindowTheme(h, explorer, NULL);
    return TRUE;
}

static void ApplyTheme() {
    g.dark = WindowsUsesDarkApps();         // always follows the Windows app mode (light / dark)
    if (g.dark) {
        g.cBg = RGB(32, 32, 32); g.cText = RGB(236, 236, 236); g.cSub = RGB(170, 170, 170);
        g.cLine = RGB(64, 64, 64); g.cAccent = RGB(76, 160, 255);
    }
    else {
        g.cBg = RGB(255, 255, 255); g.cText = RGB(28, 28, 28); g.cSub = RGB(96, 96, 96);
        g.cLine = RGB(222, 222, 222); g.cAccent = RGB(0, 103, 192);
    }
    if (g.bgBrush) DeleteObject(g.bgBrush);
    g.bgBrush = CreateSolidBrush(g.cBg);
    SetMenuTheme();
    if (!g.hwnd) return;
    BOOL darkTitle = g.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkTitle, sizeof(darkTitle));
    EnumChildWindows(g.hwnd, ThemeChildProc, 0);
    if (g.tooltip) ThemeChildProc(g.tooltip, 0);
    // NAS bar: unthemed = static (no animation), with our own colours
    HWND bar = Item(IDC_NASBAR);
    if (bar) {
        SetWindowTheme(bar, L"", L"");
        SendMessageW(bar, PBM_SETBKCOLOR, 0, (LPARAM)g.cLine);
        SendMessageW(bar, PBM_SETBARCOLOR, 0, (LPARAM)g.cAccent);
    }
    RedrawWindow(g.hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

// ------------------------------------------------------------------ tray icon

static void LoadIcons() {
    int ids[IS_COUNT] = { IDI_GREEN, IDI_BLUE, IDI_YELLOW, IDI_RED, IDI_GREY };
    for (int i = 0; i < IS_COUNT; ++i) {
        if (g.trayIcon[i]) DestroyIcon(g.trayIcon[i]);
        if (g.bigIcon[i]) DestroyIcon(g.bigIcon[i]);
        g.trayIcon[i] = NULL; g.bigIcon[i] = NULL;
        LoadIconMetric(g.inst, MAKEINTRESOURCEW(ids[i]), LIM_SMALL, &g.trayIcon[i]);
        g.bigIcon[i] = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(ids[i]), IMAGE_ICON, S(32), S(32), LR_DEFAULTCOLOR);
    }
    if (g.appSmall) DestroyIcon(g.appSmall);
    if (g.appBig) DestroyIcon(g.appBig);
    g.appSmall = NULL; g.appBig = NULL;
    LoadIconMetric(g.inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &g.appSmall);
    LoadIconMetric(g.inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &g.appBig);
}

static void TrayAdd() {
    g.nid = {};
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g.nid.uCallbackMessage = WM_APP_TRAY;
    int ic = g.lastIcon >= 0 ? g.lastIcon : IS_GREY;
    g.nid.hIcon = g.trayIcon[ic];
    wcsncpy_s(g.nid.szTip, g.lastTip.empty() ? kAppLongName : g.lastTip.c_str(), _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &g.nid) || Shell_NotifyIconW(NIM_MODIFY, &g.nid)) {
        g.nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g.nid);
        g.trayAdded = true;
        KillTimer(g.hwnd, TIMER_TRAYRETRY);
    }
    else {
        g.trayAdded = false;
        if (g.trayRetries++ < 60) SetTimer(g.hwnd, TIMER_TRAYRETRY, 2000, NULL);
    }
}

static void TrayUpdate(IconState icon, const std::wstring& tip) {
    std::wstring t = std::wstring(kAppName) + L" - " + tip;
    if (t.size() > 127) t = t.substr(0, 124) + L"...";
    if ((int)icon == g.lastIcon && t == g.lastTip) return;
    g.lastIcon = icon;
    g.lastTip = t;
    if (!g.trayAdded) return;
    g.nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g.nid.hIcon = g.trayIcon[icon];
    wcsncpy_s(g.nid.szTip, t.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

static void Balloon(const std::wstring& title, const std::wstring& text) {
    if (!g.trayAdded) return;
    NOTIFYICONDATAW n = {};
    n.cbSize = sizeof(n);
    n.hWnd = g.hwnd;
    n.uID = 1;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;
    n.hBalloonIcon = g.appBig;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// Shows a notification at most once per key (e.g. once per day, once per failed run).
static void NotifyOnce(const wchar_t* kind, const std::wstring& key, const std::wstring& title, const std::wstring& text) {
    std::wstring regKey = std::wstring(kRegKey) + L"\\Notified";
    if (RegGetStr(regKey.c_str(), kind) == key) return;
    RegSetStr(regKey.c_str(), kind, key);
    Balloon(title, text);
}

static std::wstring TodayKey() {
    SYSTEMTIME lt; MsToLocal(NowMs(), lt);
    return Fmt(L"%04d-%02d-%02d", lt.wYear, lt.wMonth, lt.wDay);
}

// ------------------------------------------------------------------ window: controls

static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, g.hwnd, (HMENU)(INT_PTR)id, g.inst, NULL);
}
static HWND Item(int id) { return GetDlgItem(g.hwnd, id); }

static void SetText(int id, const std::wstring& t) {
    HWND h = Item(id);
    if (!h) return;
    int len = GetWindowTextLengthW(h);
    std::wstring cur(len + 1, L'\0');
    GetWindowTextW(h, &cur[0], len + 1);
    cur.resize(len);
    if (cur != t) SetWindowTextW(h, t.c_str());
}

static void Show(int id, bool on) {
    HWND h = Item(id);
    // check the control's own WS_VISIBLE style (IsWindowVisible is false while the main window is hidden)
    if (h && ((GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0) != on) ShowWindow(h, on ? SW_SHOWNA : SW_HIDE);
}

static const int kOverview[] = {
    IDC_STATICON, IDC_HEADLINE, IDC_DETAIL, IDC_L_NAS, IDC_NAS1, IDC_NAS2, IDC_NASBAR,
    IDC_L_AUTO, IDC_AUTO, IDC_BTN_PAUSE, IDC_BTN_SCHEDULE, IDC_L_NEXT, IDC_NEXT, IDC_L_LAST, IDC_LAST,
    IDC_L_NOW, IDC_NOW1, IDC_NOWBAR, IDC_NOWFILE, IDC_NOWHINT, IDC_BTN_STOP, IDC_REVIEWHINT, IDC_BTN_REVIEW,
    IDC_BTN_BACKUP, IDC_BTN_OPENNAS, IDC_BTN_OPENLOG, IDC_BTN_CONFIG };
static const int kReview[] = { IDC_R_HEADER, IDC_R_LIST, IDC_R_KEEP, IDC_R_MOVE, IDC_R_DELETE, IDC_R_LATER, IDC_R_ALL, IDC_R_CHECK, IDC_R_INFO };
static const int kHistory[] = { IDC_H_LIST, IDC_H_OPENLOGS };

static void CreateFonts() {
    if (g.fNormal) DeleteObject(g.fNormal);
    if (g.fBold) DeleteObject(g.fBold);
    if (g.fHead) DeleteObject(g.fHead);
    g.fNormal = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fBold   = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fHead   = CreateFontW(-MulDiv(13, g.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static BOOL CALLBACK SetFontProc(HWND h, LPARAM) {
    int id = GetDlgCtrlID(h);
    HFONT f = g.fNormal;
    if (id == IDC_HEADLINE) f = g.fHead;
    else if (id == IDC_L_NAS || id == IDC_L_AUTO || id == IDC_L_NEXT || id == IDC_L_LAST || id == IDC_L_NOW) f = g.fBold;
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    return TRUE;
}

static void SetupListColumns() {
    HWND rl = Item(IDC_R_LIST), hl = Item(IDC_H_LIST);
    while (ListView_DeleteColumn(rl, 0)) {}
    while (ListView_DeleteColumn(hl, 0)) {}
    LVCOLUMNW c = {};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    struct Col { const wchar_t* t; int w; int fmt; };
    Col rc[] = { { L"Item (on the NAS)", 370, LVCFMT_LEFT }, { L"Size", 90, LVCFMT_RIGHT }, { L"Files", 80, LVCFMT_RIGHT } };
    for (int i = 0; i < 3; ++i) { c.pszText = (LPWSTR)rc[i].t; c.cx = S(rc[i].w); c.fmt = rc[i].fmt; ListView_InsertColumn(rl, i, &c); }
    Col hc[] = { { L"Started", 120, LVCFMT_LEFT }, { L"Result", 90, LVCFMT_LEFT }, { L"Files", 60, LVCFMT_RIGHT },
                 { L"Size", 70, LVCFMT_RIGHT }, { L"Duration", 65, LVCFMT_RIGHT }, { L"Note", 170, LVCFMT_LEFT } };
    for (int i = 0; i < 6; ++i) { c.pszText = (LPWSTR)hc[i].t; c.cx = S(hc[i].w); c.fmt = hc[i].fmt; ListView_InsertColumn(hl, i, &c); }
}

// List views: draw the column header text in the theme's text colour (dark mode headers are otherwise unreadable)
static LRESULT CALLBACK ListSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {
        NMHDR* n = (NMHDR*)lp;
        if (n->code == NM_CUSTOMDRAW && n->hwndFrom == ListView_GetHeader(h)) {
            NMCUSTOMDRAW* cd = (NMCUSTOMDRAW*)lp;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) { SetTextColor(cd->hdc, g.cText); return CDRF_DODEFAULT; }
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void CreateControls() {
    // the tab strip and separator lines are drawn in WM_PAINT (themeable, no empty tab frame)
    Ctl(L"STATIC", L"", SS_ICON | SS_CENTERIMAGE | SS_REALSIZECONTROL, IDC_STATICON);
    Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_HEADLINE);
    Ctl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_DETAIL);
    Ctl(L"STATIC", L"NAS", SS_LEFT, IDC_L_NAS);
    Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_NAS1);
    Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_NAS2);
    Ctl(PROGRESS_CLASSW, L"", 0, IDC_NASBAR);
    Ctl(L"STATIC", L"Automatic", SS_LEFT, IDC_L_AUTO);
    Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_AUTO);
    Ctl(L"BUTTON", L"Pause", WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_PAUSE);
    Ctl(L"BUTTON", L"Schedule \u25BE", WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_SCHEDULE);
    Ctl(L"STATIC", L"Next backup", SS_LEFT, IDC_L_NEXT);
    Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_NEXT);
    Ctl(L"STATIC", L"Last backup", SS_LEFT, IDC_L_LAST);
    Ctl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_LAST);
    Ctl(L"STATIC", L"Now", SS_LEFT, IDC_L_NOW);
    Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_NOW1);
    Ctl(PROGRESS_CLASSW, L"", 0, IDC_NOWBAR);
    Ctl(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX, IDC_NOWFILE);
    Ctl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_NOWHINT);
    Ctl(L"BUTTON", L"Stop", WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_STOP);
    Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_REVIEWHINT);
    Ctl(L"BUTTON", L"Review...", WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_REVIEW);
    Ctl(L"BUTTON", L"Backup now", WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_BACKUP);
    Ctl(L"BUTTON", L"Open NAS folder", WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_OPENNAS);
    Ctl(L"BUTTON", L"Open log", WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_OPENLOG);
    Ctl(L"BUTTON", L"Edit config...", WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_CONFIG);

    Ctl(L"STATIC", L"These files and folders were removed or moved on this laptop, but they are still kept on the NAS. "
                   L"Nothing is ever deleted. Tick items and choose what to do:", SS_LEFT, IDC_R_HEADER);
    Ctl(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, IDC_R_LIST);
    Ctl(L"BUTTON", L"Keep forever", WS_TABSTOP | BS_PUSHBUTTON, IDC_R_KEEP);
    Ctl(L"BUTTON", L"Move to old versions", WS_TABSTOP | BS_PUSHBUTTON, IDC_R_MOVE);
    Ctl(L"BUTTON", L"Delete from NAS...", WS_TABSTOP | BS_PUSHBUTTON, IDC_R_DELETE);
    Ctl(L"BUTTON", L"Ask later", WS_TABSTOP | BS_PUSHBUTTON, IDC_R_LATER);
    Ctl(L"BUTTON", L"Select all", WS_TABSTOP | BS_PUSHBUTTON, IDC_R_ALL);
    Ctl(L"BUTTON", L"Check again", WS_TABSTOP | BS_PUSHBUTTON, IDC_R_CHECK);
    Ctl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_R_INFO);

    Ctl(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, IDC_H_LIST);
    Ctl(L"BUTTON", L"Open log folder", WS_TABSTOP | BS_PUSHBUTTON, IDC_H_OPENLOGS);

    ListView_SetExtendedListViewStyle(Item(IDC_R_LIST), LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_SetExtendedListViewStyle(Item(IDC_H_LIST), LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowSubclass(Item(IDC_R_LIST), ListSubclass, 1, 0);
    SetWindowSubclass(Item(IDC_H_LIST), ListSubclass, 1, 0);
    SendMessageW(Item(IDC_NASBAR), PBM_SETRANGE32, 0, 1000);
    SendMessageW(Item(IDC_NOWBAR), PBM_SETRANGE32, 0, 1000);

    // tooltips for every button (the actions are not all self-explanatory)
    g.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g.hwnd, NULL, g.inst, NULL);
    struct Tip { int id; const wchar_t* text; };
    static const Tip tips[] = {
        { IDC_BTN_BACKUP,   L"Start a backup right now - even if today's backup is already done or backups are paused." },
        { IDC_BTN_OPENNAS,  L"Open the backup folder on the NAS in Explorer." },
        { IDC_BTN_OPENLOG,  L"Open the log of the last (or the running) backup in Notepad." },
        { IDC_BTN_CONFIG,   L"Open backup-config.psd1 in Notepad: which folders are backed up, the NAS path, and the time of the daily backup." },
        { IDC_BTN_PAUSE,    L"Pause the automatic backup for a while (it resumes by itself), resume it, or turn it on." },
        { IDC_BTN_SCHEDULE, L"Turn the automatic backup on or off, repair or remove it, and choose whether Hamster starts with Windows." },
        { IDC_BTN_STOP,     L"Stop the running backup. Everything copied so far is kept; the next run continues where this one stopped." },
        { IDC_BTN_REVIEW,   L"Go to the Review page and decide what happens with files that are no longer on this laptop." },
        { IDC_R_KEEP,       L"Keep the ticked items on the NAS and never ask about them again (they are written to .behalten.txt on the NAS)." },
        { IDC_R_MOVE,       L"Move the ticked items into the .versionen folder on the NAS: out of the way, but still there. Hamster never deletes anything there by itself." },
        { IDC_R_DELETE,     L"Permanently delete the ticked items from the NAS. The NAS has no recycle bin for this - it cannot be undone. You are asked to confirm first." },
        { IDC_R_LATER,      L"Hide the ticked items until the next weekly check. Nothing is changed on the NAS." },
        { IDC_R_ALL,        L"Tick all items, or untick them if all are ticked." },
        { IDC_R_CHECK,      L"Search the NAS now for files that are no longer on this laptop. Takes a few minutes; the list updates by itself." },
        { IDC_H_OPENLOGS,   L"Open the folder with all backup logs on the NAS." },
    };
    SendMessageW(g.tooltip, TTM_SETMAXTIPWIDTH, 0, 420);
    SendMessageW(g.tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    for (const Tip& t : tips) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = g.hwnd;
        ti.uId = (UINT_PTR)Item(t.id);
        ti.lpszText = (LPWSTR)t.text;
        SendMessageW(g.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

static const int kClientW = 640, kClientH = 560;

static void Place(int id, int x, int y, int w, int h) {
    HWND c = Item(id);
    if (c) MoveWindow(c, S(x), S(y), S(w), S(h), FALSE);
}

static const int kTabTop = 6, kTabH = 34, kTop = 52, kSepY = kTop + 72;

static void Layout() {
    // tab strip: text-sized "tabs" drawn in WM_PAINT
    HDC dc = GetDC(g.hwnd);
    HFONT old = (HFONT)SelectObject(dc, g.fBold);
    int x = S(14);
    for (int i = 0; i < 3; ++i) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, g.tabText[i].c_str(), (int)g.tabText[i].size(), &sz);
        SetRect(&g.tabRect[i], x, S(kTabTop), x + sz.cx + S(24), S(kTabTop + kTabH));
        x = g.tabRect[i].right + S(4);
    }
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);

    // current client size in 96-dpi units (the window is resizable; never smaller than the default)
    RECT crc; GetClientRect(g.hwnd, &crc);
    const int W = std::max(kClientW, MulDiv(crc.right, 96, (int)g.dpi));
    const int H = std::max(kClientH, MulDiv(crc.bottom, 96, (int)g.dpi));
    const int top = kTop, L = 20, V = 130;
    // overview
    Place(IDC_STATICON, L, top + 4, 32, 32);
    Place(IDC_HEADLINE, 62, top + 2, W - 62 - L, 26);
    Place(IDC_DETAIL, 62, top + 30, W - 62 - L, 34);
    int y = top + 84;
    Place(IDC_L_NAS, L, y, 100, 20);       Place(IDC_NAS1, V, y, W - V - L, 20);
    Place(IDC_NAS2, V, y + 20, 300, 20);   Place(IDC_NASBAR, V + 305, y + 23, W - V - L - 305, 13);
    y += 50;
    Place(IDC_L_AUTO, L, y + 3, 100, 20);  Place(IDC_AUTO, V, y + 3, 250, 20);
    Place(IDC_BTN_PAUSE, W - L - 230, y, 110, 26); Place(IDC_BTN_SCHEDULE, W - L - 114, y, 114, 26);
    y += 36;
    Place(IDC_L_NEXT, L, y, 100, 20);      Place(IDC_NEXT, V, y, W - V - L, 20);
    y += 28;
    Place(IDC_L_LAST, L, y, 100, 20);      Place(IDC_LAST, V, y, W - V - L, 38);
    y += 50;
    Place(IDC_L_NOW, L, y + 3, 100, 20);   Place(IDC_NOW1, V, y + 3, W - V - L - 90, 20);
    Place(IDC_BTN_STOP, W - L - 80, y, 80, 26);
    Place(IDC_NOWBAR, V, y + 30, W - V - L, 14);
    Place(IDC_NOWFILE, V, y + 48, W - V - L, 18);
    Place(IDC_NOWHINT, V, y + 66, W - V - L, 18);
    y += 96;
    Place(IDC_REVIEWHINT, L, y + 4, W - 2 * L - 120, 20); Place(IDC_BTN_REVIEW, W - L - 110, y, 110, 26);
    int by = H - 46;
    Place(IDC_BTN_BACKUP, L, by, 120, 30);
    Place(IDC_BTN_OPENNAS, L + 128, by, 130, 30);
    Place(IDC_BTN_OPENLOG, L + 266, by, 100, 30);
    Place(IDC_BTN_CONFIG, L + 374, by, 120, 30);
    // review: header with "Select all" / "Check again" on the right, action buttons below the list
    Place(IDC_R_HEADER, L, top + 2, W - 2 * L - 232, 52);
    Place(IDC_R_ALL, W - L - 224, top + 4, 108, 28);
    Place(IDC_R_CHECK, W - L - 108, top + 4, 108, 28);
    int listH = H - (top + 60) - 104;               // the list takes all the extra space
    Place(IDC_R_LIST, L, top + 60, W - 2 * L, listH);
    int ry = top + 60 + listH + 10, gap = 8, bx = L;
    struct B { int id, w; } rb[] = { { IDC_R_KEEP, 124 }, { IDC_R_MOVE, 162 }, { IDC_R_DELETE, 150 }, { IDC_R_LATER, 108 } };
    for (const B& b : rb) { Place(b.id, bx, ry, b.w, 30); bx += b.w + gap; }
    Place(IDC_R_INFO, L, ry + 40, W - 2 * L, 44);
    // history
    Place(IDC_H_LIST, L, top + 4, W - 2 * L, H - top - 60);
    Place(IDC_H_OPENLOGS, L, H - 46, 140, 30);

    // stretch the wide text columns with the window (the others keep their width)
    int inner = W - 2 * L - 26;                     // minus border and vertical scrollbar
    ListView_SetColumnWidth(Item(IDC_R_LIST), 0, S(std::max(200, inner - 90 - 80)));
    ListView_SetColumnWidth(Item(IDC_H_LIST), 5, S(std::max(170, inner - 120 - 90 - 60 - 70 - 65)));
    RedrawWindow(g.hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);   // controls were moved without repainting
}

static DWORD kWinStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;    // resizable, with maximize button

static void MinWindowSize(int& w, int& h) {
    RECT rc = { 0, 0, S(kClientW), S(kClientH) };
    AdjustWindowRectExForDpi(&rc, kWinStyle, FALSE, 0, g.dpi);
    w = rc.right - rc.left; h = rc.bottom - rc.top;
}

static void ApplyDpi(UINT dpi) {
    g.dpi = dpi ? dpi : 96;
    CreateFonts();
    EnumChildWindows(g.hwnd, SetFontProc, 0);
    LoadIcons();
    SetupListColumns();
    Layout();
    SendMessageW(g.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g.appSmall);
    SendMessageW(g.hwnd, WM_SETICON, ICON_BIG, (LPARAM)g.appBig);
}

static void ShowTab(int tab) {
    g.tab = tab;
    InvalidateRect(g.hwnd, NULL, TRUE);
    for (int id : kOverview) ShowWindow(Item(id), tab == 0 ? SW_SHOWNA : SW_HIDE);
    for (int id : kReview)   ShowWindow(Item(id), tab == 1 ? SW_SHOWNA : SW_HIDE);
    for (int id : kHistory)  ShowWindow(Item(id), tab == 2 ? SW_SHOWNA : SW_HIDE);
}

// ------------------------------------------------------------------ window: content

static void FillReviewList() {
    HWND lv = Item(IDC_R_LIST);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    int row = 0;
    for (size_t i = 0; i < g.review.size(); ++i) {
        const ReviewItem& r = g.review[i];
        if (g.askLater.count(r.path)) continue;
        std::wstring label = r.path + (r.kind == L"folder" ? L"  (folder)" : L"");
        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = row;
        it.pszText = (LPWSTR)label.c_str();
        it.lParam = (LPARAM)i;
        ListView_InsertItem(lv, &it);
        std::wstring sz = Size(r.bytes), fl = Num(r.files);
        ListView_SetItemText(lv, row, 1, (LPWSTR)sz.c_str());
        ListView_SetItemText(lv, row, 2, (LPWSTR)fl.c_str());
        ++row;
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    std::wstring info;
    if (g.reviewReportUtc == 0) info = L"No list yet. The check runs once a week after a backup, or click \"Check again\".";
    else if (row == 0) info = L"Nothing to decide. Every file on the NAS exists on this laptop or is marked \"keep\". (Checked " + When(g.reviewReportUtc) + L".)";
    else info = Fmt(L"%d item(s), checked ", row) + When(g.reviewReportUtc) + L".";
    if (!g.reviewInfo.empty()) info = g.reviewInfo + L"\n" + info;
    SetText(IDC_R_INFO, info);
    std::wstring tabText = row ? Fmt(L"Review (%d)", row) : std::wstring(L"Review");
    if (tabText != g.tabText[1]) { g.tabText[1] = tabText; Layout(); }
}

static std::wstring ResultWord(const std::wstring& r) {
    if (r == L"ok") return L"OK";
    if (r == L"warnings") return L"OK (warnings)";
    if (r == L"errors") return L"Errors";
    if (r == L"failed") return L"Failed";
    if (r == L"interrupted") return L"Interrupted";
    if (r == L"stopped") return L"Stopped";
    return r;
}

static void FillHistory() {
    HWND lv = Item(IDC_H_LIST);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    int row = 0;
    for (const HistRow& h : g.history) {
        std::wstring c0 = When(h.started);
        if (h.mode == L"dryrun") c0 += L" (dry run)";
        else if (h.mode == L"report") c0 += L" (check)";
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = (LPWSTR)c0.c_str();
        ListView_InsertItem(lv, &it);
        std::wstring c1 = ResultWord(h.result), c2 = Num(h.files), c3 = Size(h.bytes), c4 = Duration(h.ended - h.started), c5 = h.message;
        if (c5.empty() && h.trigger == L"manual") c5 = L"started by you";
        ListView_SetItemText(lv, row, 1, (LPWSTR)c1.c_str());
        ListView_SetItemText(lv, row, 2, (LPWSTR)c2.c_str());
        ListView_SetItemText(lv, row, 3, (LPWSTR)c3.c_str());
        ListView_SetItemText(lv, row, 4, (LPWSTR)c4.c_str());
        ListView_SetItemText(lv, row, 5, (LPWSTR)c5.c_str());
        ++row;
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
}

static void UpdateOverview(const View& v) {
    if (g.viewIcon != v.icon || !SendMessageW(Item(IDC_STATICON), STM_GETICON, 0, 0)) {
        SendMessageW(Item(IDC_STATICON), STM_SETICON, (WPARAM)g.bigIcon[v.icon], 0);
    }
    g.viewIcon = v.icon;
    g.headColor = StateColor(v.icon);
    SetText(IDC_HEADLINE, v.headline);
    InvalidateRect(Item(IDC_HEADLINE), NULL, TRUE);
    SetText(IDC_DETAIL, v.detail);

    // NAS
    unsigned long long fb = 0, tb = 0;
    long long since = 0;
    int ns = NasStateNow(&fb, &tb, &since);
    std::wstring root = g.targetRoot.empty() ? std::wstring(L"(not configured)") : g.targetRoot;
    switch (ns) {
    case NAS_OK:     SetText(IDC_NAS1, L"Reachable  \u00B7  " + root); break;
    case NAS_DOWN:   SetText(IDC_NAS1, (since ? L"Not reachable since " + When(since) : std::wstring(L"Not reachable")) + L"  \u00B7  " + root); break;
    case NAS_NORESP: SetText(IDC_NAS1, L"Not responding  \u00B7  " + root); break;
    case NAS_LOGIN:  SetText(IDC_NAS1, L"Reachable, but Windows has no valid login - open the folder once in Explorer and save the password"); break;
    default:         SetText(IDC_NAS1, L"Checking...  \u00B7  " + root); break;
    }
    if (tb > 0) {
        int usedPermille = (int)((tb - fb) * 1000 / tb);
        SetText(IDC_NAS2, Size((long long)fb) + L" free of " + Size((long long)tb) + Fmt(L" (%d %% used)", (usedPermille + 5) / 10));
        SendMessageW(Item(IDC_NASBAR), PBM_SETPOS, usedPermille, 0);
        SendMessageW(Item(IDC_NASBAR), PBM_SETBARCOLOR, 0, (LPARAM)(fb * 20 < tb ? StateColor(IS_RED) : g.cAccent));
        Show(IDC_NASBAR, g.tab == 0);
    }
    else {
        SetText(IDC_NAS2, L"");
        Show(IDC_NASBAR, false);
    }

    // Automatic
    std::wstring autoText, pauseBtn = L"Pause \u25BE";
    switch (g.taskState) {
    case TS_MISSING:  autoText = L"Not set up"; pauseBtn = L"Set up"; break;
    case TS_DISABLED: autoText = L"Off"; pauseBtn = L"Turn on"; break;
    case TS_ERROR:    autoText = L"Unknown (Task Scheduler not reachable)"; break;
    case TS_UNKNOWN:  autoText = L"Checking..."; break;
    default:
        if (g.pausedUntil) {
            autoText = g.pausedUntil == LLONG_MAX ? std::wstring(L"Paused until you resume") : L"Paused until " + WhenShort(g.pausedUntil);
            pauseBtn = L"Resume";
        }
        else autoText = L"On  \u00B7  daily after " + g.dailyAfter;
    }
    SetText(IDC_AUTO, autoText);
    SetText(IDC_BTN_PAUSE, pauseBtn);
    EnableWindow(Item(IDC_BTN_PAUSE), g.taskState != TS_ERROR && g.taskState != TS_UNKNOWN);

    // Next backup
    long long now = NowMs();
    long long lastSuccess = g.status[L"lastSuccessUtc"].I();
    int h, m;
    ParseHHMM(g.dailyAfter, h, m);
    std::wstring next;
    if (g.taskState == TS_MISSING || g.taskState == TS_DISABLED) next = L"Not scheduled";
    else if (g.pausedUntil) next = g.pausedUntil == LLONG_MAX ? std::wstring(L"Paused") : L"After the pause (" + WhenShort(g.pausedUntil) + L")";
    else if (lastSuccess > 0 && DayDiff(now, lastSuccess) == 0) next = L"Tomorrow after " + g.dailyAfter;
    else if (now < TodayAt(h, m)) next = L"Today after " + g.dailyAfter;
    else next = L"Today, as soon as the NAS is reachable (checked every 30 minutes)";
    SetText(IDC_NEXT, next);

    // Last backup
    const JVal& last = g.status[L"lastRun"];
    std::wstring lastText;
    if (last.IsNull()) lastText = lastSuccess > 0 ? L"Successful " + When(lastSuccess) : L"None yet";
    else {
        std::wstring res = last[L"result"].S();
        lastText = When(last[L"endedUtc"].I()) + L"  \u00B7  " + ResultWord(res) + L"  \u00B7  " +
                   Num(last[L"filesCopied"].I()) + L" files copied (" + Size(last[L"bytesCopied"].I()) + L")";
        long long ver = last[L"versioned"].I();
        if (ver > 0) lastText += L"  \u00B7  " + Num(ver) + L" older versions kept";
        std::wstring second = Fmt(L"%lld warnings  \u00B7  %lld errors", last[L"warnings"].I(), last[L"errors"].I());
        std::wstring msg = last[L"message"].S();
        if (!msg.empty()) second += L"  \u00B7  " + msg;
        lastText += L"\n" + second;
    }
    SetText(IDC_LAST, lastText);

    // Now (only while running)
    bool running = IsRunningNow();
    const JVal& cur = g.status[L"current"];
    bool showNow = running && g.tab == 0;
    for (int id : { IDC_L_NOW, IDC_NOW1, IDC_NOWBAR, IDC_NOWFILE, IDC_NOWHINT, IDC_BTN_STOP }) Show(id, showNow);
    if (running && !cur.IsNull()) {
        long long bp = cur[L"bytesPlanned"].I(), bd = cur[L"bytesDone"].I(), speed = cur[L"bytesPerSec"].I();
        long long fp = cur[L"filesPlanned"].I(), fd = cur[L"filesDone"].I();
        int permille = bp > 0 ? (int)std::min<long long>(1000, bd * 1000 / bp) : 0;
        std::wstring line = Fmt(L"%s (%lld/%lld)", cur[L"folder"].S().c_str(), cur[L"folderIndex"].I(), cur[L"folderTotal"].I());
        if (cur[L"phase"].S() == L"scanning") line += L"  \u00B7  looking for changes";
        else {
            line += Fmt(L"  \u00B7  %d %%  \u00B7  %s of %s files  \u00B7  %s of %s", permille / 10, Num(fd).c_str(), Num(fp).c_str(),
                        Size(bd).c_str(), Size(bp).c_str());
            if (speed > 0) {
                line += L"  \u00B7  " + Size(speed) + L"/s";
                if (bp > bd) line += L"  \u00B7  " + Eta((bp - bd) / speed);
            }
        }
        SetText(IDC_NOW1, line);
        SendMessageW(Item(IDC_NOWBAR), PBM_SETPOS, permille, 0);
        SetText(IDC_NOWFILE, cur[L"currentFile"].S());
        SetText(IDC_NOWHINT, L"Stopping keeps everything copied so far. The next run continues where this one stopped.");
    }
    else if (running) {
        SetText(IDC_NOW1, L"Starting...");
        SendMessageW(Item(IDC_NOWBAR), PBM_SETPOS, 0, 0);
        SetText(IDC_NOWFILE, L"");
        SetText(IDC_NOWHINT, L"");
    }
    EnableWindow(Item(IDC_BTN_BACKUP), !running && g.launchPendingUntil <= now);

    // Review hint
    int rc = VisibleReviewCount();
    Show(IDC_REVIEWHINT, rc > 0 && g.tab == 0);
    Show(IDC_BTN_REVIEW, rc > 0 && g.tab == 0);
    if (rc > 0) SetText(IDC_REVIEWHINT, Fmt(L"\u26A0  %d item(s) are no longer on this laptop - decide whether the NAS should keep them", rc));
}

// ------------------------------------------------------------------ refresh

static void Refresh(bool full) {
    ReadStatus();
    if (!g.statusOk) LoadConfigFallback();
    EnterCriticalSection(&gn.cs);
    bool rootChanged = gn.root != g.targetRoot;
    gn.root = g.targetRoot;
    LeaveCriticalSection(&gn.cs);
    if (rootChanged) WakeNas();
    g.engineRunning = IsEngineRunning();
    if (g.engineRunning) g.launchPendingUntil = 0;
    ReadPause();
    ReadNasOnly();
    if (full || g.tab == 2) ReadHistory();

    View v = ComputeView();
    TrayUpdate(v.icon, v.tip.empty() ? v.headline : v.tip);
    if (g.visible) {
        UpdateOverview(v);
        std::wstring rsig = Fmt(L"%lld|%d|%d|", g.reviewReportUtc, (int)g.review.size(), (int)g.askLater.size()) + g.reviewInfo;
        if (full || rsig != g.reviewSig) { FillReviewList(); g.reviewSig = rsig; }
        std::wstring hsig = g.history.empty() ? std::wstring() : Fmt(L"%d|", (int)g.history.size()) + g.history.front().runId;
        if ((full || g.tab == 2) && hsig != g.historySig) { FillHistory(); g.historySig = hsig; }
    }

    // notifications (each at most once per key)
    long long now = NowMs();
    const JVal& last = g.status[L"lastRun"];
    std::wstring lr = last[L"result"].S();
    if (g.wasRunning && !g.engineRunning && (lr == L"ok" || lr == L"warnings") && g.prevRunTrigger == L"manual") {
        NotifyOnce(L"finished", last[L"runId"].S(), L"Backup finished",
                   Num(last[L"filesCopied"].I()) + L" files copied (" + Size(last[L"bytesCopied"].I()) + L").");
    }
    if ((lr == L"errors" || lr == L"failed") && last[L"endedUtc"].I() > g.status[L"lastSuccessUtc"].I()) {
        NotifyOnce(L"failed", last[L"runId"].S(), L"Backup failed",
                   (last[L"message"].S().empty() ? std::wstring(L"See the log for details.") : last[L"message"].S()) + L" It retries automatically.");
    }
    long long ls = g.status[L"lastSuccessUtc"].I();
    if (ls > 0 && !g.pausedUntil) {
        int age = DayDiff(now, ls);
        if (age >= g.warnDays && (age <= g.warnDays + 2 || age % 3 == 0)) {
            std::wstring why = NasReason();
            NotifyOnce(L"overdue", TodayKey(), Fmt(L"No backup for %d days", age),
                       (why.empty() ? std::wstring(L"Check that the NAS is reachable.") : why + L".") + L" Open Hamster for details.");
        }
    }
    if (g.taskState == TS_DISABLED || g.taskState == TS_MISSING) {
        NotifyOnce(L"autooff", TodayKey(), L"Automatic backup is off", L"Open Hamster to turn it on again.");
    }
    if (g.reviewReportUtc > 0 && VisibleReviewCount() > 0) {
        NotifyOnce(L"review", std::to_wstring(g.reviewReportUtc), Fmt(L"%d item(s) are no longer on this PC", VisibleReviewCount()),
                   L"They are still kept on the NAS. Open Hamster \u2192 Review to decide.");
    }
    unsigned long long fb = 0, tb = 0;
    if (NasStateNow(&fb, &tb) == NAS_OK && tb > 0 && fb * 10 < tb) {
        SYSTEMTIME lt; MsToLocal(now, lt);
        NotifyOnce(L"lowspace", Fmt(L"%04d-w%02d", lt.wYear, (lt.wDay - 1) / 7 + lt.wMonth * 5), L"The NAS is getting full",
                   L"Only " + Size((long long)fb) + L" free. Hamster never deletes anything, so old versions pile up over time.");
    }
    if (g.engineRunning && !g.status[L"current"].IsNull()) g.prevRunTrigger = g.status[L"current"][L"trigger"].S();
    g.wasRunning = g.engineRunning;

    // refresh cadence
    UINT interval = g.visible ? 2000 : (g.engineRunning ? 10000 : 30000);
    if (g.launchPendingUntil > now) interval = 1000;
    SetTimer(g.hwnd, TIMER_REFRESH, interval, NULL);
}

// ------------------------------------------------------------------ actions

static void ShowMainWindow() {
    g.visible = true;
    InterlockedExchange(&gn.visible, 1);
    WakeNas();
    RefreshTask();
    ShowWindow(g.hwnd, SW_SHOW);
    if (IsIconic(g.hwnd)) ShowWindow(g.hwnd, SW_RESTORE);
    SetForegroundWindow(g.hwnd);
    ShowTab(g.tab);
    Refresh(true);
}

static void HideMainWindow() {
    g.visible = false;
    InterlockedExchange(&gn.visible, 0);
    ShowWindow(g.hwnd, SW_HIDE);
    Refresh(false);
}

static void Message(const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
    MessageBoxW(g.visible ? g.hwnd : NULL, text.c_str(), kAppLongName, MB_OK | icon | MB_SETFOREGROUND);
}

static void DoBackupNow() {
    if (IsEngineRunning()) return;
    if (!FileExists(g.engine)) { Message(L"Backup.ps1 was not found next to Hamster:\n" + g.engine, MB_ICONERROR); return; }
    int ns = NasStateNow();
    if (ns == NAS_DOWN) {
        Balloon(L"NAS not reachable", L"The backup can't start right now. It runs automatically once the NAS is back.");
        return;
    }
    if (!LaunchEngine(L"-Scheduled -Force")) { Message(L"Could not start the backup (PowerShell could not be started).", MB_ICONERROR); return; }
    g.launchPendingUntil = NowMs() + 30000;
    g.prevRunTrigger = L"manual";
    Refresh(false);
}

static void DoStop() {
    if (!IsEngineRunning()) return;
    if (MessageBoxW(g.hwnd, L"Stop the running backup?\n\nEverything copied so far is kept. The next run continues where this one stopped.",
                    kAppLongName, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) return;
    CreateDirectoryW(g.stateDir.c_str(), NULL);
    WriteTextFileAtomic(g.stateDir + L"\\stop.request", L"stop");
    Balloon(L"Stopping backup", L"The backup stops within a few seconds.");
}

static void DoPause(int days) {
    if (days < 0) SetPause(LLONG_MAX);
    else SetPause(NowMs() + (long long)days * 86400000LL);
    Refresh(false);
}

static void DoOpenNas() {
    if (g.targetRoot.empty()) return;
    if (NasStateNow() == NAS_DOWN) { Message(L"The NAS is not reachable right now.", MB_ICONWARNING); return; }
    ShellExecuteW(NULL, L"open", g.targetRoot.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

static void DoOpenLog() {
    std::wstring log = g.status[L"lastRun"][L"logFile"].S();
    const JVal& cur = g.status[L"current"];
    if (IsRunningNow() && !cur.IsNull() && !cur[L"logFile"].S().empty()) log = cur[L"logFile"].S();
    if (log.empty()) { Message(L"There is no log yet."); return; }
    if (log.compare(0, 2, L"\\\\") == 0 && NasStateNow() == NAS_DOWN) { Message(L"The log is on the NAS, which is not reachable right now.", MB_ICONWARNING); return; }
    ShellExecuteW(NULL, L"open", L"notepad.exe", (L"\"" + log + L"\"").c_str(), NULL, SW_SHOWNORMAL);
}

static void DoOpenLogFolder() {
    if (g.targetRoot.empty() || NasStateNow() == NAS_DOWN) { ShellExecuteW(NULL, L"open", (g.dir + L"\\logs").c_str(), NULL, NULL, SW_SHOWNORMAL); return; }
    ShellExecuteW(NULL, L"open", (g.targetRoot + L"\\.logs").c_str(), NULL, NULL, SW_SHOWNORMAL);
}

static void DoEditConfig() {
    ShellExecuteW(NULL, L"open", L"notepad.exe", (L"\"" + g.configPath + L"\"").c_str(), NULL, SW_SHOWNORMAL);
}

static void DoTask(int cmd) {
    HRESULT hr = S_OK;
    if (cmd == CMD_TASK_INSTALL) hr = RegisterTask(true);
    else if (cmd == CMD_TASK_ON) hr = (g.taskState == TS_MISSING) ? RegisterTask(true) : SetTaskEnabled(true);
    else if (cmd == CMD_TASK_OFF) {
        if (MessageBoxW(g.hwnd, L"Turn automatic backup off?\n\nBackups then only run when you click \"Backup now\". "
                        L"(To stop it only for a while, use Pause instead.)", kAppLongName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        hr = SetTaskEnabled(false);
    }
    else if (cmd == CMD_TASK_REMOVE) {
        if (MessageBoxW(g.hwnd, L"Remove the automatic backup from Windows Task Scheduler?\n\nYour backups on the NAS are not touched. "
                        L"You can set it up again any time.", kAppLongName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        hr = DeleteTask(kTaskName);
    }
    if (FAILED(hr)) Message(L"Task Scheduler reported an error:\n" + HrText(hr), MB_ICONERROR);
    RefreshTask();
    Refresh(false);
}

// --- Review actions (hold the engine mutex so no backup runs meanwhile)

struct EngineLock {
    HANDLE h = NULL;
    bool ok = false;
    EngineLock() {
        h = CreateMutexW(NULL, FALSE, kEngineMutex);
        if (h) {
            DWORD w = WaitForSingleObject(h, 0);
            ok = (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED);
        }
    }
    ~EngineLock() {
        if (h) { if (ok) ReleaseMutex(h); CloseHandle(h); }
    }
};

static std::vector<size_t> CheckedReviewItems() {
    std::vector<size_t> out;
    HWND lv = Item(IDC_R_LIST);
    int n = ListView_GetItemCount(lv);
    for (int i = 0; i < n; ++i) {
        if (!ListView_GetCheckState(lv, i)) continue;
        LVITEMW it = {}; it.mask = LVIF_PARAM; it.iItem = i;
        ListView_GetItem(lv, &it);
        out.push_back((size_t)it.lParam);
    }
    return out;
}

static bool CreateDirs(const std::wstring& longDir) {
    // longDir starts with \\?\UNC\server\share\ ... create every missing level
    size_t start = longDir.find(L'\\', 8);              // after \\?\UNC\server
    if (start != std::wstring::npos) start = longDir.find(L'\\', start + 1);   // after share
    while (start != std::wstring::npos) {
        size_t next = longDir.find(L'\\', start + 1);
        std::wstring part = longDir.substr(0, next);
        if (!CreateDirectoryW(part.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        start = next;
    }
    return true;
}

// Deletes a file or a whole folder tree (long-path API, no recycle bin). Only used by "Delete from NAS"
// in Review, after the user confirmed it; never called automatically.
static bool DeleteTree(const std::wstring& longPath, std::wstring& err) {
    DWORD a = GetFileAttributesW(longPath.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;                 // already gone
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((longPath + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (!DeleteTree(longPath + L"\\" + fd.cFileName, err)) { FindClose(h); return false; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!RemoveDirectoryW(longPath.c_str())) { err = HrText(HRESULT_FROM_WIN32(GetLastError())); return false; }
        return true;
    }
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(longPath.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (!DeleteFileW(longPath.c_str())) { err = HrText(HRESULT_FROM_WIN32(GetLastError())); return false; }
    return true;
}

static bool AppendKeepList(const std::vector<std::wstring>& items, std::wstring& err) {
    std::wstring path = LongPath(g.targetRoot + L"\\.behalten.txt");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { err = HrText(HRESULT_FROM_WIN32(GetLastError())); return false; }
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    std::string out;
    if (size.QuadPart == 0) {
        out += "# Hamster Backup: these files/folders exist only on the NAS and you decided to keep them.\r\n";
        out += "# They are never reported again. One path per line, relative to the share. A line ending\r\n";
        out += "# with \\ covers the whole folder. You can edit this file by hand.\r\n";
    }
    else {
        LARGE_INTEGER pos; pos.QuadPart = size.QuadPart - 1;
        char lastByte = '\n';
        DWORD rd = 0;
        if (SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && ReadFile(h, &lastByte, 1, &rd, NULL) && rd == 1 && lastByte != '\n') out += "\r\n";
    }
    for (const auto& i : items) out += WideToUtf8(i) + "\r\n";
    DWORD wr = 0;
    BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &wr, NULL);
    if (!ok) err = HrText(HRESULT_FROM_WIN32(GetLastError()));
    CloseHandle(h);
    return ok && wr == out.size();
}

static void RewriteNasOnly(const std::set<std::wstring>& handled) {
    // We hold the engine mutex here, so we are the only writer.
    std::wstring j = Fmt(L"{\"schema\":1,\"reportUtc\":%lld,\"items\":[", g.reviewReportUtc);
    bool first = true;
    for (const auto& r : g.review) {
        if (handled.count(r.path)) continue;
        if (!first) j += L",";
        first = false;
        j += L"{\"path\":\"" + JsonEscape(r.path) + L"\",\"kind\":\"" + JsonEscape(r.kind) + L"\",\"files\":" + std::to_wstring(r.files) +
             L",\"bytes\":" + std::to_wstring(r.bytes) + L",\"localPath\":\"" + JsonEscape(r.localPath) + L"\"}";
    }
    j += L"]}";
    WriteTextFileAtomic(g.stateDir + L"\\nas-only.json", j);
}

static void DoReview(int action) {
    std::vector<size_t> sel = CheckedReviewItems();
    if (sel.empty()) { Message(L"Tick one or more items first."); return; }
    if (action == IDC_R_LATER) {
        for (size_t i : sel) g.askLater.insert(g.review[i].path);
        g.reviewInfo = Fmt(L"%d item(s) hidden until the next check.", (int)sel.size());
        FillReviewList();
        Refresh(false);
        return;
    }
    if (NasStateNow() != NAS_OK) { Message(L"The NAS is not reachable right now. Try again when it is back.", MB_ICONWARNING); return; }
    if (action == IDC_R_MOVE) {
        if (MessageBoxW(g.hwnd, Fmt(L"Move %d item(s) into the \".versionen\" folder on the NAS?\n\nNothing is deleted: they stay on the NAS, "
                        L"just out of the way (useful for leftovers after renaming a folder).", (int)sel.size()).c_str(),
                        kAppLongName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    if (action == IDC_R_DELETE) {
        long long total = 0, files = 0;
        std::wstring list;
        int shown = 0;
        for (size_t i : sel) {
            total += g.review[i].bytes; files += g.review[i].files;
            if (shown++ < 8) list += L"\n   " + g.review[i].path;
        }
        if (sel.size() > 8) list += Fmt(L"\n   ... and %d more", (int)sel.size() - 8);
        std::wstring q = Fmt(L"Permanently delete %d item(s) from the NAS?\n", (int)sel.size()) + list +
                         L"\n\n" + Num(files) + L" files, " + Size(total) + L".\n\n"
                         L"The NAS has no recycle bin for this. This cannot be undone.\n"
                         L"(If you are not sure, use \"Move to old versions\" instead.)";
        if (MessageBoxW(g.hwnd, q.c_str(), kAppLongName, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    EngineLock lock;
    if (!lock.ok) { Message(L"A backup is running right now. Try again after it has finished.", MB_ICONWARNING); return; }

    std::set<std::wstring> handled;
    std::wstring problems;
    if (action == IDC_R_KEEP) {
        std::vector<std::wstring> items;
        for (size_t i : sel) items.push_back(g.review[i].path);
        std::wstring err;
        if (AppendKeepList(items, err)) for (const auto& p : items) handled.insert(p);
        else problems = L"Could not write .behalten.txt on the NAS: " + err;
        g.reviewInfo = handled.empty() ? problems : Fmt(L"%d item(s) will be kept forever (written to .behalten.txt).", (int)handled.size());
    }
    else if (action == IDC_R_MOVE) {
        SYSTEMTIME lt; MsToLocal(NowMs(), lt);
        std::wstring stamp = Fmt(L"%04d-%02d-%02d_%02d%02d%02d_review", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
        int moved = 0;
        for (size_t i : sel) {
            const ReviewItem& r = g.review[i];
            std::wstring rel = r.path;
            while (!rel.empty() && rel.back() == L'\\') rel.pop_back();
            // the list may be days old: never move something that exists on the laptop again
            if (!r.localPath.empty() && FileExists(LongPath(r.localPath))) {
                problems += L"\n" + r.path + L": exists on the laptop again - skipped";
                continue;
            }
            std::wstring from = LongPath(g.targetRoot + L"\\" + rel);
            std::wstring to = LongPath(g.targetRoot + L"\\.versionen\\" + stamp + L"\\" + rel);
            if (!FileExists(from)) { handled.insert(r.path); continue; }      // already gone (moved earlier)
            size_t slash = to.rfind(L'\\');
            CreateDirs(to.substr(0, slash));
            std::wstring target = to;
            for (int k = 2; FileExists(target) && k < 100; ++k) target = to + Fmt(L"_%d", k);
            if (MoveFileExW(from.c_str(), target.c_str(), 0)) { handled.insert(r.path); ++moved; }   // rename on the NAS, never a copy
            else problems += L"\n" + r.path + L": " + HrText(HRESULT_FROM_WIN32(GetLastError()));
        }
        g.reviewInfo = Fmt(L"%d item(s) moved to .versionen\\", moved) + stamp + L".";
        if (!problems.empty()) g.reviewInfo += L" Some items were skipped.";
    }
    else if (action == IDC_R_DELETE) {
        HCURSOR oldCur = SetCursor(LoadCursorW(NULL, IDC_WAIT));
        int deleted = 0;
        for (size_t i : sel) {
            const ReviewItem& r = g.review[i];
            std::wstring rel = r.path;
            while (!rel.empty() && rel.back() == L'\\') rel.pop_back();
            // safety: only real items inside a backed-up folder, never the share root or Hamster's own files
            if (rel.empty() || rel.find(L"..") != std::wstring::npos || rel[0] == L'.') {
                problems += L"\n" + r.path + L": not allowed"; continue;
            }
            if (!r.localPath.empty() && FileExists(LongPath(r.localPath))) {
                problems += L"\n" + r.path + L": exists on the laptop again - skipped";
                continue;
            }
            std::wstring err;
            if (DeleteTree(LongPath(g.targetRoot + L"\\" + rel), err)) { handled.insert(r.path); ++deleted; }
            else problems += L"\n" + r.path + L": " + err;
        }
        SetCursor(oldCur);
        g.reviewInfo = Fmt(L"%d item(s) deleted from the NAS.", deleted);
        if (!problems.empty()) g.reviewInfo += L" Some items were skipped.";
    }
    if (!handled.empty()) RewriteNasOnly(handled);
    if (!problems.empty()) Message(L"Some items could not be handled:" + problems, MB_ICONWARNING);
    ReadNasOnly();
    FillReviewList();
    Refresh(false);
}

static void DoCheckAgain() {
    if (IsEngineRunning()) { Message(L"A backup is running. The check runs automatically afterwards once a week; try again later."); return; }
    if (NasStateNow() == NAS_DOWN) { Message(L"The NAS is not reachable right now.", MB_ICONWARNING); return; }
    if (!LaunchEngine(L"-Scheduled -Report")) { Message(L"Could not start the check.", MB_ICONERROR); return; }
    g.askLater.clear();
    g.reviewInfo = L"Checking... this can take a few minutes. The list updates by itself.";
    g.launchPendingUntil = NowMs() + 15000;
    FillReviewList();
    Refresh(false);
}

// ------------------------------------------------------------------ menus

static void AddPauseItems(HMENU m) {
    AppendMenuW(m, MF_STRING, CMD_PAUSE1, L"Pause for 1 day");
    AppendMenuW(m, MF_STRING, CMD_PAUSE3, L"Pause for 3 days");
    AppendMenuW(m, MF_STRING, CMD_PAUSE7, L"Pause for 1 week");
    AppendMenuW(m, MF_STRING, CMD_PAUSEFOREVER, L"Pause until I resume");
}

static void ShowTrayMenu(int x, int y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open Hamster");
    SetMenuDefaultItem(m, CMD_OPEN, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (IsEngineRunning()) AppendMenuW(m, MF_STRING, CMD_STOP, L"Stop backup");
    else AppendMenuW(m, MF_STRING | (g.launchPendingUntil > NowMs() ? MF_GRAYED : 0), CMD_BACKUP, L"Backup now");
    if (g.pausedUntil) AppendMenuW(m, MF_STRING, CMD_RESUME, L"Resume automatic backups");
    else if (g.taskState == TS_ENABLED) {
        HMENU sub = CreatePopupMenu();
        AddPauseItems(sub);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Pause");
    }
    AppendMenuW(m, MF_STRING, CMD_OPENNAS, L"Open NAS folder");
    AppendMenuW(m, MF_STRING, CMD_OPENLOG, L"Open last log");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"Exit (backups keep running)");
    SetForegroundWindow(g.hwnd);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, g.hwnd, NULL);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void ShowButtonMenu(int buttonId, HMENU m) {
    RECT r;
    GetWindowRect(Item(buttonId), &r);
    TrackPopupMenuEx(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, r.left, r.bottom, g.hwnd, NULL);
    DestroyMenu(m);
}

static void OnPauseButton() {
    if (g.taskState == TS_MISSING) { DoTask(CMD_TASK_INSTALL); return; }
    if (g.taskState == TS_DISABLED) { DoTask(CMD_TASK_ON); return; }
    if (g.pausedUntil) { SetPause(0); Refresh(false); return; }
    HMENU m = CreatePopupMenu();
    AddPauseItems(m);
    ShowButtonMenu(IDC_BTN_PAUSE, m);
}

static void OnScheduleButton() {
    HMENU m = CreatePopupMenu();
    if (g.taskState == TS_MISSING) AppendMenuW(m, MF_STRING, CMD_TASK_INSTALL, L"Set up automatic backup");
    else {
        if (g.taskState == TS_DISABLED) AppendMenuW(m, MF_STRING, CMD_TASK_ON, L"Turn automatic backup on");
        else AppendMenuW(m, MF_STRING, CMD_TASK_OFF, L"Turn automatic backup off");
        AppendMenuW(m, MF_STRING, CMD_TASK_INSTALL, L"Reinstall automatic backup (repair)");
        AppendMenuW(m, MF_STRING, CMD_TASK_REMOVE, L"Remove automatic backup");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (AutostartWanted() ? MF_CHECKED : 0), CMD_AUTOSTART, L"Start Hamster with Windows");
    ShowButtonMenu(IDC_BTN_SCHEDULE, m);
}

static void OnCommand(int id) {
    switch (id) {
    case CMD_OPEN: ShowMainWindow(); break;
    case CMD_BACKUP: case IDC_BTN_BACKUP: DoBackupNow(); break;
    case CMD_STOP: case IDC_BTN_STOP: DoStop(); break;
    case CMD_PAUSE1: DoPause(1); break;
    case CMD_PAUSE3: DoPause(3); break;
    case CMD_PAUSE7: DoPause(7); break;
    case CMD_PAUSEFOREVER: DoPause(-1); break;
    case CMD_RESUME: SetPause(0); Refresh(false); break;
    case CMD_OPENNAS: case IDC_BTN_OPENNAS: DoOpenNas(); break;
    case CMD_OPENLOG: case IDC_BTN_OPENLOG: DoOpenLog(); break;
    case IDC_BTN_CONFIG: DoEditConfig(); break;
    case IDC_BTN_PAUSE: OnPauseButton(); break;
    case IDC_BTN_SCHEDULE: OnScheduleButton(); break;
    case CMD_TASK_ON: case CMD_TASK_OFF: case CMD_TASK_INSTALL: case CMD_TASK_REMOVE: DoTask(id); break;
    case CMD_AUTOSTART: RegSetDword(L"Autostart", AutostartWanted() ? 0 : 1); ApplyAutostart(); break;
    case IDC_BTN_REVIEW: ShowTab(1); g.reviewInfo.clear(); Refresh(true); break;
    case IDC_R_KEEP: case IDC_R_MOVE: case IDC_R_DELETE: case IDC_R_LATER: DoReview(id); break;
    case IDC_R_ALL: {
        HWND lv = Item(IDC_R_LIST);
        int n = ListView_GetItemCount(lv);
        bool allOn = n > 0;
        for (int i = 0; i < n; ++i) if (!ListView_GetCheckState(lv, i)) { allOn = false; break; }
        for (int i = 0; i < n; ++i) ListView_SetCheckState(lv, i, !allOn);
        break;
    }
    case IDC_R_CHECK: DoCheckAgain(); break;
    case IDC_H_OPENLOGS: DoOpenLogFolder(); break;
    case CMD_EXIT: DestroyWindow(g.hwnd); break;
    }
}

// ------------------------------------------------------------------ window procedure

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.msgTaskbarCreated && g.msgTaskbarCreated) {
        LoadIcons();
        g.trayRetries = 0;
        TrayAdd();
        return 0;
    }
    switch (msg) {
    case WM_CREATE: {
        g.hwnd = hwnd;
        g.dpi = GetDpiForWindow(hwnd);
        CreateControls();
        ApplyDpi(g.dpi);
        // last used size (stored in 96-dpi units), never smaller than the default
        int cw = std::max(kClientW, (int)RegGetDword(L"WindowWidth", kClientW));
        int ch = std::max(kClientH, (int)RegGetDword(L"WindowHeight", kClientH));
        RECT rc = { 0, 0, S(cw), S(ch) };
        AdjustWindowRectExForDpi(&rc, kWinStyle, FALSE, 0, g.dpi);
        SetWindowPos(hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        ApplyTheme();
        ShowTab(0);
        TrayAdd();
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && g.fBold) Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        if (g.hwnd) {
            int w, h;
            MinWindowSize(w, h);
            ((MINMAXINFO*)lp)->ptMinTrackSize.x = w;
            ((MINMAXINFO*)lp)->ptMinTrackSize.y = h;
        }
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        if (!IsZoomed(hwnd)) {
            RECT c; GetClientRect(hwnd, &c);
            RegSetDword(L"WindowWidth", (DWORD)MulDiv(c.right, 96, (int)g.dpi));
            RegSetDword(L"WindowHeight", (DWORD)MulDiv(c.bottom, 96, (int)g.dpi));
        }
        return 0;
    }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        RECT* r = (RECT*)lp;
        ApplyDpi(g.dpi);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Refresh(true);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        HWND c = (HWND)lp;
        if (c == Item(IDC_HEADLINE)) SetTextColor(dc, g.headColor);
        else if (c == Item(IDC_DETAIL) || c == Item(IDC_NOWHINT) || c == Item(IDC_R_INFO) || c == Item(IDC_NOWFILE)) SetTextColor(dc, g.cSub);
        else SetTextColor(dc, g.cText);
        SetBkColor(dc, g.cBg);
        return (LRESULT)g.bgBrush;
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)g.bgBrush;
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g.bgBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HFONT old = (HFONT)SelectObject(dc, g.fBold);
        SetBkMode(dc, TRANSPARENT);
        // tab strip: labels, accent underline under the selected one, line across the window
        HBRUSH line = CreateSolidBrush(g.cLine), accent = CreateSolidBrush(g.cAccent);
        RECT base = { 0, g.tabRect[0].bottom - 1, rc.right, g.tabRect[0].bottom };
        FillRect(dc, &base, line);
        for (int i = 0; i < 3; ++i) {
            RECT r = g.tabRect[i];
            SetTextColor(dc, i == g.tab ? g.cText : g.cSub);
            DrawTextW(dc, g.tabText[i].c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (i == g.tab) {
                RECT u = { r.left + S(8), r.bottom - S(3), r.right - S(8), r.bottom };
                FillRect(dc, &u, accent);
            }
        }
        if (g.tab == 0) {           // separator under the status block
            RECT sep = { S(20), S(kSepY), rc.right - S(20), S(kSepY) + 1 };
            FillRect(dc, &sep, line);
        }
        DeleteObject(line);
        DeleteObject(accent);
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        for (int i = 0; i < 3; ++i) {
            if (PtInRect(&g.tabRect[i], pt) && i != g.tab) {
                ShowTab(i);
                if (g.tab == 1) g.reviewInfo.clear();
                Refresh(true);
            }
        }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            for (int i = 0; i < 3; ++i) if (PtInRect(&g.tabRect[i], pt)) { SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE; }
        }
        break;
    }
    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;
    case WM_APP_TRAY: {
        UINT ev = LOWORD(lp);
        // NOTIFYICON_VERSION_4: left click = NIN_SELECT, right click = WM_CONTEXTMENU (anchor in wParam)
        if (ev == NIN_SELECT || ev == NIN_KEYSELECT) {
            ShowMainWindow();
        }
        else if (ev == WM_CONTEXTMENU) {
            ShowTrayMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
        }
        else if (ev == NIN_BALLOONUSERCLICK) {
            ShowMainWindow();
        }
        return 0;
    }
    case WM_APP_NAS:
        Refresh(false);
        return 0;
    case WM_APP_SHOW:
        ShowMainWindow();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REFRESH) Refresh(false);
        else if (wp == TIMER_TASK) RefreshTask();
        else if (wp == TIMER_TRAYRETRY) { KillTimer(hwnd, TIMER_TRAYRETRY); TrayAdd(); }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            EnterCriticalSection(&gn.cs); gn.state = NAS_UNKNOWN; gn.since = 0; LeaveCriticalSection(&gn.cs);
            WakeNas();
            Refresh(false);
        }
        return TRUE;
    case WM_SETTINGCHANGE:
        // Windows switched light/dark mode
        if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) ApplyTheme();
        Refresh(false);
        return 0;
    case WM_TIMECHANGE:
        Refresh(false);
        return 0;
    case WM_CLOSE:
        HideMainWindow();       // closing only hides; Exit is in the tray menu
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) { Shell_NotifyIconW(NIM_DELETE, &g.nid); g.trayAdded = false; }
        return 0;
    case WM_DESTROY:
        if (g.trayAdded) Shell_NotifyIconW(NIM_DELETE, &g.nid);
        g.trayAdded = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ entry point

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    g.inst = inst;

    HANDLE single = CreateMutexW(NULL, FALSE, kAppMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kWndClass, NULL);
        if (other) PostMessageW(other, WM_APP_SHOW, 0, 0);
        if (single) CloseHandle(single);
        return 0;
    }

    wchar_t path[MAX_PATH * 4];
    GetModuleFileNameW(NULL, path, _countof(path));
    g.exePath = path;
    g.dir = g.exePath.substr(0, g.exePath.rfind(L'\\'));
    // Hamster.exe lives next to Backup.ps1 (or in an "app" subfolder of it)
    if (!FileExists(g.dir + L"\\Backup.ps1")) {
        std::wstring parent = g.dir.substr(0, g.dir.rfind(L'\\'));
        if (FileExists(parent + L"\\Backup.ps1")) g.dir = parent;
    }
    g.engine = g.dir + L"\\Backup.ps1";
    g.stateDir = g.dir + L"\\state";
    g.configPath = g.dir + L"\\backup-config.psd1";

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    LoadConfigFallback();
    ReadStatus();
    UpdateTaskDefinition();
    RefreshTask();
    ApplyAutostart();

    InitializeCriticalSection(&gn.cs);
    gn.root = g.targetRoot;
    gn.wake = CreateEventW(NULL, FALSE, FALSE, NULL);

    ApplyTheme();           // colours + menu theme (window parts are themed again in WM_CREATE)
    g.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kWndClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kWndClass, L"Hamster", kWinStyle,
                                CW_USEDEFAULT, CW_USEDEFAULT, 700, 620, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ChangeWindowMessageFilterEx(hwnd, g.msgTaskbarCreated, MSGFLT_ALLOW, NULL);

    HANDLE nasThread = CreateThread(NULL, 0, NasProc, NULL, 0, NULL);
    SetTimer(hwnd, TIMER_TASK, 60000, NULL);

    bool startHidden = cmdLine && wcsstr(cmdLine, L"/tray") != NULL;
    if (startHidden) Refresh(true); else ShowMainWindow();

    if (!RegGetDword(L"FirstRunDone", 0)) {
        RegSetDword(L"FirstRunDone", 1);
        Balloon(L"Hamster is running", L"It lives in the notification area. Tip: drag the hamster from the \u02C4 menu onto the taskbar to always see the backup status.");
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    InterlockedExchange(&gn.quit, 1);
    WakeNas();
    if (nasThread) { WaitForSingleObject(nasThread, 3000); CloseHandle(nasThread); }
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
