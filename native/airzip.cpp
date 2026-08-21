// AirZip - archive extractor, raw Win32 + 7-Zip COM. No managed runtime.
//
// Mirrors the behaviour of the previous C# build:
//   AirZip.exe <archive>        extract, with a small progress window
//   AirZip.exe                  ask, then install
//   AirZip.exe --install        install (elevates itself if needed)
//   AirZip.exe --silent-install same, no dialogs
//
// Extraction target follows the "smart folder" rule: one root item inside the
// archive extracts beside the archive, several root items get a folder named
// after the archive.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <mmsystem.h>
#include <string>
#include <vector>
#include <set>
#include <cstdio>
#include <appmodel.h>

#include "resource.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

typedef unsigned int       UInt32;
typedef unsigned long long UInt64;
typedef int                Int32;
typedef long long          Int64;

// ---------------------------------------------------------------------------
// 7-Zip ABI. These declarations must match IArchive.h / IStream.h exactly --
// method order is the vtable layout, so reordering silently corrupts calls.
// ---------------------------------------------------------------------------

// Interfaces: {23170F69-40C1-278A-0000-0000gg0ii0000}
#define SZ_IFACE_GUID(name, grp, idx) \
    static const GUID IID_##name = \
        { 0x23170F69, 0x40C1, 0x278A, { 0, 0, 0, grp, 0, idx, 0, 0 } };

// Format handlers: {23170F69-40C1-278A-1000-000110ff0000}
#define SZ_FORMAT_GUID(name, id) \
    static const GUID CLSID_##name = \
        { 0x23170F69, 0x40C1, 0x278A, { 0x10, 0x00, 0x00, 0x01, 0x10, id, 0x00, 0x00 } };

SZ_IFACE_GUID(ISequentialInStream,       0x03, 0x01)
SZ_IFACE_GUID(IInStream,                 0x03, 0x03)
SZ_IFACE_GUID(ISequentialOutStream,      0x03, 0x02)
SZ_IFACE_GUID(ICryptoGetTextPassword,    0x05, 0x10)
SZ_IFACE_GUID(IArchiveOpenCallback,      0x06, 0x10)
SZ_IFACE_GUID(IArchiveExtractCallback,   0x06, 0x20)
SZ_IFACE_GUID(IArchiveOpenVolumeCallback,0x06, 0x30)
SZ_IFACE_GUID(IInArchive,                0x06, 0x60)

SZ_FORMAT_GUID(Zip,      0x01)
SZ_FORMAT_GUID(BZip2,    0x02)
SZ_FORMAT_GUID(Rar,      0x03)
SZ_FORMAT_GUID(Arj,      0x04)
SZ_FORMAT_GUID(Lzh,      0x06)
SZ_FORMAT_GUID(SevenZip, 0x07)
SZ_FORMAT_GUID(Cab,      0x08)
SZ_FORMAT_GUID(Lzma,     0x0A)
SZ_FORMAT_GUID(Xz,       0x0C)
SZ_FORMAT_GUID(Rar5,     0xCC)
SZ_FORMAT_GUID(Wim,      0xE6)
SZ_FORMAT_GUID(Iso,      0xE7)
SZ_FORMAT_GUID(Tar,      0xEE)
SZ_FORMAT_GUID(GZip,     0xEF)

enum {
    kpidPath  = 3,
    kpidName  = 4,
    kpidIsDir = 6,
    kpidSize  = 7,
    kpidAttrib = 9,
    kpidMTime = 12
};

enum { kExtract = 0, kTest = 1, kSkip = 2 };
enum { kOpOK = 0, kOpUnsupportedMethod = 1, kOpDataError = 2, kOpCRCError = 3 };

struct ISequentialInStream : public IUnknown {
    STDMETHOD(Read)(void *data, UInt32 size, UInt32 *processedSize) PURE;
};

struct IInStream : public ISequentialInStream {
    STDMETHOD(Seek)(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition) PURE;
};

struct ISequentialOutStream : public IUnknown {
    STDMETHOD(Write)(const void *data, UInt32 size, UInt32 *processedSize) PURE;
};

struct ICryptoGetTextPassword : public IUnknown {
    STDMETHOD(CryptoGetTextPassword)(BSTR *password) PURE;
};

struct IArchiveOpenCallback : public IUnknown {
    STDMETHOD(SetTotal)(const UInt64 *files, const UInt64 *bytes) PURE;
    STDMETHOD(SetCompleted)(const UInt64 *files, const UInt64 *bytes) PURE;
};

struct IArchiveOpenVolumeCallback : public IUnknown {
    STDMETHOD(GetProperty)(PROPID propID, PROPVARIANT *value) PURE;
    STDMETHOD(GetStream)(const wchar_t *name, IInStream **inStream) PURE;
};

struct IProgress : public IUnknown {
    STDMETHOD(SetTotal)(UInt64 total) PURE;
    STDMETHOD(SetCompleted)(const UInt64 *completeValue) PURE;
};

struct IArchiveExtractCallback : public IProgress {
    STDMETHOD(GetStream)(UInt32 index, ISequentialOutStream **outStream, Int32 askExtractMode) PURE;
    STDMETHOD(PrepareOperation)(Int32 askExtractMode) PURE;
    STDMETHOD(SetOperationResult)(Int32 opRes) PURE;
};

struct IInArchive : public IUnknown {
    STDMETHOD(Open)(IInStream *stream, const UInt64 *maxCheckStartPosition,
                    IArchiveOpenCallback *openCallback) PURE;
    STDMETHOD(Close)() PURE;
    STDMETHOD(GetNumberOfItems)(UInt32 *numItems) PURE;
    STDMETHOD(GetProperty)(UInt32 index, PROPID propID, PROPVARIANT *value) PURE;
    STDMETHOD(Extract)(const UInt32 *indices, UInt32 numItems, Int32 testMode,
                       IArchiveExtractCallback *extractCallback) PURE;
    STDMETHOD(GetArchiveProperty)(PROPID propID, PROPVARIANT *value) PURE;
    STDMETHOD(GetNumberOfProperties)(UInt32 *numProps) PURE;
    STDMETHOD(GetPropertyInfo)(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType) PURE;
    STDMETHOD(GetNumberOfArchiveProperties)(UInt32 *numProps) PURE;
    STDMETHOD(GetArchivePropertyInfo)(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType) PURE;
};

typedef HRESULT (WINAPI *CreateObjectFunc)(const GUID *clsID, const GUID *iid, void **outObject);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::wstring DirOf(const std::wstring &path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

static std::wstring BaseOf(const std::wstring &path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

static std::wstring StripExt(const std::wstring &name) {
    size_t p = name.find_last_of(L'.');
    return p == std::wstring::npos ? name : name.substr(0, p);
}

static std::wstring ExtOf(const std::wstring &name) {
    size_t p = name.find_last_of(L'.');
    if (p == std::wstring::npos) return L"";
    std::wstring e = name.substr(p);
    for (auto &c : e) c = (wchar_t)towlower(c);
    return e;
}

static std::wstring Lower(std::wstring s) {
    for (auto &c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::wstring Join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

// Create every missing directory along the path.
static void MakeDirs(const std::wstring &path) {
    if (path.empty()) return;
    std::wstring acc;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find_first_of(L"\\/", i);
        if (j == std::wstring::npos) j = path.size();
        acc = path.substr(0, j);
        if (acc.size() > 2 || (acc.size() == 2 && acc[1] != L':'))
            CreateDirectoryW(acc.c_str(), NULL);
        i = j + 1;
    }
}

// Delete everything inside `dir`, leaving the directory itself.
static void ClearDir(const std::wstring &dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = Join(dir, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ClearDir(full);
            RemoveDirectoryW(full.c_str());
        } else {
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(full.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Reject absolute paths and ".." escapes coming out of the archive, so a
// malicious archive cannot write outside the extraction directory (zip-slip).
static bool SafeRelativePath(const std::wstring &raw, std::wstring &out) {
    if (raw.empty()) return false;
    if (raw.size() >= 2 && raw[1] == L':') return false;
    if (raw[0] == L'\\' || raw[0] == L'/') return false;
    std::wstring cleaned;
    size_t i = 0;
    while (i < raw.size()) {
        size_t j = raw.find_first_of(L"\\/", i);
        if (j == std::wstring::npos) j = raw.size();
        std::wstring part = raw.substr(i, j - i);
        if (part == L".." ) return false;
        if (!part.empty() && part != L".") {
            if (!cleaned.empty()) cleaned += L"\\";
            cleaned += part;
        }
        i = j + 1;
    }
    if (cleaned.empty()) return false;
    out = cleaned;
    return true;
}

static bool RegDword(HKEY root, const wchar_t *sub, const wchar_t *name, DWORD *out) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, cb = sizeof(DWORD);
    bool ok = RegQueryValueExW(k, name, NULL, &type, (LPBYTE)out, &cb) == ERROR_SUCCESS
              && type == REG_DWORD;
    RegCloseKey(k);
    return ok;
}

// ---------------------------------------------------------------------------
// Console output
//
// This is a /SUBSYSTEM:WINDOWS binary so extraction shows no console window,
// which costs us stdout. For command-line use we attach to whatever console
// launched us and write there instead of popping dialogs.
// ---------------------------------------------------------------------------

enum {
    AZ_OK           = 0,
    AZ_FAILED       = 1,   // operation attempted but did not succeed
    AZ_USAGE        = 2,   // unknown or malformed arguments
    AZ_NOT_FOUND    = 3,   // archive path does not exist
    AZ_NO_ELEVATION = 4    // needed admin, did not get it
};

static bool   g_cli    = false;
static HANDLE g_conOut = INVALID_HANDLE_VALUE;
static HANDLE g_stdOut = INVALID_HANDLE_VALUE;   // captured BEFORE AttachConsole

static void ConsoleInit() {
    // Order matters. AttachConsole REPLACES the process std handles with the
    // console's, so a redirect set up by the parent (`--help > out.txt`) is lost
    // the moment we attach. Capture the inherited handle first and write to that.
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD type = GetFileType(h);
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) {
            g_stdOut = h;      // genuine redirection: keep it
            g_cli = true;
        } else if (type == FILE_TYPE_CHAR) {
            g_cli = true;      // already a console; CONOUT$ below handles it
        }
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_cli = true;
        g_conOut = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
    }
}

static void Emit(const std::wstring &text) {
    DWORD written = 0;

    // Redirected to a file or pipe: write UTF-8 bytes to the saved handle.
    if (g_stdOut != INVALID_HANDLE_VALUE) {
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                    NULL, 0, NULL, NULL);
        if (n > 0) {
            std::string buf((size_t)n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                &buf[0], n, NULL, NULL);
            WriteFile(g_stdOut, buf.data(), (DWORD)buf.size(), &written, NULL);
        }
        return;
    }

    if (g_conOut != INVALID_HANDLE_VALUE) {
        WriteConsoleW(g_conOut, text.c_str(), (DWORD)text.size(), &written, NULL);
        return;
    }

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE)
        WriteConsoleW(h, text.c_str(), (DWORD)text.size(), &written, NULL);
}

// A Store/MSIX package owns install, uninstall, and file-type association for
// its own app -- it declares associations in AppxManifest.xml and Windows
// registers them without help. The self-elevating registry-writing install
// path below is for the portable exe only; running it from a package would
// mean a Store app silently asking for admin rights, which is not allowed.
static bool IsPackaged() {
    UINT32 len = 0;
    return GetCurrentPackageFullName(&len, NULL) != APPMODEL_ERROR_NO_PACKAGE;
}

// ---------------------------------------------------------------------------
// UI state, shared between the worker thread and the window
// ---------------------------------------------------------------------------

#define WM_AZ_PROGRESS (WM_APP + 1)
#define WM_AZ_STATUS   (WM_APP + 2)
#define WM_AZ_DONE     (WM_APP + 3)
#define WM_AZ_PASSWORD (WM_APP + 4)   // sent (blocking) from worker

static HWND         g_hwnd     = NULL;
static HINSTANCE    g_inst     = NULL;
static HFONT        g_font     = NULL;
static int          g_percent  = 0;
static std::wstring g_status   = L"Initializing...";
static bool         g_light    = false;
static COLORREF     g_bg, g_fg, g_track, g_accent, g_border, g_surface;
static HBRUSH       g_bgBrush = NULL;      // dialog background
static HBRUSH       g_surfaceBrush = NULL; // edit-field background
static std::wstring g_archive;
static std::wstring g_error;
static std::wstring g_outName;   // folder the archive landed in, shown on completion
static std::wstring g_curFile;   // entry currently being written
static UInt64       g_bytesDone  = 0;
static UInt64       g_bytesTotal = 0;
static HFONT        g_fontSmall  = NULL;

// g_status and g_curFile are written by the worker and read while painting.
static CRITICAL_SECTION g_textLock;

static std::wstring ReadShared(const std::wstring &src) {
    EnterCriticalSection(&g_textLock);
    std::wstring copy = src;
    LeaveCriticalSection(&g_textLock);
    return copy;
}

static void WriteShared(std::wstring &dst, const std::wstring &value) {
    EnterCriticalSection(&g_textLock);
    dst = value;
    LeaveCriticalSection(&g_textLock);
}

// Both figures share the larger value's unit so they stay comparable.
static std::wstring FormatBytesPair(UInt64 done, UInt64 total) {
    const wchar_t *unit = L"B";
    double div = 1.0;
    if (total >= 1024ULL * 1024 * 1024) { unit = L"GB"; div = 1024.0 * 1024 * 1024; }
    else if (total >= 1024 * 1024)      { unit = L"MB"; div = 1024.0 * 1024; }
    else if (total >= 1024)             { unit = L"KB"; div = 1024.0; }

    wchar_t buf[128];
    if (div == 1.0)
        swprintf_s(buf, L"%llu / %llu %s", (unsigned long long)done,
                   (unsigned long long)total, unit);
    else
        swprintf_s(buf, L"%.1f / %.1f %s", done / div, total / div, unit);
    return buf;
}

static void ReadTheme() {
    DWORD v = 0;
    g_light = RegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", &v) && v == 1;

    g_bg      = g_light ? RGB(248, 248, 248) : RGB(32, 32, 32);
    g_fg      = g_light ? RGB(20, 20, 20)    : RGB(240, 240, 240);
    g_track   = g_light ? RGB(230, 230, 230) : RGB(60, 60, 60);
    g_border  = g_light ? RGB(210, 210, 210) : RGB(60, 60, 60);
    g_surface = g_light ? RGB(255, 255, 255) : RGB(45, 45, 45);

    g_accent = RGB(0, 120, 215);
    DWORD abgr = 0;
    if (RegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                 L"AccentColor", &abgr)) {
        // Stored ABGR; COLORREF is 0x00BBGGRR, so the low three bytes already line up.
        g_accent = RGB(abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF);
    }

    // Rebuilt on every read so a live theme switch swaps these too.
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_surfaceBrush) DeleteObject(g_surfaceBrush);
    g_bgBrush      = CreateSolidBrush(g_bg);
    g_surfaceBrush = CreateSolidBrush(g_surface);
}

// Win10 2004+ uses attribute 20; earlier builds used 19. Try both.
static void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = g_light ? FALSE : TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

// Ask the common controls to render with their dark variants.
static void ApplyControlTheme(HWND ctl, bool isEdit) {
    SetWindowTheme(ctl, g_light ? NULL : (isEdit ? L"DarkMode_CFD" : L"DarkMode_Explorer"), NULL);
}

// Completion chime. Playing the system event by ALIAS is what makes this follow
// the user's Sound scheme: the alias resolves through HKCU\AppEvents, so picking
// "No Sounds" (or unassigning .Default for this event) silences it. SND_NODEFAULT
// is the load-bearing flag -- without it an unassigned event falls back to the
// stock beep, which would ignore the profile entirely.
static void PlayDoneSound() {
    PlaySoundW(L"SystemAsterisk", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}

// True when the message signals that the light/dark setting may have changed.
static bool IsThemeChange(UINT msg, LPARAM lp) {
    if (msg != WM_SETTINGCHANGE || !lp) return false;
    return _wcsicmp((const wchar_t *)lp, L"ImmersiveColorSet") == 0;
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

class FileInStream : public IInStream {
    LONG  m_ref;
    HANDLE m_h;
public:
    FileInStream() : m_ref(1), m_h(INVALID_HANDLE_VALUE) {}
    ~FileInStream() { if (m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h); }

    bool Open(const std::wstring &path) {
        m_h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        return m_h != INVALID_HANDLE_VALUE;
    }

    STDMETHODIMP QueryInterface(REFIID iid, void **out) {
        if (iid == IID_IUnknown || iid == IID_ISequentialInStream || iid == IID_IInStream) {
            *out = static_cast<IInStream *>(this);
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP Read(void *data, UInt32 size, UInt32 *processed) {
        DWORD got = 0;
        BOOL ok = ReadFile(m_h, data, size, &got, NULL);
        if (processed) *processed = got;
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    STDMETHODIMP Seek(Int64 offset, UInt32 origin, UInt64 *newPos) {
        LARGE_INTEGER li; li.QuadPart = offset;
        LARGE_INTEGER out;
        if (!SetFilePointerEx(m_h, li, &out, origin))
            return HRESULT_FROM_WIN32(GetLastError());
        if (newPos) *newPos = (UInt64)out.QuadPart;
        return S_OK;
    }
};

// A numbered volume set (rand.7z.001, .002, ...) presented as one continuous
// stream. 7-Zip routes these through its separate "Split" handler, which needs
// a two-stage open; spanning the files ourselves is smaller and lets the normal
// format handler see exactly what it expects.
//
// Note this is only for the .NNN convention. Multi-part RAR (name.part1.rar)
// still goes through IArchiveOpenVolumeCallback on OpenCallback.
class MultiFileInStream : public IInStream {
    LONG m_ref;
    struct Vol { HANDLE h; UInt64 size; UInt64 start; };
    std::vector<Vol> m_vols;
    UInt64 m_total;
    UInt64 m_pos;
public:
    MultiFileInStream() : m_ref(1), m_total(0), m_pos(0) {}
    ~MultiFileInStream() {
        for (size_t i = 0; i < m_vols.size(); ++i)
            if (m_vols[i].h != INVALID_HANDLE_VALUE) CloseHandle(m_vols[i].h);
    }

    // stem is the path without the ".NNN" suffix; width is the digit count.
    bool Open(const std::wstring &stem, int width) {
        for (int i = 1; ; ++i) {
            // wsprintfW supports only a small format subset -- no "*" width -- so
            // pad by hand rather than with "%0*d", which silently emits garbage.
            wchar_t num[16];
            wsprintfW(num, L"%d", i);
            std::wstring digits = num;
            while ((int)digits.size() < width) digits.insert(digits.begin(), L'0');
            std::wstring vol = stem + L"." + digits;
            HANDLE h = CreateFileW(vol.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) break;
            LARGE_INTEGER sz;
            if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); break; }
            Vol v; v.h = h; v.size = (UInt64)sz.QuadPart; v.start = m_total;
            m_vols.push_back(v);
            m_total += v.size;
        }
        return !m_vols.empty();
    }

    STDMETHODIMP QueryInterface(REFIID iid, void **out) {
        if (iid == IID_IUnknown || iid == IID_ISequentialInStream || iid == IID_IInStream) {
            *out = static_cast<IInStream *>(this);
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // Short reads are legal, so one volume per call keeps this simple.
    STDMETHODIMP Read(void *data, UInt32 size, UInt32 *processed) {
        if (processed) *processed = 0;
        if (m_pos >= m_total || size == 0) return S_OK;

        size_t i = 0;
        while (i < m_vols.size() && m_pos >= m_vols[i].start + m_vols[i].size) ++i;
        if (i >= m_vols.size()) return S_OK;

        UInt64 offsetInVol = m_pos - m_vols[i].start;
        UInt64 avail = m_vols[i].size - offsetInVol;
        if (size > avail) size = (UInt32)avail;

        LARGE_INTEGER li; li.QuadPart = (LONGLONG)offsetInVol;
        if (!SetFilePointerEx(m_vols[i].h, li, NULL, FILE_BEGIN))
            return HRESULT_FROM_WIN32(GetLastError());

        DWORD got = 0;
        if (!ReadFile(m_vols[i].h, data, size, &got, NULL))
            return HRESULT_FROM_WIN32(GetLastError());

        m_pos += got;
        if (processed) *processed = got;
        return S_OK;
    }

    STDMETHODIMP Seek(Int64 offset, UInt32 origin, UInt64 *newPos) {
        Int64 base = (origin == FILE_BEGIN)   ? 0
                   : (origin == FILE_CURRENT) ? (Int64)m_pos
                   : (origin == FILE_END)     ? (Int64)m_total
                   : -1;
        if (base < 0) return E_INVALIDARG;
        Int64 target = base + offset;
        if (target < 0) return E_INVALIDARG;
        m_pos = (UInt64)target;
        if (newPos) *newPos = m_pos;
        return S_OK;
    }
};

class FileOutStream : public ISequentialOutStream {
    LONG   m_ref;
    HANDLE m_h;
public:
    FileOutStream() : m_ref(1), m_h(INVALID_HANDLE_VALUE) {}
    ~FileOutStream() { if (m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h); }

    bool Create(const std::wstring &path) {
        m_h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        return m_h != INVALID_HANDLE_VALUE;
    }

    STDMETHODIMP QueryInterface(REFIID iid, void **out) {
        if (iid == IID_IUnknown || iid == IID_ISequentialOutStream) {
            *out = static_cast<ISequentialOutStream *>(this);
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP Write(const void *data, UInt32 size, UInt32 *processed) {
        DWORD put = 0;
        BOOL ok = WriteFile(m_h, data, size, &put, NULL);
        if (processed) *processed = put;
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
};

// ---------------------------------------------------------------------------
// Password prompt (worker thread asks the UI thread via SendMessage)
// ---------------------------------------------------------------------------

static bool         g_pwCancelled = false;
static std::wstring g_password;
static bool         g_havePassword = false;

static bool AskPasswordOnUiThread();   // fwd

static bool RequestPassword() {
    if (!g_hwnd) return false;
    return SendMessageW(g_hwnd, WM_AZ_PASSWORD, 0, 0) != 0;
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

// Handles both the plain open and multi-volume sets (.7z.001, .zip.001, ...).
class OpenCallback : public IArchiveOpenCallback,
                     public IArchiveOpenVolumeCallback,
                     public ICryptoGetTextPassword {
    LONG m_ref;
    std::wstring m_dir;
    std::wstring m_name;
public:
    OpenCallback(const std::wstring &archivePath)
        : m_ref(1), m_dir(DirOf(archivePath)), m_name(BaseOf(archivePath)) {}

    STDMETHODIMP QueryInterface(REFIID iid, void **out) {
        if (iid == IID_IUnknown || iid == IID_IArchiveOpenCallback) {
            *out = static_cast<IArchiveOpenCallback *>(this);
        } else if (iid == IID_IArchiveOpenVolumeCallback) {
            *out = static_cast<IArchiveOpenVolumeCallback *>(this);
        } else if (iid == IID_ICryptoGetTextPassword) {
            *out = static_cast<ICryptoGetTextPassword *>(this);
        } else {
            *out = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // IArchiveOpenCallback
    STDMETHODIMP SetTotal(const UInt64 *, const UInt64 *)     { return S_OK; }
    STDMETHODIMP SetCompleted(const UInt64 *, const UInt64 *) { return S_OK; }

    // IArchiveOpenVolumeCallback -- lets 7-Zip walk to the next volume itself.
    STDMETHODIMP GetProperty(PROPID propID, PROPVARIANT *value) {
        PropVariantInit(value);
        if (propID == kpidName) {
            value->vt = VT_BSTR;
            value->bstrVal = SysAllocString(m_name.c_str());
            return value->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        return S_OK;
    }

    STDMETHODIMP GetStream(const wchar_t *name, IInStream **inStream) {
        *inStream = NULL;
        FileInStream *s = new FileInStream();
        if (!s->Open(Join(m_dir, name))) {
            s->Release();
            return S_FALSE;   // no such volume: 7-Zip stops asking
        }
        *inStream = s;
        return S_OK;
    }

    // ICryptoGetTextPassword
    STDMETHODIMP CryptoGetTextPassword(BSTR *password) {
        if (!g_havePassword && !RequestPassword()) return E_ABORT;
        *password = SysAllocString(g_password.c_str());
        return *password ? S_OK : E_OUTOFMEMORY;
    }
};

class ExtractCallback : public IArchiveExtractCallback,
                        public ICryptoGetTextPassword {
    LONG m_ref;
    IInArchive  *m_archive;
    std::wstring m_outDir;
    std::wstring m_fallbackName;   // used when an entry carries no stored path
    UInt64       m_total;
    FileOutStream *m_current;
    bool         m_currentIsDir;
public:
    bool         failed;
    std::wstring failMessage;

    ExtractCallback(IInArchive *a, const std::wstring &outDir, const std::wstring &fallbackName)
        : m_ref(1), m_archive(a), m_outDir(outDir), m_fallbackName(fallbackName),
          m_total(0), m_current(NULL), m_currentIsDir(false), failed(false) {}

    STDMETHODIMP QueryInterface(REFIID iid, void **out) {
        if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback) {
            *out = static_cast<IArchiveExtractCallback *>(this);
        } else if (iid == IID_ICryptoGetTextPassword) {
            *out = static_cast<ICryptoGetTextPassword *>(this);
        } else {
            *out = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // IProgress
    STDMETHODIMP SetTotal(UInt64 total) {
        m_total = total;
        g_bytesTotal = total;
        return S_OK;
    }

    STDMETHODIMP SetCompleted(const UInt64 *value) {
        if (value && m_total) {
            g_bytesDone = *value;
            int pct = (int)((*value) * 100 / m_total);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            if (pct != g_percent) {
                g_percent = pct;
                if (g_hwnd) PostMessageW(g_hwnd, WM_AZ_PROGRESS, (WPARAM)pct, 0);
            }
        }
        return S_OK;
    }

    STDMETHODIMP GetStream(UInt32 index, ISequentialOutStream **outStream, Int32 askMode) {
        *outStream = NULL;
        m_current = NULL;
        m_currentIsDir = false;
        if (askMode != kExtract) return S_OK;

        // Single-stream compressors (.gz, .bz2, .xz, .lzma) usually store no
        // name at all, so fall back to the archive name minus its extension.
        PROPVARIANT prop;
        PropVariantInit(&prop);
        std::wstring raw;
        if (m_archive->GetProperty(index, kpidPath, &prop) == S_OK && prop.vt == VT_BSTR)
            raw.assign(prop.bstrVal, SysStringLen(prop.bstrVal));
        PropVariantClear(&prop);
        if (raw.empty()) raw = m_fallbackName;

        std::wstring rel;
        if (!SafeRelativePath(raw, rel)) return S_OK;   // skip, do not abort the run

        PropVariantInit(&prop);
        bool isDir = m_archive->GetProperty(index, kpidIsDir, &prop) == S_OK
                     && prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE;
        PropVariantClear(&prop);

        std::wstring full = Join(m_outDir, rel);
        if (isDir) {
            MakeDirs(full);
            m_currentIsDir = true;
            return S_OK;
        }

        WriteShared(g_curFile, BaseOf(rel));
        if (g_hwnd) PostMessageW(g_hwnd, WM_AZ_STATUS, 0, 0);

        MakeDirs(DirOf(full));
        FileOutStream *s = new FileOutStream();
        if (!s->Create(full)) {
            s->Release();
            failed = true;
            failMessage = L"Cannot create: " + full;
            return E_FAIL;
        }
        m_current = s;
        *outStream = s;
        return S_OK;
    }

    STDMETHODIMP PrepareOperation(Int32) { return S_OK; }

    STDMETHODIMP SetOperationResult(Int32 opRes) {
        m_current = NULL;
        if (opRes != kOpOK && !m_currentIsDir) {
            failed = true;
            if (failMessage.empty()) {
                failMessage = (opRes == kOpCRCError)  ? L"CRC error - archive is damaged."
                            : (opRes == kOpDataError) ? L"Data error - archive is damaged or the password is wrong."
                            : (opRes == kOpUnsupportedMethod) ? L"Unsupported compression method."
                            : L"Extraction failed.";
            }
        }
        return S_OK;
    }

    // ICryptoGetTextPassword
    STDMETHODIMP CryptoGetTextPassword(BSTR *password) {
        if (!g_havePassword && !RequestPassword()) return E_ABORT;
        *password = SysAllocString(g_password.c_str());
        return *password ? S_OK : E_OUTOFMEMORY;
    }
};

// ---------------------------------------------------------------------------
// Loading the embedded 7-Zip engine
// ---------------------------------------------------------------------------

static HMODULE          g_7z = NULL;
static CreateObjectFunc g_createObject = NULL;

static std::wstring TempDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (!n) return L".";
    return Join(std::wstring(buf, n), L"AirZip");
}

static std::wstring SelfPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    return std::wstring(buf, n);
}

// Writes the resource out under a scratch name and renames it, so an
// interrupted first run cannot leave a truncated 7z.dll that every later run
// would then trust.
static bool UnpackSevenZip(const std::wstring &dest) {
    // RT_RCDATA is MAKEINTRESOURCE(10), i.e. char-typed; the W call needs the wide form.
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_SEVENZIP), (LPCWSTR)RT_RCDATA);
    if (!res) return false;
    HGLOBAL h = LoadResource(NULL, res);
    if (!h) return false;
    const void *data = LockResource(h);
    DWORD size = SizeofResource(NULL, res);
    if (!data || !size) return false;

    wchar_t pid[32];
    wsprintfW(pid, L".%u.tmp", GetCurrentProcessId());
    std::wstring staging = dest + pid;

    HANDLE f = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(f, data, size, &put, NULL) && put == size;
    CloseHandle(f);
    if (!ok) { DeleteFileW(staging.c_str()); return false; }

    if (!MoveFileExW(staging.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // Another instance won the race and already put a good copy there.
        DeleteFileW(staging.c_str());
        return GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    return true;
}

static bool LoadSevenZip() {
    if (g_createObject) return true;

    // A 7z.dll sitting next to the exe wins, so a newer engine can be dropped in.
    std::wstring beside = Join(DirOf(SelfPath()), L"7z.dll");
    if (GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES)
        g_7z = LoadLibraryW(beside.c_str());

    if (!g_7z) {
        std::wstring dir = TempDir();
        CreateDirectoryW(dir.c_str(), NULL);
        std::wstring dll = Join(dir, L"7z.dll");
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
            UnpackSevenZip(dll);
        g_7z = LoadLibraryW(dll.c_str());
    }
    if (!g_7z) return false;

    g_createObject = (CreateObjectFunc)GetProcAddress(g_7z, "CreateObject");
    return g_createObject != NULL;
}

// Extension -> handler. Split volumes keep the inner extension (rand.7z.001),
// so look past a trailing numeric part too.
static const GUID *GuessFormat(const std::wstring &path) {
    std::wstring name = Lower(BaseOf(path));
    std::wstring ext  = ExtOf(name);

    // rand.7z.001 -> treat as .7z and let the volume callback stitch the parts.
    if (ext.size() >= 2 && iswdigit(ext[1])) {
        std::wstring inner = ExtOf(StripExt(name));
        if (!inner.empty()) ext = inner;
    }

    if (ext == L".7z")   return &CLSID_SevenZip;
    if (ext == L".zip" || ext == L".zipx" || ext == L".jar" ||
        ext == L".apk" || ext == L".docx" || ext == L".xlsx" || ext == L".pptx")
                         return &CLSID_Zip;
    if (ext == L".rar")  return &CLSID_Rar5;   // Rar5 handler also opens rar4
    if (ext == L".tar")  return &CLSID_Tar;
    if (ext == L".gz" || ext == L".tgz")  return &CLSID_GZip;
    if (ext == L".bz2" || ext == L".tbz") return &CLSID_BZip2;
    if (ext == L".xz")   return &CLSID_Xz;
    if (ext == L".iso")  return &CLSID_Iso;
    if (ext == L".cab")  return &CLSID_Cab;
    if (ext == L".wim")  return &CLSID_Wim;
    if (ext == L".lzma") return &CLSID_Lzma;
    if (ext == L".arj")  return &CLSID_Arj;
    if (ext == L".lzh" || ext == L".lha") return &CLSID_Lzh;
    return NULL;
}

// Tried in order when the extension does not settle it, or lied.
static const GUID *const kAllFormats[] = {
    &CLSID_SevenZip, &CLSID_Zip, &CLSID_Rar5, &CLSID_Rar, &CLSID_Tar,
    &CLSID_GZip, &CLSID_BZip2, &CLSID_Xz, &CLSID_Iso, &CLSID_Cab,
    &CLSID_Wim, &CLSID_Lzma, &CLSID_Arj, &CLSID_Lzh
};

// Recognises the ".NNN" volume convention: rand.7z.001 -> stem "rand.7z", width 3.
static bool SplitVolumeInfo(const std::wstring &path, std::wstring &stem, int &width) {
    std::wstring name = BaseOf(path);
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return false;
    std::wstring digits = name.substr(dot + 1);
    if (digits.size() < 2 || digits.size() > 4) return false;
    for (size_t i = 0; i < digits.size(); ++i)
        if (!iswdigit(digits[i])) return false;
    stem  = path.substr(0, path.size() - digits.size() - 1);
    width = (int)digits.size();
    return true;
}

// Spanning stream for numbered volume sets, plain file otherwise.
static IInStream *MakeStream(const std::wstring &path) {
    std::wstring stem;
    int width = 0;
    if (SplitVolumeInfo(path, stem, width)) {
        MultiFileInStream *m = new MultiFileInStream();
        if (m->Open(stem, width)) return m;
        m->Release();
    }
    FileInStream *f = new FileInStream();
    if (f->Open(path)) return f;
    f->Release();
    return NULL;
}

// Opens with `fmt`; caller owns the returned archive.
static IInArchive *TryOpen(const GUID *fmt, const std::wstring &path, HRESULT *hrOut) {
    IInArchive *archive = NULL;
    if (g_createObject(fmt, &IID_IInArchive, (void **)&archive) != S_OK || !archive)
        return NULL;

    IInStream *stream = MakeStream(path);
    if (!stream) {
        archive->Release();
        return NULL;
    }

    OpenCallback *cb = new OpenCallback(path);
    const UInt64 scanLimit = 1 << 23;
    HRESULT hr = archive->Open(stream, &scanLimit, cb);
    if (hrOut) *hrOut = hr;

    stream->Release();
    cb->Release();

    if (hr != S_OK) {
        archive->Release();
        return NULL;
    }
    return archive;
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

static void SetStatus(const std::wstring &text) {
    WriteShared(g_status, text);
    if (g_hwnd) PostMessageW(g_hwnd, WM_AZ_STATUS, 0, 0);
}

static bool ExtractArchive(const std::wstring &archivePath, std::wstring &error) {
    if (!LoadSevenZip()) {
        error = L"Could not load the 7-Zip engine.";
        return false;
    }

    IInArchive *archive = NULL;
    HRESULT hr = S_OK;

    const GUID *guess = GuessFormat(archivePath);
    if (guess) archive = TryOpen(guess, archivePath, &hr);

    if (!archive) {
        for (size_t i = 0; i < ARRAYSIZE(kAllFormats) && !archive; ++i) {
            if (guess == kAllFormats[i]) continue;
            archive = TryOpen(kAllFormats[i], archivePath, &hr);
        }
    }

    // No password retry here on purpose: when a handler needs one it calls
    // ICryptoGetTextPassword on OpenCallback during Open, so the prompt already
    // happens at the right moment. Retrying on every open failure would pop a
    // password box for merely damaged or unsupported files.
    if (!archive) {
        error = L"Not a supported archive, or the file is damaged.";
        return false;
    }

    UInt32 count = 0;
    if (archive->GetNumberOfItems(&count) != S_OK) {
        archive->Close();
        archive->Release();
        error = L"Could not read the archive contents.";
        return false;
    }

    // Name to use for entries that carry no stored path, e.g. the payload of a
    // .gz / .bz2 / .xz: "archive.tar.gz" -> "archive.tar".
    std::wstring fallbackName = StripExt(BaseOf(archivePath));
    if (fallbackName.empty()) fallbackName = L"extracted";

    // Smart folder: how many distinct items sit at the archive root?
    std::set<std::wstring> roots;
    for (UInt32 i = 0; i < count && roots.size() < 2; ++i) {
        PROPVARIANT prop;
        PropVariantInit(&prop);
        std::wstring p;
        if (archive->GetProperty(i, kpidPath, &prop) == S_OK && prop.vt == VT_BSTR)
            p.assign(prop.bstrVal, SysStringLen(prop.bstrVal));
        PropVariantClear(&prop);

        if (p.empty()) p = fallbackName;   // counts as one root, not zero
        size_t cut = p.find_first_of(L"\\/");
        std::wstring root = (cut == std::wstring::npos) ? p : p.substr(0, cut);
        if (!root.empty()) roots.insert(Lower(root));
    }

    std::wstring parent = DirOf(archivePath);
    std::wstring outDir;
    if (roots.size() == 1) {
        outDir = parent;
    } else {
        // Strip the volume suffix and then the format suffix: rand.7z.001 -> rand
        std::wstring stem = StripExt(BaseOf(archivePath));
        std::wstring low = Lower(stem);
        if (low.size() > 3) {
            std::wstring innerExt = ExtOf(low);
            if (innerExt == L".7z" || innerExt == L".zip" || innerExt == L".rar" ||
                innerExt == L".tar")
                stem = StripExt(stem);
        }
        if (stem.empty()) stem = L"Extracted";
        outDir = Join(parent, stem);
        MakeDirs(outDir);
    }

    g_outName = BaseOf(outDir);
    SetStatus(L"Extracting to " + g_outName + L"...");

    ExtractCallback *cb = new ExtractCallback(archive, outDir, fallbackName);
    hr = archive->Extract(NULL, (UInt32)(Int32)(-1), 0, cb);
    bool failed = cb->failed;
    std::wstring failMsg = cb->failMessage;
    cb->Release();

    archive->Close();
    archive->Release();

    if (hr == E_ABORT) return false;          // password cancelled
    if (hr != S_OK || failed) {
        error = failMsg.empty() ? L"Extraction failed." : failMsg;
        return false;
    }
    return true;
}

static DWORD WINAPI WorkerThread(LPVOID) {
    std::wstring error;
    bool ok = ExtractArchive(g_archive, error);
    if (!ok && !error.empty()) g_error = error;
    if (g_hwnd) PostMessageW(g_hwnd, WM_AZ_DONE, ok ? 1 : 0, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Password dialog -- a small popup with its own modal loop
// ---------------------------------------------------------------------------

static HWND g_pwEdit = NULL;
static bool g_pwOk   = false;
static bool g_pwDone = false;

static LRESULT CALLBACK PwProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_bgBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, g_fg);
        SetBkColor((HDC)wp, g_bg);
        return (LRESULT)g_bgBrush;

    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, g_fg);
        SetBkColor((HDC)wp, g_surface);
        return (LRESULT)g_surfaceBrush;

    case WM_SETTINGCHANGE:
        if (IsThemeChange(msg, lp)) {
            ReadTheme();
            ApplyDarkTitleBar(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[512] = {0};
            GetWindowTextW(g_pwEdit, buf, ARRAYSIZE(buf));
            g_password = buf;
            g_pwOk = true;
            g_pwDone = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            g_pwOk = false;
            g_pwDone = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        g_pwOk = false;
        g_pwDone = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_pwDone = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool AskPasswordOnUiThread() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = PwProc;
        wc.hInstance     = g_inst;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;   // painted in WM_ERASEBKGND so it follows the theme
        wc.lpszClassName = L"AirZipPassword";
        wc.hIcon         = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassExW(&wc);
        registered = true;
    }

    int w = 320, h = 150;
    RECT sr; SystemParametersInfoW(SPI_GETWORKAREA, 0, &sr, 0);
    int x = sr.left + ((sr.right - sr.left) - w) / 2;
    int y = sr.top + ((sr.bottom - sr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"AirZipPassword",
        L"Password Required", WS_POPUPWINDOW | WS_CAPTION,
        x, y, w, h, g_hwnd, NULL, g_inst, NULL);
    if (!dlg) return false;

    ApplyDarkTitleBar(dlg);

    // DEFAULT_GUI_FONT is the ancient bitmap face; match the main window instead.
    UINT dlgDpi = GetDpiForWindow(dlg);
    HFONT uiFont = CreateFontW(-MulDiv(9, dlgDpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    HWND lbl = CreateWindowExW(0, L"STATIC", L"Enter password:", WS_CHILD | WS_VISIBLE,
        16, 14, 260, 20, dlg, NULL, g_inst, NULL);
    g_pwEdit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
        16, 38, 282, 24, dlg, NULL, g_inst, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        198, 74, 100, 26, dlg, (HMENU)IDOK, g_inst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        90, 74, 100, 26, dlg, (HMENU)IDCANCEL, g_inst, NULL);

    SendMessageW(lbl, WM_SETFONT, (WPARAM)uiFont, TRUE);
    SendMessageW(g_pwEdit, WM_SETFONT, (WPARAM)uiFont, TRUE);
    SendMessageW(ok, WM_SETFONT, (WPARAM)uiFont, TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)uiFont, TRUE);

    ApplyControlTheme(g_pwEdit, true);
    ApplyControlTheme(ok, false);
    ApplyControlTheme(cancel, false);

    g_pwOk = false;
    g_pwDone = false;
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(g_pwEdit);

    MSG msg;
    while (!g_pwDone && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    g_pwEdit = NULL;
    DeleteObject(uiFont);

    if (g_pwOk) g_havePassword = true;
    return g_pwOk;
}

// ---------------------------------------------------------------------------
// Themed message box
//
// The stock MessageBox is not dark-mode aware -- it renders light grey no matter
// the system theme -- so alerts get the same treatment as the password prompt.
// It also means we must play the event sound ourselves, which MessageBox used to
// do for us; SND_NODEFAULT keeps that honouring the user's sound scheme.
// ---------------------------------------------------------------------------

enum { AZ_BTN_OK = 0, AZ_BTN_YESNO = 1 };

static int  g_msgResult = IDOK;
static bool g_msgDone   = false;

static LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_bgBrush);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, g_fg);
        SetBkColor((HDC)wp, g_bg);
        return (LRESULT)g_bgBrush;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK || id == IDYES || id == IDNO || id == IDCANCEL) {
            g_msgResult = (id == IDCANCEL) ? IDNO : id;
            g_msgDone = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        g_msgDone = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_msgDone = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int ThemedMessage(HWND parent, const std::wstring &title,
                         const std::wstring &text, int buttons, bool isError) {
    ReadTheme();

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = MsgProc;
        wc.hInstance     = g_inst;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"AirZipMessage";
        wc.hIcon         = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassExW(&wc);
        registered = true;
    }

    UINT dpi = GetDpiForSystem();
    HFONT font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    // Measure the text so the window fits it.
    int maxTextW = MulDiv(420, dpi, 96);
    RECT calc = { 0, 0, maxTextW, 0 };
    HDC screen = GetDC(NULL);
    HFONT oldF = (HFONT)SelectObject(screen, font);
    DrawTextW(screen, text.c_str(), -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
    SelectObject(screen, oldF);
    ReleaseDC(NULL, screen);

    int pad    = MulDiv(18, dpi, 96);
    int btnW   = MulDiv(96, dpi, 96);
    int btnH   = MulDiv(28, dpi, 96);
    int textW  = (calc.right  < MulDiv(260, dpi, 96)) ? MulDiv(260, dpi, 96) : calc.right;
    int textH  = calc.bottom;
    int clientW = textW + pad * 2;
    int clientH = pad + textH + MulDiv(18, dpi, 96) + btnH + pad;

    RECT wr = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&wr, WS_POPUPWINDOW | WS_CAPTION, FALSE, WS_EX_DLGMODALFRAME);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;

    RECT sr; SystemParametersInfoW(SPI_GETWORKAREA, 0, &sr, 0);
    int x = sr.left + ((sr.right - sr.left) - w) / 2;
    int y = sr.top + ((sr.bottom - sr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"AirZipMessage",
        title.c_str(), WS_POPUPWINDOW | WS_CAPTION, x, y, w, h, parent, NULL, g_inst, NULL);
    if (!dlg) {
        DeleteObject(font);
        // Last resort so the message is never silently swallowed.
        return MessageBoxW(parent, text.c_str(), title.c_str(),
                           (buttons == AZ_BTN_YESNO ? MB_YESNO : MB_OK) |
                           (isError ? MB_ICONERROR : MB_ICONINFORMATION));
    }

    ApplyDarkTitleBar(dlg);

    HWND lbl = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, pad, textW, textH, dlg, NULL, g_inst, NULL);
    SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);

    int btnY = pad + textH + MulDiv(18, dpi, 96);
    HWND b1, b2 = NULL;
    if (buttons == AZ_BTN_YESNO) {
        b1 = CreateWindowExW(0, L"BUTTON", L"Yes",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            clientW - pad - btnW, btnY, btnW, btnH, dlg, (HMENU)IDYES, g_inst, NULL);
        b2 = CreateWindowExW(0, L"BUTTON", L"No",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            clientW - pad - btnW * 2 - MulDiv(8, dpi, 96), btnY, btnW, btnH,
            dlg, (HMENU)IDNO, g_inst, NULL);
    } else {
        b1 = CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            clientW - pad - btnW, btnY, btnW, btnH, dlg, (HMENU)IDOK, g_inst, NULL);
    }
    SendMessageW(b1, WM_SETFONT, (WPARAM)font, TRUE);
    ApplyControlTheme(b1, false);
    if (b2) { SendMessageW(b2, WM_SETFONT, (WPARAM)font, TRUE); ApplyControlTheme(b2, false); }

    PlaySoundW(isError ? L"SystemHand" : L"SystemAsterisk", NULL,
               SND_ALIAS | SND_ASYNC | SND_NODEFAULT);

    g_msgResult = (buttons == AZ_BTN_YESNO) ? IDNO : IDOK;
    g_msgDone = false;
    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(b1);

    MSG m;
    while (!g_msgDone && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (parent) { EnableWindow(parent, TRUE); SetForegroundWindow(parent); }
    DeleteObject(font);
    return g_msgResult;
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------

#define TIMER_DONE 1
#define TIMER_SHOW 2

// Most extractions finish in well under a second. Showing a window for those is
// just a flash, so the window stays hidden until the work is still running after
// this long.
static const UINT kShowDelayMs = 400;
static bool g_shown = false;

static void RevealWindow(HWND hwnd) {
    if (g_shown) return;
    g_shown = true;
    // SHOWNOACTIVATE: appearing mid-task must not steal focus from the user.
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
}

static void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);

    // Everything is drawn into an off-screen bitmap and blitted once. Painting
    // the background, text and bar straight onto the window let the half-drawn
    // frame reach the screen, which shows up as flicker on archives with many
    // entries, where progress ticks arrive back to back.
    HDC     mem    = CreateCompatibleDC(dc);
    HBITMAP bmp    = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(g_bg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    UINT dpi = GetDpiForWindow(hwnd);
    int pad   = MulDiv(14, dpi, 96);
    int barY  = MulDiv(40, dpi, 96);
    int barH  = MulDiv(8, dpi, 96);
    int barW  = rc.right - pad * 2;
    int row2Y = barY + barH + MulDiv(8, dpi, 96);

    SetBkMode(mem, TRANSPARENT);

    std::wstring status  = ReadShared(g_status);
    std::wstring curFile = ReadShared(g_curFile);

    // Row 1: name on the left, percentage hard right.
    HFONT old = (HFONT)SelectObject(mem, g_font);
    wchar_t pctText[16];
    swprintf_s(pctText, L"%d%%", g_percent);

    SIZE pctSize = { 0, 0 };
    GetTextExtentPoint32W(mem, pctText, (int)wcslen(pctText), &pctSize);

    int row1Y = MulDiv(11, dpi, 96);
    int row1H = MulDiv(20, dpi, 96);

    SetTextColor(mem, g_fg);
    RECT nameRect = { pad, row1Y, rc.right - pad - pctSize.cx - MulDiv(10, dpi, 96),
                      row1Y + row1H };
    // Before any entry is opened there is nothing to name, so show the phase.
    const std::wstring &row1Left = curFile.empty() ? status : curFile;
    DrawTextW(mem, row1Left.c_str(), -1, &nameRect,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_PATH_ELLIPSIS);

    RECT pctRect = { rc.right - pad - pctSize.cx, row1Y, rc.right - pad, row1Y + row1H };
    DrawTextW(mem, pctText, -1, &pctRect, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    SelectObject(mem, old);

    // Row 2: destination on the left, bytes on the right, in the quieter font.
    HFONT oldSmall = (HFONT)SelectObject(mem, g_fontSmall ? g_fontSmall : g_font);
    COLORREF dim = g_light ? RGB(110, 110, 110) : RGB(160, 160, 160);
    SetTextColor(mem, dim);

    int row2H = MulDiv(16, dpi, 96);
    if (g_bytesTotal > 0) {
        std::wstring bytes = FormatBytesPair(g_bytesDone, g_bytesTotal);
        SIZE bs = { 0, 0 };
        GetTextExtentPoint32W(mem, bytes.c_str(), (int)bytes.size(), &bs);
        RECT byteRect = { rc.right - pad - bs.cx, row2Y, rc.right - pad, row2Y + row2H };
        DrawTextW(mem, bytes.c_str(), -1, &byteRect, DT_RIGHT | DT_TOP | DT_SINGLELINE);

        RECT destRect = { pad, row2Y, rc.right - pad - bs.cx - MulDiv(10, dpi, 96),
                          row2Y + row2H };
        DrawTextW(mem, status.c_str(), -1, &destRect,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        RECT destRect = { pad, row2Y, rc.right - pad, row2Y + row2H };
        DrawTextW(mem, status.c_str(), -1, &destRect,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(mem, oldSmall);

    // Track, then fill
    RECT track = { pad, barY, pad + barW, barY + barH };
    HBRUSH tb = CreateSolidBrush(g_track);
    FillRect(mem, &track, tb);
    DeleteObject(tb);

    if (g_percent > 0) {
        RECT fill = { pad, barY, pad + (barW * g_percent) / 100, barY + barH };
        HBRUSH fb = CreateSolidBrush(g_accent);
        FillRect(mem, &fill, fb);
        DeleteObject(fb);
    }

    // 1px border
    HPEN pen = CreatePen(PS_SOLID, 1, g_border);
    HPEN oldPen = (HPEN)SelectObject(mem, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, 0, 0, rc.right, rc.bottom);
    SelectObject(mem, oldPen);
    SelectObject(mem, oldBrush);
    DeleteObject(pen);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;   // painted fully in WM_PAINT; avoids flicker

    case WM_AZ_PROGRESS:
        g_percent = (int)wp;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_AZ_STATUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_SETTINGCHANGE:
        // Follow a light/dark switch made while the window is up.
        if (IsThemeChange(msg, lp)) {
            ReadTheme();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;

    case WM_AZ_PASSWORD:
        // The prompt needs a visible owner even if the delay has not elapsed.
        RevealWindow(hwnd);
        return AskPasswordOnUiThread() ? 1 : 0;

    case WM_AZ_DONE:
        KillTimer(hwnd, TIMER_SHOW);
        if (wp) {
            PlayDoneSound();
            if (g_shown) {
                // Hold a finished frame briefly so completion is actually seen.
                g_percent = 100;
                // Headline row carries the result; clear row 2 so it is not echoed.
                WriteShared(g_curFile, g_outName.empty() ? L"Done"
                                                         : (L"Done - " + g_outName));
                WriteShared(g_status, L"");
                InvalidateRect(hwnd, NULL, TRUE);
                UpdateWindow(hwnd);
                SetTimer(hwnd, TIMER_DONE, 900, NULL);
            } else {
                // Finished before the window ever appeared: no flash at all.
                DestroyWindow(hwnd);
            }
        } else {
            // ThemedMessage plays the system error sound itself.
            if (!g_error.empty()) {
                RevealWindow(hwnd);
                ThemedMessage(hwnd, L"AirZip", g_error, AZ_BTN_OK, true);
            }
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_SHOW) {
            KillTimer(hwnd, TIMER_SHOW);
            RevealWindow(hwnd);
            return 0;
        }
        if (wp == TIMER_DONE) {
            KillTimer(hwnd, TIMER_DONE);
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int RunExtractorWindow(HINSTANCE inst, const std::wstring &archivePath) {
    g_inst = inst;
    g_archive = archivePath;
    ReadTheme();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"AirZipMain";
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int w = MulDiv(380, dpi, 96);
    int h = MulDiv(84, dpi, 96);   // room for the second information row
    RECT sr; SystemParametersInfoW(SPI_GETWORKAREA, 0, &sr, 0);
    int x = sr.left + ((sr.right - sr.left) - w) / 2;
    int y = sr.top + ((sr.bottom - sr.top) - h) / 2;

    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"AirZipMain", L"AirZip",
                             WS_POPUP, x, y, w, h, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    int corner = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(g_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                          &corner, sizeof(corner));

    g_font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    g_fontSmall = CreateFontW(-MulDiv(8, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    SetStatus(L"Reading " + BaseOf(archivePath) + L"...");

    // Deliberately not shown yet -- see kShowDelayMs.
    SetTimer(g_hwnd, TIMER_SHOW, kShowDelayMs, NULL);

    HANDLE th = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
    if (th) CloseHandle(th);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_font) DeleteObject(g_font);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    return 0;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

static const wchar_t *const kExtensions[] = {
    L".zip", L".rar", L".7z", L".tar", L".gz", L".bz2",
    L".xz",  L".iso", L".cab", L".lzma", L".wim", L".arj"
};

static bool IsAdmin() {
    BOOL admin = FALSE;
    PSID group = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(NULL, group, &admin);
        FreeSid(group);
    }
    return admin != FALSE;
}

static bool SetRegString(HKEY root, const wchar_t *sub, const wchar_t *value) {
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE *)value,
                            (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static bool SetRegValue(HKEY root, const wchar_t *sub, const wchar_t *name, const wchar_t *value) {
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value,
                            (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static bool SetRegDword(HKEY root, const wchar_t *sub, const wchar_t *name, DWORD value) {
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static bool GetRegDefault(HKEY root, const wchar_t *sub, std::wstring &out) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[512] = {0};
    DWORD cb = sizeof(buf), type = 0;
    LONG r = RegQueryValueExW(k, NULL, NULL, &type, (LPBYTE)buf, &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ) return false;
    out = buf;
    return true;
}

#define AZ_BACKUP_KEY L"SOFTWARE\\AirZip\\Associations"
#define AZ_UNINST_KEY L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AirZip"

// Record who owned each extension before we took it, so uninstall can hand it
// back instead of leaving the file type orphaned.
static void BackupAssociations() {
    HKEY bk;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, AZ_BACKUP_KEY, 0, NULL, 0,
                        KEY_READ | KEY_WRITE, NULL, &bk, NULL) != ERROR_SUCCESS) return;

    for (size_t i = 0; i < ARRAYSIZE(kExtensions); ++i) {
        // Never overwrite an existing backup: a reinstall would otherwise
        // record "AirZip.Archive" as the previous owner and lose the real one.
        DWORD type = 0, cb = 0;
        if (RegQueryValueExW(bk, kExtensions[i], NULL, &type, NULL, &cb) == ERROR_SUCCESS)
            continue;

        std::wstring prev;
        if (!GetRegDefault(HKEY_CLASSES_ROOT, kExtensions[i], prev)) prev = L"";
        if (prev == L"AirZip.Archive") continue;

        RegSetValueExW(bk, kExtensions[i], 0, REG_SZ, (const BYTE *)prev.c_str(),
                       (DWORD)((prev.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(bk);
}

static void RestoreAssociations() {
    HKEY bk = NULL;
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, AZ_BACKUP_KEY, 0, KEY_READ, &bk);

    for (size_t i = 0; i < ARRAYSIZE(kExtensions); ++i) {
        std::wstring cur;
        if (!GetRegDefault(HKEY_CLASSES_ROOT, kExtensions[i], cur)) continue;
        // Another app has since claimed this type -- leave it alone.
        if (cur != L"AirZip.Archive") continue;

        std::wstring prev;
        bool havePrev = false;
        if (bk) {
            wchar_t buf[512] = {0};
            DWORD cb = sizeof(buf), type = 0;
            if (RegQueryValueExW(bk, kExtensions[i], NULL, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS
                && type == REG_SZ) {
                prev = buf;
                havePrev = true;
            }
        }

        // No recorded backup (installed by a build predating this, or a fresh
        // type): fall back to any other ProgID the shell already lists under
        // OpenWithProgids, e.g. CompressedFolder for .zip. Evidence from the
        // registry rather than a hardcoded guess.
        if (!havePrev || prev.empty()) {
            HKEY ow;
            std::wstring sub = std::wstring(kExtensions[i]) + L"\\OpenWithProgids";
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, sub.c_str(), 0, KEY_READ, &ow)
                == ERROR_SUCCESS) {
                // Prefer a classic ProgID (CompressedFolder, ArchiveFolder) over
                // an AppX package identity, which does not work as a plain default.
                std::wstring anyCandidate;
                for (DWORD vi = 0; ; ++vi) {
                    wchar_t name[256];
                    DWORD cch = ARRAYSIZE(name);
                    if (RegEnumValueW(ow, vi, name, &cch, NULL, NULL, NULL, NULL)
                        != ERROR_SUCCESS) break;
                    if (!cch || _wcsicmp(name, L"AirZip.Archive") == 0) continue;
                    if (anyCandidate.empty()) anyCandidate = name;
                    if (_wcsnicmp(name, L"AppX", 4) != 0) {
                        prev = name;
                        havePrev = true;
                        break;
                    }
                }
                if (!havePrev && !anyCandidate.empty()) {
                    prev = anyCandidate;
                    havePrev = true;
                }
                RegCloseKey(ow);
            }
        }

        if (havePrev && !prev.empty()) {
            SetRegString(HKEY_CLASSES_ROOT, kExtensions[i], prev.c_str());
        } else {
            // Genuinely unowned: drop our default value, keep the key.
            HKEY k;
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, kExtensions[i], 0, KEY_SET_VALUE, &k)
                == ERROR_SUCCESS) {
                RegDeleteValueW(k, NULL);
                RegCloseKey(k);
            }
        }
    }
    if (bk) RegCloseKey(bk);
}

// Makes AirZip appear in Settings > Apps and in Add/Remove Programs.
static void RegisterUninstallEntry(const std::wstring &targetDir, const std::wstring &targetExe) {
    std::wstring icon      = L"\"" + targetExe + L"\",0";
    std::wstring uninst    = L"\"" + targetExe + L"\" --uninstall";
    std::wstring quiet     = L"\"" + targetExe + L"\" --silent-uninstall";

    SetRegValue(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY, L"DisplayName",          L"AirZip");
    SetRegValue(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY, L"DisplayVersion",       L"1.0.0.0");
    SetRegValue(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY, L"Publisher",            L"AirZip");
    SetRegValue(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY, L"DisplayIcon",          icon.c_str());
    SetRegValue(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY, L"UninstallString",      uninst.c_str());
    SetRegValue(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY, L"QuietUninstallString", quiet.c_str());
    SetRegValue(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY, L"InstallLocation",      targetDir.c_str());
    SetRegDword(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY, L"NoModify", 1);
    SetRegDword(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY, L"NoRepair", 1);

    LARGE_INTEGER sz; sz.QuadPart = 0;
    HANDLE h = CreateFileW(targetExe.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { GetFileSizeEx(h, &sz); CloseHandle(h); }
    SetRegDword(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY, L"EstimatedSize",
                (DWORD)(sz.QuadPart / 1024));
}

static bool PerformInstall(bool silent) {
    wchar_t pf[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, pf))) return false;

    std::wstring targetDir = Join(pf, L"AirZip");
    std::wstring targetExe = Join(targetDir, L"AirZip.exe");
    std::wstring self = SelfPath();

    if (_wcsicmp(self.c_str(), targetExe.c_str()) != 0) {
        // Clear any previous install first. Earlier versions shipped several
        // files (AirZip.dll, SevenZipSharp.dll, 7z.dll, *.json); this build is a
        // single exe, so copying over the top would strand all of them.
        if (GetFileAttributesW(targetDir.c_str()) != INVALID_FILE_ATTRIBUTES)
            ClearDir(targetDir);

        MakeDirs(targetDir);
        if (!CopyFileW(self.c_str(), targetExe.c_str(), FALSE)) {
            if (!silent) ThemedMessage(NULL, L"AirZip", L"Could not write to:\n" + targetDir,
                                       AZ_BTN_OK, true);
            return false;
        }
    }

    std::wstring iconVal = L"\"" + targetExe + L"\",0";
    std::wstring cmdVal  = L"\"" + targetExe + L"\" \"%1\"";

    SetRegString(HKEY_CLASSES_ROOT, L"AirZip.Archive", L"AirZip Archive");
    SetRegString(HKEY_CLASSES_ROOT, L"AirZip.Archive\\DefaultIcon", iconVal.c_str());
    SetRegString(HKEY_CLASSES_ROOT, L"AirZip.Archive\\shell\\open\\command", cmdVal.c_str());

    BackupAssociations();   // must run before we overwrite the defaults

    for (size_t i = 0; i < ARRAYSIZE(kExtensions); ++i)
        SetRegString(HKEY_CLASSES_ROOT, kExtensions[i], L"AirZip.Archive");

    RegisterUninstallEntry(targetDir, targetExe);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

    if (!silent) ThemedMessage(NULL, L"AirZip", L"AirZip installed successfully.",
                               AZ_BTN_OK, false);
    return true;
}

static bool PerformUninstall(bool silent) {
    RestoreAssociations();
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"AirZip.Archive");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, AZ_UNINST_KEY);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AirZip");

    wchar_t pf[MAX_PATH];
    bool filesGone = true;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, pf))) {
        std::wstring targetDir = Join(pf, L"AirZip");
        // The exe that launched us may still be shutting down, so retry briefly.
        for (int attempt = 0; attempt < 12; ++attempt) {
            if (GetFileAttributesW(targetDir.c_str()) == INVALID_FILE_ATTRIBUTES) break;
            ClearDir(targetDir);
            RemoveDirectoryW(targetDir.c_str());
            if (GetFileAttributesW(targetDir.c_str()) == INVALID_FILE_ATTRIBUTES) break;
            Sleep(250);
        }
        filesGone = GetFileAttributesW(targetDir.c_str()) == INVALID_FILE_ATTRIBUTES;
    }

    // Drop the unpacked 7-Zip cache too.
    std::wstring cache = TempDir();
    if (GetFileAttributesW(cache.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ClearDir(cache);
        RemoveDirectoryW(cache.c_str());
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

    if (!silent) {
        ThemedMessage(NULL, L"AirZip",
            filesGone ? L"AirZip has been removed."
                      : L"AirZip was unregistered, but some files are still in use "
                        L"and will be removed on the next restart.",
            AZ_BTN_OK, false);
    }
    return filesGone;
}

static bool EnsureAdminAndUninstall(bool silent) {
    wchar_t pf[MAX_PATH];
    std::wstring targetDir;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, pf)))
        targetDir = Join(pf, L"AirZip");

    std::wstring self = SelfPath();
    bool runningFromInstall = !targetDir.empty() && self.size() > targetDir.size() &&
        _wcsnicmp(self.c_str(), targetDir.c_str(), targetDir.size()) == 0;

    // A running exe cannot delete itself, and Apps & Features launches exactly
    // that copy. Hand the job to a throwaway clone outside the install folder.
    if (runningFromInstall) {
        wchar_t tmp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tmp)) {
            std::wstring clone = Join(tmp, L"AirZipUninstall.exe");
            if (CopyFileW(self.c_str(), clone.c_str(), FALSE)) {
                SHELLEXECUTEINFOW ei = { sizeof(ei) };
                ei.lpVerb = L"runas";
                ei.lpFile = clone.c_str();
                ei.lpParameters = silent ? L"--uninstall-run-silent" : L"--uninstall-run";
                ei.nShow = SW_SHOWNORMAL;
                if (!ShellExecuteExW(&ei)) return false;
                // Cannot wait: this process must exit before its own exe can be
                // deleted, so the clone's result is unobservable from here.
                if (g_cli)
                    Emit(L"\nUninstall running in the background "
                         L"(this copy must exit before it can be removed).\n");
                return true;
            }
        }
    }

    if (IsAdmin()) return PerformUninstall(silent);

    SHELLEXECUTEINFOW ei = { sizeof(ei) };
    ei.fMask  = SEE_MASK_NOCLOSEPROCESS;
    ei.lpVerb = L"runas";
    ei.lpFile = self.c_str();
    ei.lpParameters = silent ? L"--silent-uninstall" : L"--uninstall";
    ei.nShow  = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&ei) || !ei.hProcess) return false;   // elevation refused
    WaitForSingleObject(ei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(ei.hProcess, &code);
    CloseHandle(ei.hProcess);
    return code == AZ_OK;
}

// Runs from the temp clone: wait for the caller to exit, remove everything,
// then have Windows delete this clone on the next restart.
static void UninstallFromClone(bool silent) {
    Sleep(800);
    if (!IsAdmin()) {
        SHELLEXECUTEINFOW ei = { sizeof(ei) };
        ei.fMask  = SEE_MASK_NOCLOSEPROCESS;
        ei.lpVerb = L"runas";
        std::wstring self = SelfPath();
        ei.lpFile = self.c_str();
        ei.lpParameters = silent ? L"--uninstall-run-silent" : L"--uninstall-run";
        ei.nShow  = SW_SHOWNORMAL;
        if (ShellExecuteExW(&ei) && ei.hProcess) {
            WaitForSingleObject(ei.hProcess, INFINITE);
            CloseHandle(ei.hProcess);
        }
        return;
    }
    PerformUninstall(silent);
    MoveFileExW(SelfPath().c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
}

static bool EnsureAdminAndInstall(bool silent) {
    if (IsAdmin()) return PerformInstall(silent);

    std::wstring self = SelfPath();
    SHELLEXECUTEINFOW ei = { sizeof(ei) };
    ei.fMask  = SEE_MASK_NOCLOSEPROCESS;
    ei.lpVerb = L"runas";
    ei.lpFile = self.c_str();
    ei.lpParameters = silent ? L"--silent-install" : L"--install";
    ei.nShow  = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&ei) || !ei.hProcess) {
        if (g_cli) Emit(L"\nAirZip: administrator rights are required.\n");
        return false;
    }
    WaitForSingleObject(ei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(ei.hProcess, &code);
    CloseHandle(ei.hProcess);
    return code == AZ_OK;
}

// ---------------------------------------------------------------------------
// Console support
//
// This is a /SUBSYSTEM:WINDOWS binary so that extraction shows no console
// window. That costs us stdout, so for command-line use we attach to whatever
// console launched us and write there instead of popping dialogs.
//
// One consequence cannot be engineered away: a GUI-subsystem process does not
// block cmd.exe, so the shell prints its prompt immediately. Use
// `start /wait AirZip.exe ...` (cmd) or `Start-Process -Wait` (PowerShell) when
// the exit code matters.
// ---------------------------------------------------------------------------

// On a console say it in text; otherwise fall back to a dialog.
static void ReportInfo(const std::wstring &text) {
    if (g_cli) Emit(L"\n" + text + L"\n");
    else MessageBoxW(NULL, text.c_str(), L"AirZip", MB_OK | MB_ICONINFORMATION);
}

static void ReportError(const std::wstring &text) {
    if (g_cli) Emit(L"\nAirZip: " + text + L"\n");
    else MessageBoxW(NULL, text.c_str(), L"AirZip", MB_OK | MB_ICONERROR);
}

static void PrintUsage() {
    Emit(
        L"\nAirZip - archive extractor\n\n"
        L"Usage:\n"
        L"  AirZip.exe <archive>         Extract an archive\n"
        L"  AirZip.exe --install         Install and register file types\n"
        L"  AirZip.exe --silent-install  Install with no prompts\n"
        L"  AirZip.exe --uninstall       Remove AirZip (asks first)\n"
        L"  AirZip.exe --silent-uninstall  Remove with no prompts\n"
        L"  AirZip.exe --version         Print version\n"
        L"  AirZip.exe --help            This text\n\n"
        L"Exit codes:\n"
        L"  0 success   1 failed   2 bad usage   3 file not found   4 not elevated\n\n"
        L"Installing and uninstalling need administrator rights and will prompt\n"
        L"for elevation unless already elevated.\n\n"
        L"This is a GUI-subsystem binary, so cmd.exe does not wait for it. For a\n"
        L"meaningful exit code use:\n"
        L"  start /wait AirZip.exe --silent-install        (cmd)\n"
        L"  Start-Process AirZip.exe '--silent-install' -Wait -PassThru   (PowerShell)\n\n"
        L"Uninstalling the copy in Program Files is asynchronous: the exe cannot\n"
        L"delete itself, so the work is handed to a temporary clone and this\n"
        L"process returns immediately.\n");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    InitializeCriticalSection(&g_textLock);
    g_inst = inst;
    ReadTheme();      // dialogs are themed even on the install/uninstall paths
    ConsoleInit();

    HRESULT coInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int rc = AZ_OK;

    const wchar_t *cmd = argc >= 2 ? argv[1] : NULL;
    bool isFlag = cmd && (cmd[0] == L'-' || (cmd[0] == L'/' && cmd[1] == L'?'));

    bool packaged = IsPackaged();

    if (argc < 2) {
        if (packaged) {
            // Store owns the file associations; there is nothing to offer.
            ThemedMessage(NULL, L"AirZip",
                L"AirZip is installed. Right-click an archive file, choose "
                L"\"Open with\", and select AirZip -- or set it as your default "
                L"app for archive files in Settings.",
                AZ_BTN_OK, false);
        } else {
            // Double-clicked with no argument: offer to install.
            int answer = ThemedMessage(NULL, L"AirZip",
                L"Install AirZip and make it the default handler for archive files "
                L"(.zip, .rar, .7z, and others)?",
                AZ_BTN_YESNO, false);
            if (answer == IDYES) rc = EnsureAdminAndInstall(false) ? AZ_OK : AZ_FAILED;
        }
    } else if (_wcsicmp(cmd, L"--help") == 0 || _wcsicmp(cmd, L"-h") == 0 ||
               _wcsicmp(cmd, L"/?") == 0) {
        if (g_cli) PrintUsage();
        else MessageBoxW(NULL, L"Run AirZip.exe --help from a command prompt "
                               L"to see the available options.",
                         L"AirZip", MB_OK | MB_ICONINFORMATION);
    } else if (_wcsicmp(cmd, L"--version") == 0) {
        if (g_cli) Emit(L"\nAirZip 1.0.0.0\n");
        else MessageBoxW(NULL, L"AirZip 1.0.0.0", L"AirZip", MB_OK | MB_ICONINFORMATION);
    } else if (_wcsicmp(cmd, L"--install") == 0 || _wcsicmp(cmd, L"--silent-install") == 0) {
        bool silent = _wcsicmp(cmd, L"--silent-install") == 0;
        if (packaged) {
            if (!silent) ThemedMessage(NULL, L"AirZip",
                L"AirZip was installed through the Microsoft Store and does not "
                L"need a separate install step.", AZ_BTN_OK, false);
        } else {
            rc = EnsureAdminAndInstall(silent) ? AZ_OK : AZ_FAILED;
        }
    } else if (_wcsicmp(cmd, L"--uninstall") == 0 || _wcsicmp(cmd, L"--silent-uninstall") == 0) {
        bool silent = _wcsicmp(cmd, L"--silent-uninstall") == 0;
        if (packaged) {
            if (!silent) ThemedMessage(NULL, L"AirZip",
                L"AirZip was installed through the Microsoft Store. Uninstall it "
                L"from Settings or the Start menu.", AZ_BTN_OK, false);
        } else {
            bool go = true;
            if (!silent && !g_cli) {
                go = ThemedMessage(NULL, L"AirZip",
                    L"Remove AirZip and restore the previous handlers for archive files?",
                    AZ_BTN_YESNO, false) == IDYES;
            }
            if (go) rc = EnsureAdminAndUninstall(silent) ? AZ_OK : AZ_FAILED;
        }
    } else if (_wcsicmp(cmd, L"--uninstall-run") == 0) {
        UninstallFromClone(false);
    } else if (_wcsicmp(cmd, L"--uninstall-run-silent") == 0) {
        UninstallFromClone(true);
    } else if (isFlag) {
        // A typo must not sit on a modal dialog forever waiting for a click.
        ReportError(std::wstring(L"unknown option: ") + cmd);
        if (g_cli) PrintUsage();
        rc = AZ_USAGE;
    } else {
        std::wstring archive = cmd;
        if (GetFileAttributesW(archive.c_str()) == INVALID_FILE_ATTRIBUTES) {
            ReportError(L"file not found: " + archive);
            rc = AZ_NOT_FOUND;
        } else {
            // Full path, so the extraction target does not depend on the cwd.
            wchar_t full[MAX_PATH];
            if (GetFullPathNameW(archive.c_str(), MAX_PATH, full, NULL)) archive = full;
            rc = RunExtractorWindow(inst, archive);
        }
    }

    LocalFree(argv);
    if (g_7z) FreeLibrary(g_7z);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return rc;
}
