/*
 * loopback_rec_gui.c - Win32 GUI front-end for the loopback_rec recorder.
 *
 * Records everything played on the default Windows output device (WASAPI
 * shared-mode loopback) to 48 kHz / 16-bit / stereo WAV. The capture, writer,
 * silence-fill and device-change engine is the same as in loopback_rec.c
 * (console version); this file adds a small pure Win32/GDI window, settings,
 * a global hotkey, a tray icon/notification, a level meter, low-disk
 * protection, optional time-based file splitting and a headless test mode.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-windres loopback_rec_gui.rc -O coff -o loopback_rec_gui_res.o
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -mwindows -static -s \
 *       -o loopback_rec_gui.exe loopback_rec_gui.c loopback_rec_gui_res.o \
 *       -lole32 -lavrt -lshell32 -luser32 -lgdi32 -lcomctl32 -luuid -lmfuuid
 *
 * Compression (Windows' own Media Foundation AAC encoder, loaded dynamically):
 *   - "durante" (default): PCM is encoded live on the writer thread and written as
 *     MP4 parts by a small built-in muxer with a crash-recovery index (see the
 *     "Live AAC recording" section); pause/resume and resumable sessions build on it.
 *   - "dopo": WAV parts are converted to .m4a after the recording, verified, deleted.
 *
 * Test mode (no UI):  loopback_rec_gui.exe --test-seconds N --out <folder>
 *   [--name T] [--max-mb M] [--split-seconds S] [--segment-seconds S] [--kbps K] [--mono]
 *   [--keep-wav] [--no-convert] [--after] [--pause-at S --resume-at S] [--suspend-after S]
 *   [--simulate-crash S] [--resume-session] [--save-session] [--recover-only]
 *   [--silence-cut | --no-silence-cut] [--silence-db D] [--silence-min S] [--silence-pad MS]
 *   [--synth S,Q]  (replace captured audio with S s tone / Q s silence, for testing)
 *   [--silence-change-at S:on,db,min,pad]  (up to 8: change the silence cut live at S s)
 *   [--rename-at S:Name]  (up to 4: type a new name at S s; the files are renamed live)
 *   [--source-exe NAME]   (record only one app, e.g. chrome.exe, via process loopback)
 *   [--source-switch-at S:EXE|S:system]  (up to 4: change the source live at S s, 1.4.1)
 *   --list-sources --out <folder>  (log the apps the source picker would offer, then exit)
 *
 * Source switch (1.4.1): the picker also works while recording/paused/pending; the capture
 * thread swaps the capture client live into the same file (gap = synthesized silence).
 * Source (1.4): "Tutto il sistema" = WASAPI loopback of the default output device, or one
 * app = Windows 10 2004+/11 process loopback (VAD\Process_Loopback, include process tree)
 * on the app's root process, so every child process (e.g. the browser's audio service) is
 * included. A single browser tab cannot be isolated: Windows has no API for it.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <avrt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <propidl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <tlhelp32.h>
#include <audiopolicy.h>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#endif

/* ------------------------------------------------------------------ */
/* GUIDs defined locally so we do not depend on uuid.lib contents.     */
/* ------------------------------------------------------------------ */
static const GUID MY_CLSID_MMDeviceEnumerator = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID MY_IID_IMMDeviceEnumerator  = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID MY_IID_IAudioClient         = {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID MY_IID_IAudioCaptureClient  = {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};
static const GUID MY_IID_IAudioRenderClient   = {0xF294ACFC,0x3146,0x4483,{0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2}};
static const GUID MY_IID_IMMNotificationClient= {0x7991EEC9,0x7E89,0x4D85,{0x83,0x90,0x6C,0x70,0x3C,0xEC,0x60,0xC0}};
static const GUID MY_IID_IUnknown             = {0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID MY_SUBTYPE_PCM              = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MY_SUBTYPE_IEEE_FLOAT       = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MY_CLSID_AACMFTEncoder      = {0x93AF0C51,0x2275,0x45D2,{0xA3,0x5B,0xF2,0xBA,0x21,0xCA,0xED,0x00}};
static const GUID MY_IID_IMFTransform         = {0xBF94C121,0x5B05,0x4E6F,{0x80,0x00,0xBA,0x59,0x89,0x61,0x41,0x4D}};
static const GUID MY_FOLDERID_Documents       = {0xFDD39AD0,0x238F,0x46AF,{0xAD,0xB4,0x6C,0x85,0x48,0x03,0x69,0xC7}};
static const PROPERTYKEY MY_PKEY_Device_FriendlyName =
    {{0xA45C254E,0xDF1C,0x4EFD,{0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}}, 14};

static const GUID MY_IID_IAudioSessionManager2 = {0x77AA99A0,0x1BD6,0x484F,{0x8B,0xC7,0x2C,0x65,0x4C,0x9A,0x9B,0x6F}};
static const GUID MY_IID_IAudioSessionControl2 = {0xBFB7FF88,0x7239,0x4FC9,{0x8F,0xA2,0x07,0xC9,0x50,0xBE,0x9C,0x6D}};
static const GUID MY_IID_IActivateCompletion   = {0x41D949AB,0x9862,0x444A,{0x80,0xF6,0xC2,0x61,0x33,0x4D,0xA5,0xEB}};
static const GUID MY_IID_IAgileObject          = {0x94EA2B94,0xE9CC,0x49E0,{0xC0,0xFF,0xEE,0x64,0xCA,0x8F,0x5B,0x90}};

static int guid_eq(const GUID *a, const GUID *b) { return memcmp(a, b, sizeof(GUID)) == 0; }

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define OUT_RATE        48000
#define OUT_CH          2
#define OUT_BPF         4                         /* bytes per output frame */
#define RING_BYTES      (32u * 1024u * 1024u)     /* ~174 s of audio */
#define MAX_DATA_BYTES  4000000000ULL             /* roll before 4 GiB RIFF limit */
#define HEADER_EVERY_MS 10000
#define CAPTURE_BUF_HNS 5000000                   /* 500 ms loopback buffer */
#define RENDER_BUF_HNS  2000000                   /* 200 ms keep-alive buffer */
#define POLL_MS         10
#define IDLE_PAD_MS     100     /* no packets for this long -> synthesize silence */
#define IDLE_SLACK_MS   20      /* keep this much behind "now" when synthesizing */
#define GAP_PAD_MS      100     /* timestamp gap that is always filled */
#define QUIET_PAD_MS    5       /* gap filled when packet is quiet/flagged */
#define AHEAD_DROP_MS   50      /* drop quiet packets when this far ahead */
#define WATCHDOG_MS     5000    /* no packets at all for this long -> reinit */
#define DEVCHECK_MS     2000    /* poll default-device id (backstop for notifications) */
#define BACKOFF_MIN_MS  250
#define BACKOFF_MAX_MS  5000
#define QUIET_PEAK      0.001f  /* -60 dBFS */
#define MS_TO_FRAMES(ms) ((LONG64)(ms) * OUT_RATE / 1000)

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */
static HANDLE g_stopEvent;        /* manual reset: stop requested           */
static HANDLE g_doneEvent;        /* manual reset: file finalized           */
static HANDLE g_devChangedEvent;  /* auto reset: default device changed     */
static HANDLE g_captureThread;
static volatile LONG g_stopReason;   /* 0 none, 1 user, 3 session end, 4 write error, 5 low disk */

static BYTE *g_ring;
static volatile LONG64 g_ringW;   /* total bytes produced (capture thread) */
static volatile LONG64 g_ringR;   /* total bytes consumed (writer thread)  */

static LONG64 g_qpcFreq;
static volatile LONG64 g_start100ns;  /* timeline origin, 100-ns QPC units (moved forward after a pause) */
static volatile LONG64 g_framesOut;    /* frames accepted into the timeline */
static volatile LONG64 g_framesPadded; /* of which synthesized silence     */
static volatile LONG64 g_framesDropped;/* lost to ring overflow / dropped  */
static volatile LONG64 g_bytesWritten; /* bytes written to disk, all parts */
static volatile LONG   g_reconnects;
static volatile LONG   g_discont;
static volatile LONG   g_haveDevice;
static volatile LONG   g_writeError;

static CRITICAL_SECTION g_statusLock;
static WCHAR g_devName[256] = L"(none)";
static WCHAR g_fmtDesc[128] = L"";
static WCHAR g_curPath[MAX_PATH * 2] = L"";

static CRITICAL_SECTION g_outLock;   /* serializes log file writes */
static WCHAR g_logPath[MAX_PATH * 2];

/* level meter: peak * 1e6, "since last UI read" and "whole session" */
static volatile LONG g_peakNow;
static volatile LONG g_peakMax;

/* ------------------------------------------------------------------ */
/* Output / logging                                                    */
/* ------------------------------------------------------------------ */
/* Log lines go to a UTF-8 file (%APPDATA%\LoopbackRec\log.txt, or test.log in test mode). */
static void log_msg(const WCHAR *fmt, ...)
{
    WCHAR msg[1024], line[1200];
    char utf8[3800];
    SYSTEMTIME st;
    va_list ap;
    HANDLE h;
    int len;
    va_start(ap, fmt);
    _vsnwprintf(msg, 1023, fmt, ap);
    va_end(ap);
    msg[1023] = 0;
    GetLocalTime(&st);
    _snwprintf(line, 1199, L"%04u-%02u-%02u %02u:%02u:%02u  %ls\r\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    line[1199] = 0;
    len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    if (len <= 1 || !g_logPath[0]) return;
    EnterCriticalSection(&g_outLock);
    h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD n;
        WriteFile(h, utf8, (DWORD)(len - 1), &n, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_outLock);
}

static const WCHAR *hr_str(HRESULT hr, WCHAR *buf, size_t cch)
{
    const WCHAR *s = NULL;
    switch ((unsigned long)hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:      s = L"AUDCLNT_E_DEVICE_INVALIDATED (device removed/changed)"; break;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:     s = L"AUDCLNT_E_SERVICE_NOT_RUNNING (Windows Audio service stopped)"; break;
    case AUDCLNT_E_DEVICE_IN_USE:           s = L"AUDCLNT_E_DEVICE_IN_USE (exclusive-mode app holds the device)"; break;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:      s = L"AUDCLNT_E_UNSUPPORTED_FORMAT"; break;
    case AUDCLNT_E_NOT_INITIALIZED:         s = L"AUDCLNT_E_NOT_INITIALIZED"; break;
    case AUDCLNT_E_ALREADY_INITIALIZED:     s = L"AUDCLNT_E_ALREADY_INITIALIZED"; break;
    case AUDCLNT_E_WRONG_ENDPOINT_TYPE:     s = L"AUDCLNT_E_WRONG_ENDPOINT_TYPE"; break;
    case AUDCLNT_E_BUFFER_SIZE_ERROR:       s = L"AUDCLNT_E_BUFFER_SIZE_ERROR"; break;
    case AUDCLNT_E_BUFFER_TOO_LARGE:        s = L"AUDCLNT_E_BUFFER_TOO_LARGE"; break;
    case AUDCLNT_E_BUFFER_OPERATION_PENDING:s = L"AUDCLNT_E_BUFFER_OPERATION_PENDING"; break;
    case AUDCLNT_E_OUT_OF_ORDER:            s = L"AUDCLNT_E_OUT_OF_ORDER"; break;
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:  s = L"AUDCLNT_E_ENDPOINT_CREATE_FAILED"; break;
    case AUDCLNT_E_CPUUSAGE_EXCEEDED:       s = L"AUDCLNT_E_CPUUSAGE_EXCEEDED"; break;
    case AUDCLNT_E_RESOURCES_INVALIDATED:   s = L"AUDCLNT_E_RESOURCES_INVALIDATED"; break;
    case 0x80070490UL:                      s = L"E_NOTFOUND (no default output device)"; break;
    default: break;
    }
    if (s) {
        _snwprintf(buf, cch - 1, L"0x%08lX %ls", (unsigned long)hr, s);
    } else {
        WCHAR sys[512] = L"";
        DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL, (DWORD)hr, 0, sys, 511, NULL);
        while (n > 0 && (sys[n - 1] == L'\r' || sys[n - 1] == L'\n' || sys[n - 1] == L' ')) sys[--n] = 0;
        _snwprintf(buf, cch - 1, L"0x%08lX %ls", (unsigned long)hr, n ? sys : L"(unknown error)");
    }
    buf[cch - 1] = 0;
    return buf;
}

#define LOG_HR(what, hr) do { WCHAR _b[640]; log_msg(L"%ls failed: %ls", (what), hr_str((hr), _b, 640)); } while (0)

static void request_stop(LONG reason)
{
    InterlockedCompareExchange(&g_stopReason, reason, 0);
    SetEvent(g_stopEvent);
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
static LONG64 now_100ns(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / g_qpcFreq) * 10000000LL + (c.QuadPart % g_qpcFreq) * 10000000LL / g_qpcFreq;
}
static LONG64 now_ms(void) { return now_100ns() / 10000; }
/* timeline position (output frames) corresponding to a 100-ns QPC time */
static LONG64 t100_to_frames(LONG64 t) { return (t - g_start100ns) * OUT_RATE / 10000000LL; }

/* ------------------------------------------------------------------ */
/* Lock-free SPSC ring (capture thread -> writer thread)               */
/* ------------------------------------------------------------------ */
static LONG64 atomic_read64(volatile LONG64 *p) { return InterlockedCompareExchange64(p, 0, 0); }

static size_t ring_free(void)
{
    return RING_BYTES - (size_t)(g_ringW - atomic_read64(&g_ringR));   /* g_ringW owned by producer */
}

/* write n bytes (src NULL = zeros); returns bytes written (multiple of OUT_BPF) */
static size_t ring_put(const BYTE *src, size_t n)
{
    size_t fr = ring_free(), off, first;
    LONG64 w = g_ringW;
    if (n > fr) n = fr;
    n -= n % OUT_BPF;
    if (n == 0) return 0;
    off = (size_t)(w % RING_BYTES);
    first = RING_BYTES - off;
    if (first > n) first = n;
    if (src) { memcpy(g_ring + off, src, first); memcpy(g_ring, src + first, n - first); }
    else     { memset(g_ring + off, 0, first);   memset(g_ring, 0, n - first); }
    InterlockedExchange64(&g_ringW, w + (LONG64)n);   /* full barrier: publish after data */
    return n;
}

/* ------------------------------------------------------------------ */
/* WAV writer (writer thread)                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    HANDLE   h;
    ULONG64  dataBytes;
    int      part;
    WCHAR    folder[MAX_PATH];
    WCHAR    base[64];
    ULONG64  splitBytes;   /* 0 = only split at the 4 GB limit */
    DWORD    lastErr;      /* Win32 error of the last failed open */
    WCHAR    path[MAX_PATH * 2];   /* current part */
    int      live;         /* 1 = live AAC .m4a parts (live_* functions), 0 = WAV parts */
} WavOut;

static void on_part_closed(const WCHAR *path, ULONG64 dataBytes);   /* GUI hook: queue AAC conversion */
/* live AAC path (implemented after the Media Foundation section) */
static BOOL live_open(WavOut *o);
static BOOL live_write(WavOut *o, const BYTE *p, size_t n);
static void live_close(WavOut *o);
static void live_tick(WavOut *o);

static void put_le32(BYTE *p, DWORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }
static void put_le16(BYTE *p, WORD v)  { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }

static BOOL write_all(HANDLE h, const void *p, DWORD n)
{
    const BYTE *b = (const BYTE *)p;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(h, b, n, &w, NULL) || w == 0) return FALSE;
        b += w; n -= w;
    }
    return TRUE;
}

static BOOL wav_header(WavOut *o)
{
    BYTE hd[44];
    LARGE_INTEGER zero, end;
    DWORD data = (DWORD)o->dataBytes;
    memcpy(hd, "RIFF", 4);       put_le32(hd + 4, 36 + data);
    memcpy(hd + 8, "WAVEfmt ", 8); put_le32(hd + 16, 16);
    put_le16(hd + 20, 1);        put_le16(hd + 22, OUT_CH);
    put_le32(hd + 24, OUT_RATE); put_le32(hd + 28, OUT_RATE * OUT_BPF);
    put_le16(hd + 32, OUT_BPF);  put_le16(hd + 34, 16);
    memcpy(hd + 36, "data", 4);  put_le32(hd + 40, data);
    zero.QuadPart = 0;
    if (!SetFilePointerEx(o->h, zero, NULL, FILE_BEGIN)) return FALSE;
    if (!write_all(o->h, hd, 44)) return FALSE;
    end.QuadPart = 0;
    return SetFilePointerEx(o->h, end, NULL, FILE_END);
}

static BOOL wav_open(WavOut *o)
{
    WCHAR path[MAX_PATH * 2];
    int attempt;
    if (o->live) return live_open(o);
    for (attempt = 0; attempt < 100; attempt++) {
        WCHAR sfx[32] = L"";
        if (o->part > 1)  _snwprintf(sfx, 31, L"_part%d", o->part);
        if (attempt > 0)  _snwprintf(sfx + wcslen(sfx), 31 - wcslen(sfx), L"_%d", attempt);
        sfx[31] = 0;
        _snwprintf(path, MAX_PATH * 2 - 1, L"%ls\\%ls%ls.wav", o->folder, o->base, sfx);
        path[MAX_PATH * 2 - 1] = 0;
        o->h = CreateFileW(path, GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (o->h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (o->h == INVALID_HANDLE_VALUE) {
        o->lastErr = GetLastError();
        LOG_HR(L"Creating output file", HRESULT_FROM_WIN32(GetLastError()));
        return FALSE;
    }
    o->dataBytes = 0;
    if (!wav_header(o)) {
        LOG_HR(L"Writing WAV header", HRESULT_FROM_WIN32(GetLastError()));
        CloseHandle(o->h); o->h = INVALID_HANDLE_VALUE;
        return FALSE;
    }
    wcscpy(o->path, path);
    EnterCriticalSection(&g_statusLock);
    wcsncpy(g_curPath, path, MAX_PATH * 2 - 1);
    LeaveCriticalSection(&g_statusLock);
    log_msg(L"Writing %ls", path);
    return TRUE;
}

static void wav_close(WavOut *o)
{
    if (o->live) { live_close(o); return; }
    if (o->h == INVALID_HANDLE_VALUE) return;
    if (!wav_header(o)) LOG_HR(L"Finalizing WAV header", HRESULT_FROM_WIN32(GetLastError()));
    FlushFileBuffers(o->h);
    CloseHandle(o->h);
    o->h = INVALID_HANDLE_VALUE;
    on_part_closed(o->path, o->dataBytes);
}

/* write n bytes to the current part, rolling to a new part near 4 GiB */
static BOOL wav_write(WavOut *o, const BYTE *p, size_t n)
{
    if (o->live) return live_write(o, p, n);
    while (n) {
        ULONG64 limit = (o->splitBytes && o->splitBytes < MAX_DATA_BYTES) ? o->splitBytes : MAX_DATA_BYTES;
        ULONG64 room = limit > o->dataBytes ? limit - o->dataBytes : 0;
        size_t chunk = n;
        if (room < OUT_BPF) {
            wav_close(o);
            o->part++;
            log_msg(limit < MAX_DATA_BYTES ? L"Split interval reached, starting part %d"
                                           : L"Approaching 4 GB WAV limit, starting part %d", o->part);
            if (!wav_open(o)) return FALSE;
            continue;
        }
        if ((ULONG64)chunk > room) chunk = (size_t)(room - room % OUT_BPF);
        if (!write_all(o->h, p, (DWORD)chunk)) {
            LOG_HR(L"Writing audio data", HRESULT_FROM_WIN32(GetLastError()));
            return FALSE;
        }
        o->dataBytes += chunk;
        InterlockedExchangeAdd64(&g_bytesWritten, (LONG64)chunk);
        p += chunk; n -= chunk;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Automatic silence cut (writer thread, before the WAV/AAC encoder)   */
/* ------------------------------------------------------------------ */
/* The capture timeline (g_framesOut) is untouched: the gate sits between the ring and the
   file, so drift/padding logic keeps working. Audio is classified in 10 ms blocks by RMS
   level; a silent run longer than SilenceMinSec is removed except SilencePadMs after the last
   sound and SilencePadMs before the sound resumes (lookahead via the hold ring). */
#define GATE_BLOCK  480                          /* frames per level block (10 ms) */
static int g_silCut = 0, g_silMinSec = 3, g_silPadMs = 500, g_silDb = -45;   /* settings */
static volatile LONG g_gateOn;                   /* gate active now (writer thread owns; UI reads) */
static volatile LONG g_gateUsed;                 /* gate was on at some point in this run */
static int g_gateMinSec, g_gatePadMs, g_gateDb;  /* parameters in effect (writer thread) */
/* live changes from the settings dialog: UI writes the request under g_gateReqLock and bumps
   g_gateReqVer; the writer thread applies it at the next gate_write() call */
static CRITICAL_SECTION g_gateReqLock;
static volatile LONG g_gateReqVer;
static LONG g_gateAppliedVer;                    /* writer thread */
static int  g_gateReqOn, g_gateReqMin, g_gateReqPad, g_gateReqDb;
static volatile LONG64 g_cutFrames;              /* frames removed in this run */
static volatile LONG   g_gateCutting;            /* 1 while a cut is in progress (UI) */
static volatile LONG64 g_gateRunF;               /* length of the current silence, frames (UI) */
static LONG64 g_cutBaseMs;                       /* cut in earlier runs of a resumed session */
static volatile LONG   g_gateCuts;               /* number of cuts in this run */
/* silence metering: runs on every recording, also with the cut off (display only) */
static volatile LONG64 g_silRunF;                /* current below-threshold run, frames */
static volatile LONG64 g_silLastF;               /* length of the last silence of >= 1 s, frames */
static volatile LONG64 g_silLastEndMs;           /* when it ended (now_ms) */
static volatile LONG   g_silMeterReset;          /* set on resume from pause: restart the run */
static volatile LONG   g_silEvents;              /* silences of >= 1 s in this run (test log) */
static volatile LONG64 g_silPrevF;               /* length of the last below-threshold run (any length), frames (UI) */
static volatile LONG64 g_silPrevEndMs;           /* when it ended (now_ms) */
static volatile LONG64 g_fileFrames;             /* PCM frames handed to the file/encoder this run */
/* level meter: loudest 10 ms RMS block since the UI last looked, in 1/100 dBFS + 20000 (0 = none) */
static volatile LONG g_rmsNow;
typedef struct { double sum; size_t n; BYTE carry[OUT_BPF]; size_t cn; LONG64 runF; } SilMeter;
static SilMeter g_sm;

typedef struct {
    LONG64  minF, padF;         /* frames */
    double  thrSq;              /* mean-square threshold (int16 units^2) */
    LONG64  runF;               /* frames in the current silent run */
    int     cutting;
    DWORD  *hold;               /* hold ring (one DWORD = one stereo int16 frame) */
    size_t  cap, head, cnt;
    DWORD   blk[GATE_BLOCK];    /* partial block accumulator */
    size_t  blkN;               /* frames in blk */
    BYTE    tail[OUT_BPF];      /* partial frame bytes (never expected) */
    size_t  tailN;
} Gate;
static Gate g_gate;

static void gate_free(void)
{
    free(g_gate.hold);
    memset(&g_gate, 0, sizeof(g_gate));
}

/* hold ring sized for the largest allowed settings (60 s + 5 s) so parameters can change mid-run */
#define GATE_CAP ((size_t)(60 + 5) * OUT_RATE + 4 * GATE_BLOCK)

static void gate_set_params(int minSec, int padMs, int db)
{
    double lin = pow(10.0, db / 20.0) * 32768.0;
    g_gateMinSec = minSec; g_gatePadMs = padMs; g_gateDb = db;
    g_gate.minF = (LONG64)minSec * OUT_RATE;
    g_gate.padF = (LONG64)padMs * OUT_RATE / 1000;
    g_gate.thrSq = lin * lin;
}

/* writer thread (or before it starts): make sure the hold ring exists */
static int gate_alloc(void)
{
    if (g_gate.hold) return 1;
    g_gate.hold = (DWORD *)malloc(GATE_CAP * sizeof(DWORD));
    if (!g_gate.hold) { log_msg(L"Silence cut unavailable: cannot allocate %.1f MB", GATE_CAP * 4.0 / 1e6); return 0; }
    g_gate.cap = GATE_CAP; g_gate.head = g_gate.cnt = 0;
    return 1;
}

/* UI thread: queue the current settings for the running recording */
static void gate_request_update(void)
{
    EnterCriticalSection(&g_gateReqLock);
    g_gateReqOn = g_silCut; g_gateReqMin = g_silMinSec; g_gateReqPad = g_silPadMs; g_gateReqDb = g_silDb;
    LeaveCriticalSection(&g_gateReqLock);
    InterlockedIncrement(&g_gateReqVer);
}

/* session start (UI thread, before the writer starts): snapshot settings */
static void gate_init(void)
{
    gate_free();
    g_cutFrames = 0; g_gateCutting = 0; g_gateCuts = 0;
    memset(&g_sm, 0, sizeof(g_sm));
    g_silRunF = 0; g_silLastF = 0; g_silLastEndMs = 0; g_silPrevF = 0; g_silPrevEndMs = 0; g_silMeterReset = 0; g_silEvents = 0; g_fileFrames = 0;
    g_gateAppliedVer = g_gateReqVer;
    gate_set_params(g_silMinSec, g_silPadMs, g_silDb);
    g_gateOn = g_silCut && gate_alloc();
    g_gateUsed = g_gateOn;
    if (g_gateOn)
        log_msg(L"Silence cut ON: below %d dBFS for more than %d s, keeping %d ms before/after",
                g_gateDb, g_gateMinSec, g_gatePadMs);
}

static void gate_count_cut(LONG64 n)
{
    if (n > 0) InterlockedExchangeAdd64(&g_cutFrames, n);
}

static LONG64 cut_total_ms(void) { return g_cutBaseMs + atomic_read64(&g_cutFrames) * 1000 / OUT_RATE; }

/* writer thread: classify 10 ms blocks with the SilenceDb threshold (same rule as the gate) */
static void sil_meter_frame(DWORD v)
{
    double l = (double)(short)(v & 0xFFFF), r = (double)(short)(v >> 16);
    g_sm.sum += l * l + r * r;
    if (++g_sm.n < GATE_BLOCK) return;
    {   /* same mean square the silence rule uses, as dBFS for the level meter */
        double ms = g_sm.sum / (2.0 * GATE_BLOCK);
        double db = ms > 0.0 ? 10.0 * log10(ms / (32768.0 * 32768.0)) : -120.0;
        LONG v, cur;
        if (db < -120.0) db = -120.0;
        if (db > 0.0) db = 0.0;
        v = (LONG)(db * 100.0 + 20000.5);
        cur = g_rmsNow;
        while (v > cur) {
            LONG prev = InterlockedCompareExchange(&g_rmsNow, v, cur);
            if (prev == cur) break;
            cur = prev;
        }
    }
    if (g_sm.sum / (2.0 * GATE_BLOCK) < g_gate.thrSq) g_sm.runF += GATE_BLOCK;
    else {
        if (g_sm.runF > 0) {               /* UI: counter hold + "Ultimo silenzio" */
            InterlockedExchange64(&g_silPrevF, g_sm.runF);
            InterlockedExchange64(&g_silPrevEndMs, now_ms());
        }
        if (g_sm.runF >= OUT_RATE) {
            InterlockedExchange64(&g_silLastF, g_sm.runF);
            InterlockedExchange64(&g_silLastEndMs, now_ms());
            InterlockedIncrement(&g_silEvents);
            log_msg(L"Silence (metered, below %d dBFS): %.1f s%ls", g_gateDb, (double)g_sm.runF / OUT_RATE,
                    g_gateOn ? L"" : L" - not cut");
        }
        g_sm.runF = 0;
    }
    InterlockedExchange64(&g_silRunF, g_sm.runF);
    g_sm.sum = 0.0; g_sm.n = 0;
}

static void sil_meter_feed(const BYTE *p, size_t n)
{
    if (g_silMeterReset && InterlockedExchange(&g_silMeterReset, 0)) {
        g_sm.runF = 0; g_sm.sum = 0.0; g_sm.n = 0;
        InterlockedExchange64(&g_silRunF, 0);
    }
    while (n && g_sm.cn) {
        g_sm.carry[g_sm.cn++] = *p++; n--;
        if (g_sm.cn == OUT_BPF) { DWORD v; memcpy(&v, g_sm.carry, 4); sil_meter_frame(v); g_sm.cn = 0; }
    }
    while (n >= OUT_BPF) { DWORD v; memcpy(&v, p, 4); sil_meter_frame(v); p += OUT_BPF; n -= OUT_BPF; }
    while (n) { g_sm.carry[g_sm.cn++] = *p++; n--; }
}

static BOOL gate_emit(WavOut *o, const DWORD *f, size_t n)
{
    if (!n) return TRUE;
    g_fileFrames += (LONG64)n;
    return wav_write(o, (const BYTE *)f, n * OUT_BPF);
}

/* emit the newest `keep` held frames, discard (count as cut) the older ones, empty the ring */
static BOOL gate_release(WavOut *o, size_t keep)
{
    Gate *g = &g_gate;
    BOOL ok = TRUE;
    if (keep > g->cnt) keep = g->cnt;
    gate_count_cut((LONG64)(g->cnt - keep));
    g->head = (g->head + (g->cnt - keep)) % g->cap;
    g->cnt = keep;
    while (ok && g->cnt) {
        size_t n = g->cap - g->head;
        if (n > g->cnt) n = g->cnt;
        ok = gate_emit(o, g->hold + g->head, n);
        g->head = (g->head + n) % g->cap;
        g->cnt -= n;
    }
    g->head = 0;
    return ok;
}

static void gate_hold(const DWORD *f, size_t n)
{
    Gate *g = &g_gate;
    while (n) {
        size_t tail = (g->head + g->cnt) % g->cap, room = g->cap - tail, k;
        if (g->cnt == g->cap) {                  /* cannot happen (capacity covers the max) */
            g->head = (g->head + 1) % g->cap; g->cnt--; gate_count_cut(1);
            continue;
        }
        k = n < room ? n : room;
        if (k > g->cap - g->cnt) k = g->cap - g->cnt;
        memcpy(g->hold + tail, f, k * sizeof(DWORD));
        g->cnt += k; f += k; n -= k;
    }
}

/* drop the oldest held frames so that at most `keep` remain */
static void gate_trim(size_t keep)
{
    Gate *g = &g_gate;
    if (g->cnt <= keep) return;
    gate_count_cut((LONG64)(g->cnt - keep));
    g->head = (g->head + (g->cnt - keep)) % g->cap;
    g->cnt = keep;
}

static BOOL gate_block(WavOut *o, const DWORD *f, size_t n)
{
    Gate *g = &g_gate;
    double sum = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        double l = (double)(short)(f[i] & 0xFFFF), r = (double)(short)(f[i] >> 16);
        sum += l * l + r * r;
    }
    if (n == 0) return TRUE;
    if (sum / (2.0 * (double)n) >= g->thrSq) {           /* sound */
        BOOL ok = TRUE;
        if (g->runF > 0) {
            if (g->cutting) {
                ok = gate_release(o, (size_t)g->padF);    /* lead-in before the sound */
                log_msg(L"Silence cut: removed %.1f s (total %.1f s)", (double)(g->runF - 2 * g->padF > 0 ? g->runF - 2 * g->padF : 0) / OUT_RATE,
                        (double)g_cutFrames / OUT_RATE);
            } else ok = gate_release(o, g->cnt);         /* short pause: keep all of it */
            g->runF = 0; g->cutting = 0;
            InterlockedExchange(&g_gateCutting, 0); InterlockedExchange64(&g_gateRunF, 0);
        }
        return ok && gate_emit(o, f, n);
    }
    /* silent block: the first padF frames of a run are written (pad after the last sound) */
    {
        BOOL ok = TRUE;
        size_t direct = 0;
        if (g->runF < g->padF) {
            LONG64 left = g->padF - g->runF;
            direct = (size_t)(left < (LONG64)n ? left : (LONG64)n);
            ok = gate_emit(o, f, direct);
        }
        gate_hold(f + direct, n - direct);
        g->runF += (LONG64)n;
        InterlockedExchange64(&g_gateRunF, g->runF);
        if (!g->cutting && g->runF > g->minF) {
            g->cutting = 1;
            InterlockedIncrement(&g_gateCuts);
            InterlockedExchange(&g_gateCutting, 1);
        }
        if (g->cutting) gate_trim((size_t)g->padF);
        return ok;
    }
}

/* writer thread: apply a settings change requested from the dialog */
static BOOL gate_apply(WavOut *o)
{
    Gate *g = &g_gate;
    BOOL ok = TRUE;
    int on, mn, pd, db;
    LONG ver = g_gateReqVer;
    if (ver == g_gateAppliedVer) return TRUE;
    g_gateAppliedVer = ver;
    EnterCriticalSection(&g_gateReqLock);
    on = g_gateReqOn; mn = g_gateReqMin; pd = g_gateReqPad; db = g_gateReqDb;
    LeaveCriticalSection(&g_gateReqLock);
    if (g_gateOn && !on) {
        /* turning off: write out everything still held back, then pass audio straight through */
        if (g->blkN) { ok = gate_block(o, g->blk, g->blkN); g->blkN = 0; }
        if (ok) ok = gate_release(o, g->cutting ? (size_t)g->padF : g->cnt);   /* mid-cut: keep the lead-in margin */
        if (ok && g->tailN) ok = wav_write(o, g->tail, g->tailN);
        g->tailN = 0; g->runF = 0; g->cutting = 0;
        InterlockedExchange(&g_gateCutting, 0); InterlockedExchange64(&g_gateRunF, 0);
        InterlockedExchange(&g_gateOn, 0);
        gate_set_params(mn, pd, db);
        log_msg(L"Silence cut turned OFF during the recording (%.1f s cut so far)", cut_total_ms() / 1000.0);
        return ok;
    }
    gate_set_params(mn, pd, db);           /* hold ring is sized for the maximum: safe mid-run */
    if (!g_gateOn && on) {
        if (!gate_alloc()) return TRUE;
        g->runF = 0; g->cutting = 0; g->head = g->cnt = 0; g->blkN = 0; g->tailN = 0;
        InterlockedExchange(&g_gateOn, 1);
        InterlockedExchange(&g_gateUsed, 1);
        log_msg(L"Silence cut turned ON during the recording: below %d dBFS for more than %d s, keeping %d ms",
                db, mn, pd);
    } else if (on) {
        if (g->cutting) gate_trim((size_t)g->padF);   /* a smaller margin takes effect at once */
        log_msg(L"Silence cut settings changed during the recording: %d dBFS, %d s, %d ms", db, mn, pd);
    }
    return ok;
}

/* writer thread: route ring data through the gate */
static BOOL gate_write(WavOut *o, const BYTE *p, size_t n)
{
    Gate *g = &g_gate;
    BOOL ok = TRUE;
    if (g_gateReqVer != g_gateAppliedVer && !gate_apply(o)) return FALSE;
    sil_meter_feed(p, n);
    if (!g_gateOn) { g_fileFrames += (LONG64)(n / OUT_BPF); return wav_write(o, p, n); }
    while (ok && g->tailN && n) {                /* complete a partial frame first */
        g->tail[g->tailN++] = *p++; n--;
        if (g->tailN == OUT_BPF) { DWORD v; memcpy(&v, g->tail, 4); g->blk[g->blkN++] = v; g->tailN = 0; }
        if (g->blkN == GATE_BLOCK) { ok = gate_block(o, g->blk, g->blkN); g->blkN = 0; }
    }
    while (ok && n >= OUT_BPF) {
        size_t want = GATE_BLOCK - g->blkN, have = n / OUT_BPF;
        if (g->blkN == 0 && have >= GATE_BLOCK && ((ULONG_PTR)p & 3) == 0) {
            ok = gate_block(o, (const DWORD *)p, GATE_BLOCK);   /* fast path, ring data is aligned */
            p += GATE_BLOCK * OUT_BPF; n -= GATE_BLOCK * OUT_BPF;
            continue;
        }
        if (want > have) want = have;
        memcpy(g->blk + g->blkN, p, want * OUT_BPF);
        g->blkN += want; p += want * OUT_BPF; n -= want * OUT_BPF;
        if (g->blkN == GATE_BLOCK) { ok = gate_block(o, g->blk, g->blkN); g->blkN = 0; }
    }
    while (n) { g->tail[g->tailN++] = *p++; n--; }
    return ok;
}

/* end of the run (stop or suspend): classify the partial block, settle the hold ring */
static BOOL gate_finish(WavOut *o)
{
    Gate *g = &g_gate;
    BOOL ok = TRUE;
    if (g_gateReqVer != g_gateAppliedVer && !gate_apply(o)) return FALSE;
    if (!g_gateOn) return TRUE;
    if (g->blkN) { ok = gate_block(o, g->blk, g->blkN); g->blkN = 0; }
    if (g->cutting) gate_release(o, 0);          /* trailing silence of a cut: drop it */
    else if (ok) ok = gate_release(o, g->cnt);   /* short pause: keep */
    g->runF = 0; g->cutting = 0;
    InterlockedExchange(&g_gateCutting, 0); InterlockedExchange64(&g_gateRunF, 0);
    return ok;
}

static void rename_apply(WavOut *o);
static volatile LONG g_renReqVer;             /* live rename request (UI) */
static LONG  g_renAppliedVer;                 /* writer thread */
static unsigned __stdcall writer_thread(void *arg)
{
    WavOut *o = (WavOut *)arg;
    LONG64 lastHdr = now_ms();
    BOOL ok = TRUE;
    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* live AAC encoder runs here */
    for (;;) {
        /* sample "capture finished" BEFORE draining so nothing is left behind */
        BOOL finished = (WaitForSingleObject(g_captureThread, 0) == WAIT_OBJECT_0);
        LONG64 w = atomic_read64(&g_ringW), r = g_ringR;
        while (ok && w > r) {
            size_t off = (size_t)(r % RING_BYTES);
            size_t n = (size_t)(w - r);
            if (n > RING_BYTES - off) n = RING_BYTES - off;
            if (n > 1024 * 1024) n = 1024 * 1024;
            ok = gate_write(o, g_ring + off, n);
            r += (LONG64)n;
            InterlockedExchange64(&g_ringR, r);
        }
        if (!ok) {
            /* disk full / I/O error: keep consuming so capture never blocks, stop recording */
            InterlockedExchange(&g_writeError, 1);
            request_stop(4);
            InterlockedExchange64(&g_ringR, w);
        }
        if (finished) {
            if (ok && !gate_finish(o)) { InterlockedExchange(&g_writeError, 1); request_stop(4); }
            break;
        }
        if (ok && g_renReqVer != g_renAppliedVer) rename_apply(o);   /* live rename (writer owns the handles) */
        if (ok && o->live) live_tick(o);
        else if (ok && now_ms() - lastHdr >= HEADER_EVERY_MS) {
            lastHdr = now_ms();
            if (!wav_header(o)) LOG_HR(L"Updating WAV header", HRESULT_FROM_WIN32(GetLastError()));
            FlushFileBuffers(o->h);
        }
        WaitForSingleObject(g_captureThread, 50);
    }
    wav_close(o);
    if (SUCCEEDED(coHr)) CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Format conversion + resampling (capture thread)                     */
/* ------------------------------------------------------------------ */
enum { SF_F32, SF_F64, SF_I16, SF_I24, SF_I32, SF_U8 };

typedef struct {
    int    ch, rate, sf, bps, blockAlign;
    /* resampler state */
    double step, pos;
    float  pl, pr;
    /* scratch */
    float *tmp;  size_t tmpCap;     /* stereo float, frames */
    short *out;  size_t outCap;     /* stereo int16, frames */
} Conv;

static int parse_format(const WAVEFORMATEX *wf, Conv *c, WCHAR *desc, size_t cch)
{
    WORD tag = wf->wFormatTag;
    int isFloat;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)wf;
        if (guid_eq(&x->SubFormat, &MY_SUBTYPE_IEEE_FLOAT)) isFloat = 1;
        else if (guid_eq(&x->SubFormat, &MY_SUBTYPE_PCM)) isFloat = 0;
        else return 0;
    } else if (tag == WAVE_FORMAT_IEEE_FLOAT) isFloat = 1;
    else if (tag == WAVE_FORMAT_PCM) isFloat = 0;
    else return 0;

    c->ch = wf->nChannels; c->rate = (int)wf->nSamplesPerSec;
    c->bps = wf->wBitsPerSample / 8; c->blockAlign = wf->nBlockAlign;
    if (c->ch < 1 || c->rate < 1000 || c->rate > 1536000 || c->blockAlign < c->ch * c->bps) return 0;
    if (isFloat)      { if (c->bps == 4) c->sf = SF_F32; else if (c->bps == 8) c->sf = SF_F64; else return 0; }
    else switch (c->bps) {
        case 1: c->sf = SF_U8; break;
        case 2: c->sf = SF_I16; break;
        case 3: c->sf = SF_I24; break;
        case 4: c->sf = SF_I32; break;
        default: return 0;
    }
    c->step = (double)c->rate / OUT_RATE;
    c->pos = 0.0; c->pl = c->pr = 0.0f;
    _snwprintf(desc, cch - 1, L"%d Hz, %d ch, %d-bit %ls", c->rate, c->ch, wf->wBitsPerSample,
               isFloat ? L"float" : L"int");
    desc[cch - 1] = 0;
    return 1;
}

static float rd_sample(const BYTE *p, int sf)
{
    switch (sf) {
    case SF_F32: { float f; memcpy(&f, p, 4); return f; }
    case SF_F64: { double d; memcpy(&d, p, 8); return (float)d; }
    case SF_I16: { short s; memcpy(&s, p, 2); return s / 32768.0f; }
    case SF_I24: { int v = (int)((unsigned)p[0] << 8 | (unsigned)p[1] << 16 | (unsigned)p[2] << 24); return (float)(v / 2147483648.0); }
    case SF_I32: { int v; memcpy(&v, p, 4); return (float)(v / 2147483648.0); }
    case SF_U8:  return (p[0] - 128) / 128.0f;
    }
    return 0.0f;
}

static int ensure_cap(void **buf, size_t *cap, size_t need, size_t elem)
{
    if (*cap >= need) return 1;
    {
        void *nb = realloc(*buf, need * elem);
        if (!nb) return 0;
        *buf = nb; *cap = need;
    }
    return 1;
}

/* input -> stereo float in c->tmp; returns peak abs value */
static float to_stereo(Conv *c, const BYTE *data, UINT32 frames, int silent)
{
    UINT32 i;
    float peak = 0.0f;
    const float k = 0.70710678f;
    if (silent || !data) { memset(c->tmp, 0, frames * 2 * sizeof(float)); return 0.0f; }
    for (i = 0; i < frames; i++) {
        const BYTE *f = data + (size_t)i * c->blockAlign;
        float L, R;
        if (c->ch == 1) {
            L = R = rd_sample(f, c->sf);
        } else {
            /* WAVEFORMATEXTENSIBLE standard order: FL FR FC LFE BL BR SL SR */
            float norm = 1.0f;
            L = rd_sample(f, c->sf);
            R = rd_sample(f + c->bps, c->sf);
            if (c->ch >= 3) { float s = rd_sample(f + 2 * c->bps, c->sf) * k; L += s; R += s; norm += k; }
            if (c->ch >= 6) { L += rd_sample(f + 4 * c->bps, c->sf) * k; R += rd_sample(f + 5 * c->bps, c->sf) * k; norm += k; }
            else if (c->ch == 5) { L += rd_sample(f + 3 * c->bps, c->sf) * k; R += rd_sample(f + 4 * c->bps, c->sf) * k; norm += k; }
            if (c->ch >= 8) { L += rd_sample(f + 6 * c->bps, c->sf) * k; R += rd_sample(f + 7 * c->bps, c->sf) * k; norm += k; }
            if (norm > 1.0f) { L /= norm; R /= norm; }
        }
        if (L != L) L = 0.0f;           /* NaN guard */
        if (R != R) R = 0.0f;
        c->tmp[2 * i] = L; c->tmp[2 * i + 1] = R;
        if (L < 0) L = -L;
        if (R < 0) R = -R;
        if (L > peak) peak = L;
        if (R > peak) peak = R;
    }
    return peak;
}

static short f2s(float x)
{
    if (x >= 1.0f) return 32767;
    if (x <= -1.0f) return -32768;
    return (short)(x * 32767.0f + (x >= 0 ? 0.5f : -0.5f));
}

/* c->tmp (frames @ c->rate) -> c->out (@ 48k); returns output frames */
static size_t resample(Conv *c, UINT32 frames)
{
    size_t n = 0;
    UINT32 i;
    if (c->rate == OUT_RATE) {
        for (i = 0; i < frames; i++) { c->out[2 * i] = f2s(c->tmp[2 * i]); c->out[2 * i + 1] = f2s(c->tmp[2 * i + 1]); }
        return frames;
    }
    for (i = 0; i < frames; i++) {
        float L = c->tmp[2 * i], R = c->tmp[2 * i + 1];
        while (c->pos < 1.0) {
            float t = (float)c->pos;
            c->out[2 * n]     = f2s(c->pl + (L - c->pl) * t);
            c->out[2 * n + 1] = f2s(c->pr + (R - c->pr) * t);
            n++;
            c->pos += c->step;
        }
        c->pos -= 1.0;
        c->pl = L; c->pr = R;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Timeline helpers (capture thread)                                   */
/* ------------------------------------------------------------------ */
/* ---- pause: requested by the UI, applied by the capture thread ---- */
static volatile LONG g_pauseReq;      /* UI: 1 = paused */
static volatile LONG g_pausedAck;     /* capture thread is discarding audio */
static LONG64 g_pauseStart100;        /* capture thread only */
static LONG64 g_pausedTotalMs, g_pauseStartMs;   /* UI thread: this run */
static LONG64 g_pausedBaseMs;                    /* paused time in earlier runs of a resumed session */

/* manual pause time of the whole session (earlier runs + this run + the pause in progress) */
static LONG64 paused_total_ms(void)
{
    LONG64 p = g_pausedBaseMs + g_pausedTotalMs, st = g_pauseStartMs;
    if (g_pauseReq && st) p += now_ms() - st;
    return p;
}

/* capture thread: returns 1 while paused. On resume the timeline origin is moved forward by
   the paused time, so no silence is synthesized for the pause and the recording continues
   seamlessly. */
static int pause_active(void)
{
    if (g_pauseReq) {
        if (!g_pausedAck) { g_pauseStart100 = now_100ns(); InterlockedExchange(&g_pausedAck, 1); log_msg(L"Paused"); }
        return 1;
    }
    if (g_pausedAck) {
        LONG64 d = now_100ns() - g_pauseStart100;
        InterlockedExchangeAdd64(&g_start100ns, d);
        InterlockedExchange(&g_pausedAck, 0);
        log_msg(L"Resumed after %.1f s of pause", d / 1e7);
    }
    return 0;
}

static void push_frames(const short *pcm, size_t frames)
{
    size_t put = ring_put((const BYTE *)pcm, frames * OUT_BPF) / OUT_BPF;
    InterlockedExchangeAdd64(&g_framesOut, (LONG64)put);
    if (put < frames) InterlockedExchangeAdd64(&g_framesDropped, (LONG64)(frames - put));
}

/* append up to `frames` of silence (bounded by ring space; remainder is
   picked up on the next call because the deficit is recomputed) */
static void pad_silence(LONG64 frames, Conv *c)
{
    size_t put;
    if (frames <= 0) return;
    if ((ULONG64)frames > RING_BYTES / OUT_BPF) frames = RING_BYTES / OUT_BPF;
    put = ring_put(NULL, (size_t)frames * OUT_BPF) / OUT_BPF;
    InterlockedExchangeAdd64(&g_framesOut, (LONG64)put);
    InterlockedExchangeAdd64(&g_framesPadded, (LONG64)put);
    if (c) { c->pl = c->pr = 0.0f; }
}

/* fill silence up to "now - slack" on the wall-clock timeline */
static void pad_to_now(Conv *c)
{
    LONG64 target = t100_to_frames(now_100ns()) - MS_TO_FRAMES(IDLE_SLACK_MS);
    pad_silence(target - g_framesOut, c);
}

/* ------------------------------------------------------------------ */
/* Device session (capture thread)                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    IMMDeviceEnumerator *en;
    IMMDevice           *dev;
    LPWSTR               id;
    WAVEFORMATEX        *wfx;
    IAudioClient        *cap;
    IAudioCaptureClient *capc;
    IAudioClient        *ren;
    IAudioRenderClient  *renc;
    UINT32               renFrames;
    int                  capStarted, renStarted;
    int                  proc;          /* 1 = process loopback of one app */
    DWORD                pid;           /* its root process */
    HANDLE               hProc, evt;
} Dev;

#define SAFE_RELEASE(p, T) do { if (p) { T##_Release(p); (p) = NULL; } } while (0)

static void dev_close(Dev *d)
{
    if (d->capStarted && d->cap) IAudioClient_Stop(d->cap);
    if (d->renStarted && d->ren) IAudioClient_Stop(d->ren);
    d->capStarted = d->renStarted = 0;
    SAFE_RELEASE(d->renc, IAudioRenderClient);
    SAFE_RELEASE(d->ren,  IAudioClient);
    SAFE_RELEASE(d->capc, IAudioCaptureClient);
    SAFE_RELEASE(d->cap,  IAudioClient);
    SAFE_RELEASE(d->dev,  IMMDevice);
    SAFE_RELEASE(d->en,   IMMDeviceEnumerator);
    if (d->wfx) { CoTaskMemFree(d->wfx); d->wfx = NULL; }
    if (d->id)  { CoTaskMemFree(d->id);  d->id = NULL; }
    if (d->hProc) { CloseHandle(d->hProc); d->hProc = NULL; }
    if (d->evt)   { CloseHandle(d->evt);   d->evt = NULL; }
    d->renFrames = 0;
}

static void dev_name(IMMDevice *dev, WCHAR *out, size_t cch)
{
    IPropertyStore *ps = NULL;
    PROPVARIANT pv;
    wcsncpy(out, L"(unknown device)", cch - 1); out[cch - 1] = 0;
    if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) return;
    PropVariantInit(&pv);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &MY_PKEY_Device_FriendlyName, &pv)) &&
        pv.vt == VT_LPWSTR && pv.pwszVal) {
        wcsncpy(out, pv.pwszVal, cch - 1); out[cch - 1] = 0;
    }
    PropVariantClear(&pv);
    IPropertyStore_Release(ps);
}

/* keep-alive: top the render buffer up with silence */
static HRESULT keepalive(Dev *d)
{
    UINT32 pad = 0, avail;
    BYTE *buf = NULL;
    HRESULT hr;
    if (!d->renc) return S_OK;
    hr = IAudioClient_GetCurrentPadding(d->ren, &pad);
    if (FAILED(hr)) return hr;
    avail = d->renFrames - pad;
    if (avail == 0) return S_OK;
    hr = IAudioRenderClient_GetBuffer(d->renc, avail, &buf);
    if (FAILED(hr)) return hr;
    return IAudioRenderClient_ReleaseBuffer(d->renc, avail, AUDCLNT_BUFFERFLAGS_SILENT);
}

static HRESULT dev_open(Dev *d, Conv *c)
{
    HRESULT hr;
    WCHAR name[256], desc[128];
    REFERENCE_TIME hnsReq = CAPTURE_BUF_HNS;

    memset(d, 0, sizeof(*d));
    hr = CoCreateInstance(&MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &MY_IID_IMMDeviceEnumerator, (void **)&d->en);
    if (FAILED(hr)) { LOG_HR(L"CoCreateInstance(MMDeviceEnumerator)", hr); goto fail; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(d->en, eRender, eConsole, &d->dev);
    if (FAILED(hr)) { LOG_HR(L"GetDefaultAudioEndpoint", hr); goto fail; }
    hr = IMMDevice_GetId(d->dev, &d->id);
    if (FAILED(hr)) { LOG_HR(L"IMMDevice::GetId", hr); goto fail; }
    dev_name(d->dev, name, 256);

    hr = IMMDevice_Activate(d->dev, &MY_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&d->cap);
    if (FAILED(hr)) { LOG_HR(L"Activate(IAudioClient capture)", hr); goto fail; }
    hr = IAudioClient_GetMixFormat(d->cap, &d->wfx);
    if (FAILED(hr)) { LOG_HR(L"GetMixFormat", hr); goto fail; }
    if (!parse_format(d->wfx, c, desc, 128)) {
        log_msg(L"Unsupported mix format (tag 0x%04X, %u bits, %u ch)", d->wfx->wFormatTag,
                d->wfx->wBitsPerSample, d->wfx->nChannels);
        hr = AUDCLNT_E_UNSUPPORTED_FORMAT; goto fail;
    }
    hr = IAudioClient_Initialize(d->cap, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                 hnsReq, 0, d->wfx, NULL);
    if (FAILED(hr)) { LOG_HR(L"IAudioClient::Initialize(loopback)", hr); goto fail; }
    {
        UINT32 bufFrames = 0;
        hr = IAudioClient_GetBufferSize(d->cap, &bufFrames);
        if (FAILED(hr)) { LOG_HR(L"GetBufferSize(capture)", hr); goto fail; }
        if (!ensure_cap((void **)&c->tmp, &c->tmpCap, (size_t)bufFrames * 2 + 64, 2 * sizeof(float)) ||
            !ensure_cap((void **)&c->out, &c->outCap,
                        (size_t)((double)bufFrames * 2 * OUT_RATE / c->rate) + 64, 2 * sizeof(short))) {
            hr = E_OUTOFMEMORY; LOG_HR(L"Allocating buffers", hr); goto fail;
        }
    }
    hr = IAudioClient_GetService(d->cap, &MY_IID_IAudioCaptureClient, (void **)&d->capc);
    if (FAILED(hr)) { LOG_HR(L"GetService(IAudioCaptureClient)", hr); goto fail; }

    /* keep-alive render stream: non-fatal if it fails */
    {
        HRESULT h2 = IMMDevice_Activate(d->dev, &MY_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&d->ren);
        if (SUCCEEDED(h2)) h2 = IAudioClient_Initialize(d->ren, AUDCLNT_SHAREMODE_SHARED, 0, RENDER_BUF_HNS, 0, d->wfx, NULL);
        if (SUCCEEDED(h2)) h2 = IAudioClient_GetBufferSize(d->ren, &d->renFrames);
        if (SUCCEEDED(h2)) h2 = IAudioClient_GetService(d->ren, &MY_IID_IAudioRenderClient, (void **)&d->renc);
        if (SUCCEEDED(h2)) h2 = keepalive(d);
        if (SUCCEEDED(h2)) { h2 = IAudioClient_Start(d->ren); if (SUCCEEDED(h2)) d->renStarted = 1; }
        if (FAILED(h2)) {
            LOG_HR(L"Silent keep-alive stream (continuing without it)", h2);
            SAFE_RELEASE(d->renc, IAudioRenderClient);
            SAFE_RELEASE(d->ren, IAudioClient);
            d->renFrames = 0;
        }
    }

    hr = IAudioClient_Start(d->cap);
    if (FAILED(hr)) { LOG_HR(L"IAudioClient::Start(capture)", hr); goto fail; }
    d->capStarted = 1;

    EnterCriticalSection(&g_statusLock);
    wcsncpy(g_devName, name, 255); g_devName[255] = 0;
    wcsncpy(g_fmtDesc, desc, 127); g_fmtDesc[127] = 0;
    LeaveCriticalSection(&g_statusLock);
    log_msg(L"Capturing: %ls  [%ls]%ls", name, desc, d->renc ? L"" : L" (no keep-alive)");
    return S_OK;
fail:
    dev_close(d);
    return FAILED(hr) ? hr : E_FAIL;
}

/* is the default render device still the one we are capturing? */
static int default_still_same(Dev *d)
{
    IMMDevice *def = NULL;
    LPWSTR id = NULL;
    int same = 0;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(d->en, eRender, eConsole, &def))) {
        if (SUCCEEDED(IMMDevice_GetId(def, &id))) {
            same = (wcscmp(id, d->id) == 0);
            CoTaskMemFree(id);
        }
        IMMDevice_Release(def);
    }
    return same;
}

/* ------------------------------------------------------------------ */
/* Source = one app: process helpers + process loopback (1.4)          */
/* ------------------------------------------------------------------ */
static WCHAR g_curSrcExe[64], g_curSrcName[128];   /* this session: "" = whole system */
static WCHAR g_startSrcExe[64], g_startSrcName[128]; /* source for the next new session (set by the UI / test) */
static int   g_testMode;             /* headless test: never touch the GUI's log/settings/session */
static int   g_openSrcMenu;          /* --open-source-menu: open the picker at start (screenshots) */
static volatile LONG g_srcState;     /* 0 system, 1 app attached, 2 waiting for the app, 3 fell back to system */
static volatile LONG g_srcEverOk;    /* app was attached at least once in this run */
static volatile LONG g_srcNotice;    /* UI: show "process loopback unavailable" once */
static volatile LONG g_srcAttaches, g_srcActFails, g_srcExits;
static volatile LONG g_srcPid;
static HRESULT g_srcLastHr;
/* 1.4.1: live source switch. The UI (or the test) posts a request; the capture thread applies it */
static WCHAR g_reqSrcExe[64], g_reqSrcName[128];   /* protected by g_statusLock */
static volatile LONG g_srcSwitchReq;  /* 1 = a request is waiting */
static volatile LONG g_srcSwitches;   /* switches applied in this run */
static HANDLE g_srcSwitchEvent;       /* manual reset: wakes the capture thread */
static int src_switch_apply(void);    /* capture thread (defined after the session helpers) */
static void src_switch_request(const WCHAR *exe, const WCHAR *name)
{
    EnterCriticalSection(&g_statusLock);
    wcsncpy(g_reqSrcExe, exe ? exe : L"", 63); g_reqSrcExe[63] = 0;
    wcsncpy(g_reqSrcName, (exe && exe[0] && name) ? name : L"", 127); g_reqSrcName[127] = 0;
    LeaveCriticalSection(&g_statusLock);
    InterlockedExchange(&g_srcSwitchReq, 1);
    if (g_srcSwitchEvent) SetEvent(g_srcSwitchEvent);
}

typedef struct { DWORD pid, ppid; WCHAR exe[64]; } PInfo;

static int proc_snap(PInfo **out)
{
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    int n = 0, cap = 0;
    PInfo *p = NULL;
    *out = NULL;
    if (sn == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(sn, &pe)) do {
        if (n == cap) {
            PInfo *np = (PInfo *)realloc(p, (size_t)(cap = cap ? cap * 2 : 256) * sizeof(PInfo));
            if (!np) break;
            p = np;
        }
        p[n].pid = pe.th32ProcessID; p[n].ppid = pe.th32ParentProcessID;
        wcsncpy(p[n].exe, pe.szExeFile, 63); p[n].exe[63] = 0;
        n++;
    } while (Process32NextW(sn, &pe));
    CloseHandle(sn);
    *out = p;
    return n;
}
static int pi_find(const PInfo *p, int n, DWORD pid)
{
    int i;
    for (i = 0; i < n; i++) if (p[i].pid == pid) return i;
    return -1;
}
static ULONG64 proc_ctime(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    FILETIME c, e, k, u;
    ULONG64 t = 0;
    if (!h) return 0;
    if (GetProcessTimes(h, &c, &e, &k, &u)) t = ((ULONG64)c.dwHighDateTime << 32) | c.dwLowDateTime;
    CloseHandle(h);
    return t;
}
/* processes that are never "the app" (a WebView2 started by them stays its own app) */
static int proc_is_shell(const WCHAR *exe)
{
    static const WCHAR *k[] = { L"explorer.exe", L"svchost.exe", L"services.exe", L"sihost.exe", L"runtimebroker.exe",
        L"wininit.exe", L"winlogon.exe", L"applicationframehost.exe", L"dllhost.exe", L"taskhostw.exe", L"system", NULL };
    int i;
    for (i = 0; k[i]; i++) if (!_wcsicmp(exe, k[i])) return 1;
    return 0;
}
/* hidden / system processes left out of the picker */
static int proc_hidden(const WCHAR *exe)
{
    static const WCHAR *k[] = { L"loopback_rec_gui.exe", L"audiodg.exe", L"shellexperiencehost.exe", L"searchhost.exe",
        L"startmenuexperiencehost.exe", L"textinputhost.exe", L"lockapp.exe", L"systemsettings.exe", L"ctfmon.exe",
        L"searchapp.exe", L"widgets.exe", L"phoneexperiencehost.exe", L"csrss.exe", L"dwm.exe", NULL };
    int i;
    if (proc_is_shell(exe)) return 1;
    for (i = 0; k[i]; i++) if (!_wcsicmp(exe, k[i])) return 1;
    return 0;
}
/* root of an app: walk up while the parent is the same exe (browser -> its renderer/audio
   children), and past a WebView2 to the app that hosts it (e.g. the new Teams). Creation
   times guard against a recycled parent PID. */
static int proc_root_idx(const PInfo *p, int n, int i)
{
    int depth;
    if (i < 0) return i;
    for (depth = 0; depth < 32; depth++) {
        int j = pi_find(p, n, p[i].ppid), webview;
        ULONG64 cj, ci;
        if (j < 0 || j == i || p[i].ppid == 0) break;
        webview = !_wcsicmp(p[i].exe, L"msedgewebview2.exe");
        if (_wcsicmp(p[j].exe, p[i].exe) && !(webview && !proc_is_shell(p[j].exe))) break;
        cj = proc_ctime(p[j].pid); ci = proc_ctime(p[i].pid);
        if (!cj || (ci && cj > ci)) break;
        i = j;
    }
    return i;
}
/* friendly app name: FileDescription of the exe ("Google Chrome"), else the file name */
static void proc_friendly(DWORD pid, const WCHAR *exe, WCHAR *out, size_t cch)
{
    WCHAR path[MAX_PATH * 2];
    DWORD len = MAX_PATH * 2, dummy = 0, vs;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    size_t L;
    out[0] = 0;
    if (h) {
        if (QueryFullProcessImageNameW(h, 0, path, &len) && (vs = GetFileVersionInfoSizeW(path, &dummy)) > 0) {
            BYTE *vi = (BYTE *)malloc(vs);
            if (vi && GetFileVersionInfoW(path, 0, vs, vi)) {
                struct { WORD lang, cp; } *tr = NULL;
                UINT tl = 0, dl = 0;
                WCHAR q[64], *desc = NULL;
                if (VerQueryValueW(vi, L"\\VarFileInfo\\Translation", (void **)&tr, &tl) && tl >= 4) {
                    _snwprintf(q, 63, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp); q[63] = 0;
                    if (!VerQueryValueW(vi, q, (void **)&desc, &dl)) desc = NULL;
                }
                if (!desc || !dl || !desc[0]) {
                    if (!VerQueryValueW(vi, L"\\StringFileInfo\\040904b0\\FileDescription", (void **)&desc, &dl)) desc = NULL;
                }
                if (desc && dl && desc[0]) { wcsncpy(out, desc, cch - 1); out[cch - 1] = 0; }
            }
            free(vi);
        }
        CloseHandle(h);
    }
    /* trim, and fall back to the file name without ".exe" */
    L = wcslen(out);
    while (L && out[L - 1] == L' ') out[--L] = 0;
    if (!out[0] || L > 60) {
        wcsncpy(out, exe, cch - 1); out[cch - 1] = 0;
        L = wcslen(out);
        if (L > 4 && !_wcsicmp(out + L - 4, L".exe")) out[L - 4] = 0;
    }
}
/* PIDs of all audio sessions on the active render devices (COM must be initialized) */
static int sess_pids(DWORD *pids, int *active, int max)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDeviceCollection *col = NULL;
    UINT nd = 0, di;
    int n = 0;
    if (FAILED(CoCreateInstance(&MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &MY_IID_IMMDeviceEnumerator, (void **)&en))) return 0;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) IMMDeviceCollection_GetCount(col, &nd);
    for (di = 0; di < nd && n < max; di++) {
        IMMDevice *dev = NULL;
        IAudioSessionManager2 *sm = NULL;
        IAudioSessionEnumerator *se = NULL;
        int cnt = 0, k;
        if (FAILED(IMMDeviceCollection_Item(col, di, &dev))) continue;
        if (SUCCEEDED(IMMDevice_Activate(dev, &MY_IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&sm)) &&
            SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(sm, &se)) && SUCCEEDED(IAudioSessionEnumerator_GetCount(se, &cnt))) {
            for (k = 0; k < cnt && n < max; k++) {
                IAudioSessionControl *sc = NULL;
                IAudioSessionControl2 *s2 = NULL;
                DWORD pid = 0;
                AudioSessionState st = AudioSessionStateInactive;
                if (FAILED(IAudioSessionEnumerator_GetSession(se, k, &sc))) continue;
                if (SUCCEEDED(IAudioSessionControl_QueryInterface(sc, &MY_IID_IAudioSessionControl2, (void **)&s2))) {
                    if (IAudioSessionControl2_IsSystemSoundsSession(s2) != S_OK &&
                        SUCCEEDED(IAudioSessionControl2_GetProcessId(s2, &pid)) && pid &&
                        SUCCEEDED(IAudioSessionControl_GetState(sc, &st)) && st != AudioSessionStateExpired) {
                        int j, dup = 0;
                        for (j = 0; j < n; j++) if (pids[j] == pid) { dup = 1; if (st == AudioSessionStateActive) active[j] = 1; }
                        if (!dup) { pids[n] = pid; active[n] = st == AudioSessionStateActive; n++; }
                    }
                    IAudioSessionControl2_Release(s2);
                }
                IAudioSessionControl_Release(sc);
            }
        }
        if (se) IAudioSessionEnumerator_Release(se);
        if (sm) IAudioSessionManager2_Release(sm);
        IMMDevice_Release(dev);
    }
    if (col) IMMDeviceCollection_Release(col);
    IMMDeviceEnumerator_Release(en);
    return n;
}

/* current root PID of an app by exe name (0 = not running). With several instances the
   one playing audio wins, then the oldest. */
static DWORD src_find_root(const WCHAR *exe)
{
    PInfo *p = NULL;
    int n = proc_snap(&p), i, nr = 0, k;
    int roots[16];
    DWORD best = 0;
    if (!exe[0] || n <= 0) { free(p); return 0; }
    for (i = 0; i < n; i++) {
        int r, dup = 0;
        if (_wcsicmp(p[i].exe, exe) && _wcsicmp(p[i].exe, L"msedgewebview2.exe")) continue;
        r = proc_root_idx(p, n, i);
        if (r < 0 || _wcsicmp(p[r].exe, exe)) continue;
        for (k = 0; k < nr; k++) if (roots[k] == r) dup = 1;
        if (!dup && nr < 16) roots[nr++] = r;
    }
    if (nr == 1) best = p[roots[0]].pid;
    else if (nr > 1) {
        DWORD sp[64]; int sa[64], ns = sess_pids(sp, sa, 64), j, bestScore = -1;
        ULONG64 bestT = 0;
        for (k = 0; k < nr; k++) {
            int score = 0;
            ULONG64 t;
            for (j = 0; j < ns; j++) {
                int si = pi_find(p, n, sp[j]);
                if (si >= 0 && proc_root_idx(p, n, si) == roots[k]) score = sa[j] ? 2 : (score > 1 ? score : 1);
            }
            t = proc_ctime(p[roots[k]].pid);
            if (score > bestScore || (score == bestScore && t && (!bestT || t < bestT))) { bestScore = score; bestT = t; best = p[roots[k]].pid; }
        }
    }
    free(p);
    return best;
}

/* ---- ActivateAudioInterfaceAsync on VAD\Process_Loopback ---- */
typedef struct { DWORD TargetProcessId; int ProcessLoopbackMode; } MY_PROCESS_LOOPBACK_PARAMS;
typedef struct { int ActivationType; MY_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams; } MY_ACTIVATION_PARAMS;
#define MY_ACTIVATION_TYPE_PROCESS_LOOPBACK 1
#define MY_PROCESS_LOOPBACK_MODE_INCLUDE_TREE 0
typedef HRESULT (WINAPI *PFN_ActivateAudioInterfaceAsync)(LPCWSTR, REFIID, PROPVARIANT *,
    IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **);

typedef struct { IActivateAudioInterfaceCompletionHandler iface; volatile LONG ref; HANDLE done; } ActHandler;
static HRESULT STDMETHODCALLTYPE ah_QueryInterface(IActivateAudioInterfaceCompletionHandler *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (guid_eq(riid, &MY_IID_IUnknown) || guid_eq(riid, &MY_IID_IActivateCompletion) || guid_eq(riid, &MY_IID_IAgileObject)) {
        *ppv = This; InterlockedIncrement(&((ActHandler *)This)->ref); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ah_AddRef(IActivateAudioInterfaceCompletionHandler *This) { return (ULONG)InterlockedIncrement(&((ActHandler *)This)->ref); }
static ULONG STDMETHODCALLTYPE ah_Release(IActivateAudioInterfaceCompletionHandler *This)
{
    ActHandler *a = (ActHandler *)This;
    LONG r = InterlockedDecrement(&a->ref);
    if (r == 0) { CloseHandle(a->done); free(a); }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE ah_ActivateCompleted(IActivateAudioInterfaceCompletionHandler *This, IActivateAudioInterfaceAsyncOperation *op)
{
    (void)op;
    SetEvent(((ActHandler *)This)->done);
    return S_OK;
}
static IActivateAudioInterfaceCompletionHandlerVtbl g_ahVtbl = { ah_QueryInterface, ah_AddRef, ah_Release, ah_ActivateCompleted };

static int g_forceProcFail;          /* test: --force-proc-fail */
static HRESULT proc_activate(DWORD pid, IAudioClient **out)
{
    static PFN_ActivateAudioInterfaceAsync fn;
    static int loaded;
    MY_ACTIVATION_PARAMS ap;
    PROPVARIANT pv;
    ActHandler *a;
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    HRESULT hr;
    *out = NULL;
    if (g_forceProcFail) return E_NOTIMPL;
    if (!loaded) {
        HMODULE m = LoadLibraryExW(L"mmdevapi.dll", NULL, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
        if (!m) m = LoadLibraryW(L"mmdevapi.dll");
        fn = m ? (PFN_ActivateAudioInterfaceAsync)(void (*)(void))GetProcAddress(m, "ActivateAudioInterfaceAsync") : NULL;
        loaded = 1;
    }
    if (!fn) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    memset(&ap, 0, sizeof(ap));
    ap.ActivationType = MY_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = pid;
    ap.ProcessLoopbackParams.ProcessLoopbackMode = MY_PROCESS_LOOPBACK_MODE_INCLUDE_TREE;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(ap); pv.blob.pBlobData = (BYTE *)&ap;
    a = (ActHandler *)calloc(1, sizeof(*a));
    if (!a) return E_OUTOFMEMORY;
    a->iface.lpVtbl = &g_ahVtbl; a->ref = 1;
    a->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!a->done) { free(a); return E_OUTOFMEMORY; }
    hr = fn(L"VAD\\Process_Loopback", &MY_IID_IAudioClient, &pv, &a->iface, &op);
    if (SUCCEEDED(hr) && op) {
        if (WaitForSingleObject(a->done, 5000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        else {
            HRESULT ar = E_FAIL;
            IUnknown *u = NULL;
            hr = IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &ar, &u);
            if (SUCCEEDED(hr)) hr = ar;
            if (SUCCEEDED(hr)) hr = u ? IUnknown_QueryInterface(u, &MY_IID_IAudioClient, (void **)out) : E_NOINTERFACE;
            if (u) IUnknown_Release(u);
        }
        IActivateAudioInterfaceAsyncOperation_Release(op);
    } else if (SUCCEEDED(hr)) hr = E_FAIL;
    ah_Release(&a->iface);
    return hr;
}

/* open a capture of one app (its whole process tree). Process loopback has no mix format:
   ask for 48 kHz / 16-bit / stereo and let the audio engine convert. */
static HRESULT proc_open(Dev *d, Conv *c, DWORD pid)
{
    HRESULT hr;
    WCHAR desc[128], name[256];
    UINT32 bufFrames = 0;
    WAVEFORMATEX *w;
    memset(d, 0, sizeof(*d));
    d->proc = 1; d->pid = pid;
    d->hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    hr = proc_activate(pid, &d->cap);
    if (FAILED(hr)) { LOG_HR(L"ActivateAudioInterfaceAsync(process loopback)", hr); goto fail; }
    w = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (!w) { hr = E_OUTOFMEMORY; goto fail; }
    memset(w, 0, sizeof(*w));
    w->wFormatTag = WAVE_FORMAT_PCM; w->nChannels = 2; w->nSamplesPerSec = OUT_RATE; w->wBitsPerSample = 16;
    w->nBlockAlign = 4; w->nAvgBytesPerSec = OUT_RATE * 4;
    d->wfx = w;
    if (!parse_format(d->wfx, c, desc, 128)) { hr = AUDCLNT_E_UNSUPPORTED_FORMAT; goto fail; }
    d->evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = IAudioClient_Initialize(d->cap, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                 CAPTURE_BUF_HNS, 0, d->wfx, NULL);
    if (FAILED(hr)) { LOG_HR(L"IAudioClient::Initialize(process loopback)", hr); goto fail; }
    hr = IAudioClient_SetEventHandle(d->cap, d->evt);
    if (FAILED(hr)) { LOG_HR(L"SetEventHandle(process loopback)", hr); goto fail; }
    if (FAILED(IAudioClient_GetBufferSize(d->cap, &bufFrames)) || bufFrames == 0) bufFrames = OUT_RATE;
    if (!ensure_cap((void **)&c->tmp, &c->tmpCap, (size_t)bufFrames * 2 + 64, 2 * sizeof(float)) ||
        !ensure_cap((void **)&c->out, &c->outCap, (size_t)bufFrames * 2 + 64, 2 * sizeof(short))) {
        hr = E_OUTOFMEMORY; goto fail;
    }
    hr = IAudioClient_GetService(d->cap, &MY_IID_IAudioCaptureClient, (void **)&d->capc);
    if (FAILED(hr)) { LOG_HR(L"GetService(IAudioCaptureClient, process loopback)", hr); goto fail; }
    hr = IAudioClient_Start(d->cap);
    if (FAILED(hr)) { LOG_HR(L"IAudioClient::Start(process loopback)", hr); goto fail; }
    d->capStarted = 1;
    _snwprintf(name, 255, L"App: %ls", g_curSrcName[0] ? g_curSrcName : g_curSrcExe); name[255] = 0;
    EnterCriticalSection(&g_statusLock);
    wcsncpy(g_devName, name, 255); g_devName[255] = 0;
    wcsncpy(g_fmtDesc, desc, 127); g_fmtDesc[127] = 0;
    LeaveCriticalSection(&g_statusLock);
    log_msg(L"Capturing app: %ls (%ls, root PID %lu, include process tree)  [%ls]", g_curSrcName, g_curSrcExe,
            (unsigned long)pid, desc);
    return S_OK;
fail:
    dev_close(d);
    return FAILED(hr) ? hr : E_FAIL;
}

/* is the captured app still running? */
static int proc_alive(Dev *d)
{
    if (d->hProc) return WaitForSingleObject(d->hProc, 0) == WAIT_TIMEOUT;
    {
        PInfo *p = NULL;
        int n = proc_snap(&p), alive = pi_find(p, n, d->pid) >= 0;
        free(p);
        return alive;
    }
}

static void atomic_max(volatile LONG *p, LONG v)
{
    LONG cur = *p;
    while (v > cur) {
        LONG prev = InterlockedCompareExchange(p, v, cur);
        if (prev == cur) break;
        cur = prev;
    }
}
static void peak_update(float peak)
{
    LONG v = (LONG)((peak > 1.0f ? 1.0f : peak) * 1000000.0f);
    atomic_max(&g_peakNow, v);
    atomic_max(&g_peakMax, v);
}

/* test mode only (--synth S,Q): replace the captured audio with S s of a 440 Hz tone
   followed by Q s of digital silence, repeating, so the silence cut can be verified
   without playing anything on the speakers */
static double g_synthSound, g_synthQuiet;
static LONG64 g_synthPos;
static float synth_fill(Conv *c, UINT32 frames)
{
    UINT32 i;
    float peak = 0.0f;
    double per = g_synthSound + g_synthQuiet;
    for (i = 0; i < frames; i++, g_synthPos++) {
        double t = (double)g_synthPos / c->rate;
        float v = fmod(t, per) < g_synthSound ? (float)(0.3 * sin(6.283185307179586 * 440.0 * t)) : 0.0f;
        c->tmp[2 * i] = c->tmp[2 * i + 1] = v;
        if (fabsf(v) > peak) peak = fabsf(v);
    }
    return peak;
}

/* drain all pending capture packets; returns S_OK or failure HRESULT */
static HRESULT drain(Dev *d, Conv *c, int *gotPacket)
{
    HRESULT hr;
    for (;;) {
        UINT32 next = 0, frames = 0;
        BYTE *data = NULL;
        DWORD flags = 0;
        UINT64 devpos = 0, qpc = 0;
        float peak;
        int silentFlag, quiet, flagged;
        size_t outN;

        hr = IAudioCaptureClient_GetNextPacketSize(d->capc, &next);
        if (FAILED(hr)) return hr;
        if (next == 0) return S_OK;
        hr = IAudioCaptureClient_GetBuffer(d->capc, &data, &frames, &flags, &devpos, &qpc);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) return S_OK;
        if (FAILED(hr)) return hr;

        if (!ensure_cap((void **)&c->tmp, &c->tmpCap, (size_t)frames + 64, 2 * sizeof(float)) ||
            !ensure_cap((void **)&c->out, &c->outCap, (size_t)((double)frames * OUT_RATE / c->rate) + 64, 2 * sizeof(short))) {
            IAudioCaptureClient_ReleaseBuffer(d->capc, frames);
            return E_OUTOFMEMORY;
        }
        if (pause_active()) {                  /* paused: discard, keep the device alive */
            hr = IAudioCaptureClient_ReleaseBuffer(d->capc, frames);
            if (FAILED(hr)) return hr;
            *gotPacket = 1;
            c->pl = c->pr = 0.0f;
            continue;
        }
        silentFlag = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        peak = to_stereo(c, data, frames, silentFlag);
        if (g_synthSound > 0) peak = synth_fill(c, frames);
        peak_update(peak);
        hr = IAudioCaptureClient_ReleaseBuffer(d->capc, frames);   /* release ASAP */
        if (FAILED(hr)) return hr;
        if (frames == 0) continue;
        *gotPacket = 1;

        quiet = silentFlag || peak < QUIET_PEAK;
        flagged = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        if (flagged) InterlockedIncrement(&g_discont);

        /* keep the file aligned with the QPC timeline */
        if (d->proc && qpc != 0) {            /* app capture: trust only plausible timestamps */
            LONG64 off = t100_to_frames((LONG64)qpc) - t100_to_frames(now_100ns());
            if (off > MS_TO_FRAMES(1000) || off < -MS_TO_FRAMES(3000)) qpc = 0;
        }
        if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc != 0) {
            LONG64 ideal = t100_to_frames((LONG64)qpc);
            LONG64 diff  = ideal - g_framesOut;               /* >0: file is behind */
            if (diff > MS_TO_FRAMES(GAP_PAD_MS) ||
                ((quiet || flagged) && diff > MS_TO_FRAMES(QUIET_PAD_MS))) {
                pad_silence(diff, c);
            } else if (quiet && -diff > MS_TO_FRAMES(AHEAD_DROP_MS)) {
                /* device clock faster than QPC: drop a silent packet to resync */
                c->pl = c->pr = 0.0f;
                continue;
            }
        }
        outN = resample(c, frames);
        push_frames(c->out, outN);
    }
}

/* ------------------------------------------------------------------ */
/* IMMNotificationClient (C vtable)                                    */
/* ------------------------------------------------------------------ */
static ULONG STDMETHODCALLTYPE nc_AddRef(IMMNotificationClient *This)  { (void)This; return 1; } /* static object */
static ULONG STDMETHODCALLTYPE nc_Release(IMMNotificationClient *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE nc_QueryInterface(IMMNotificationClient *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (guid_eq(riid, &MY_IID_IUnknown) || guid_eq(riid, &MY_IID_IMMNotificationClient)) {
        *ppv = This; return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE nc_OnDeviceStateChanged(IMMNotificationClient *This, LPCWSTR id, DWORD st) { (void)This; (void)id; (void)st; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_OnDeviceAdded(IMMNotificationClient *This, LPCWSTR id)   { (void)This; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_OnDeviceRemoved(IMMNotificationClient *This, LPCWSTR id) { (void)This; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_OnDefaultDeviceChanged(IMMNotificationClient *This, EDataFlow flow, ERole role, LPCWSTR id)
{
    (void)This; (void)id;
    /* never call COM from here: just signal the capture thread */
    if (flow == eRender && role == eConsole) SetEvent(g_devChangedEvent);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_OnPropertyValueChanged(IMMNotificationClient *This, LPCWSTR id, const PROPERTYKEY key)
{ (void)This; (void)id; (void)key; return S_OK; }

static IMMNotificationClientVtbl g_ncVtbl = {
    nc_QueryInterface, nc_AddRef, nc_Release,
    nc_OnDeviceStateChanged, nc_OnDeviceAdded, nc_OnDeviceRemoved,
    nc_OnDefaultDeviceChanged, nc_OnPropertyValueChanged
};
static IMMNotificationClient g_nc = { &g_ncVtbl };


static unsigned __stdcall capture_thread(void *arg)
{
    Conv conv;
    DWORD taskIdx = 0;
    HANDLE mmcss;
    HRESULT hr;
    DWORD backoff = BACKOFF_MIN_MS;
    int first = 1;
    IMMDeviceEnumerator *notifEn = NULL;
    int notifRegistered = 0;
    int noReconnect = 0;                  /* next open follows a source switch: not a reconnect */
    (void)arg;

    memset(&conv, 0, sizeof(conv));
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LOG_HR(L"CoInitializeEx(capture thread)", hr); request_stop(4); return 1; }
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    if (!mmcss) log_msg(L"Warning: MMCSS 'Pro Audio' not available (error %lu), using normal priority",
                        (unsigned long)GetLastError());
    if (!mmcss) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    {
        /* device-change notifications (2 s polling below is the backstop) */
        HRESULT h2 = CoCreateInstance(&MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                      &MY_IID_IMMDeviceEnumerator, (void **)&notifEn);
        if (SUCCEEDED(h2)) {
            h2 = IMMDeviceEnumerator_RegisterEndpointNotificationCallback(notifEn, &g_nc);
            if (SUCCEEDED(h2)) notifRegistered = 1;
            else LOG_HR(L"RegisterEndpointNotificationCallback (polling instead)", h2);
        } else LOG_HR(L"CoCreateInstance(notification enumerator)", h2);
    }

    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        Dev d;
        LONG64 lastPacket, lastCheck;
        HANDLE waits[3];

        ResetEvent(g_devChangedEvent);
        ResetEvent(g_srcSwitchEvent);
        if (src_switch_apply()) {
            noReconnect = 1;
            backoff = BACKOFF_MIN_MS;
            if (!pause_active()) pad_to_now(&conv);    /* the gap of the switch becomes silence */
        }
        if (g_curSrcExe[0] && g_srcState != 3) {
            /* one app: find its root process, wait (recording silence) while it is not running */
            DWORD pid = src_find_root(g_curSrcExe);
            if (!pid) {
                LONG64 until;
                if (g_srcState != 2) log_msg(L"Source app %ls (%ls) not running: recording silence, checking every 2 s",
                                                     g_curSrcName, g_curSrcExe);
                InterlockedExchange(&g_srcState, 2);
                InterlockedExchange(&g_haveDevice, 0);
                InterlockedExchange(&g_srcPid, 0);
                EnterCriticalSection(&g_statusLock);
                _snwprintf(g_devName, 255, L"In attesa di %ls", g_curSrcName); g_devName[255] = 0; g_fmtDesc[0] = 0;
                LeaveCriticalSection(&g_statusLock);
                until = now_ms() + DEVCHECK_MS;
                while (now_ms() < until) {
                    if (!pause_active()) pad_to_now(&conv);
                    if (WaitForSingleObject(g_stopEvent, 100) == WAIT_OBJECT_0) break;
                    if (WaitForSingleObject(g_srcSwitchEvent, 0) == WAIT_OBJECT_0) break;
                }
                continue;
            }
            hr = proc_open(&d, &conv, pid);
            if (FAILED(hr)) {
                g_srcLastHr = hr;
                InterlockedIncrement(&g_srcActFails);
                if (!g_srcEverOk && g_srcActFails >= 2) {
                    /* process loopback not available here (older Windows / Wine): whole system */
                    WCHAR b[640];
                    log_msg(L"Process loopback unavailable (%ls): FALLING BACK to whole-system capture", hr_str(hr, b, 640));
                    InterlockedExchange(&g_srcState, 3);
                    InterlockedExchange(&g_srcNotice, 1);
                    continue;
                }
            } else {
                if (!g_srcEverOk && g_srcState == 2) log_msg(L"Source app started (PID %lu)", (unsigned long)pid);
                else if (g_srcEverOk) log_msg(L"Source app reattached (PID %lu)", (unsigned long)pid);
                InterlockedExchange(&g_srcEverOk, 1);
                InterlockedExchange(&g_srcState, 1);
                InterlockedExchange(&g_srcPid, (LONG)pid);
                InterlockedIncrement(&g_srcAttaches);
                g_srcActFails = 0;
            }
        } else hr = dev_open(&d, &conv);
        if (FAILED(hr)) {
            /* no device: keep the timeline moving with silence while we back off */
            LONG64 until;
            InterlockedExchange(&g_haveDevice, 0);
            EnterCriticalSection(&g_statusLock);
            wcscpy(g_devName, L"(no device - retrying)"); g_fmtDesc[0] = 0;
            LeaveCriticalSection(&g_statusLock);
            log_msg(L"Retrying in %lu ms", (unsigned long)backoff);
            until = now_ms() + backoff;
            while (now_ms() < until) {
                if (!pause_active()) pad_to_now(&conv);
                if (WaitForSingleObject(g_stopEvent, 100) == WAIT_OBJECT_0) break;
                if (WaitForSingleObject(g_devChangedEvent, 0) == WAIT_OBJECT_0) break;
                if (WaitForSingleObject(g_srcSwitchEvent, 0) == WAIT_OBJECT_0) break;
            }
            backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
            continue;
        }
        backoff = BACKOFF_MIN_MS;
        InterlockedExchange(&g_haveDevice, 1);
        if (!first && !noReconnect) InterlockedIncrement(&g_reconnects);
        first = 0; noReconnect = 0;
        lastPacket = lastCheck = now_ms();
        waits[0] = g_stopEvent; waits[1] = g_srcSwitchEvent; waits[2] = g_devChangedEvent;

        for (;;) {
            DWORD w = WaitForMultipleObjects(d.proc ? 2 : 3, waits, FALSE, POLL_MS);   /* app capture follows the default device by itself */
            int got = 0;
            LONG64 t;
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_OBJECT_0 + 1) {         /* live source switch */
                drain(&d, &conv, &got);           /* keep what the old source already delivered */
                ResetEvent(g_srcSwitchEvent);
                if (src_switch_apply()) { noReconnect = 1; break; }
                continue;                         /* same source: nothing to do */
            }
            if (w == WAIT_OBJECT_0 + 2) { log_msg(L"Default output device changed, switching"); break; }
            hr = keepalive(&d);
            if (FAILED(hr)) { LOG_HR(L"Keep-alive render", hr); break; }
            hr = drain(&d, &conv, &got);
            if (FAILED(hr)) { LOG_HR(L"Capture", hr); break; }
            t = now_ms();
            if (got) lastPacket = t;
            else if (t - lastPacket >= IDLE_PAD_MS && !pause_active()) pad_to_now(&conv);
            if (!got) pause_active();          /* apply a resume even when no packets arrive */
            /* watchdog only when the keep-alive should guarantee packets */
            if (d.renc && t - lastPacket >= WATCHDOG_MS) { log_msg(L"No audio packets for %d s, reinitializing", WATCHDOG_MS / 1000); break; }
            if (t - lastCheck >= DEVCHECK_MS) {
                lastCheck = t;
                if (d.proc) {
                    if (!proc_alive(&d)) {
                        log_msg(L"Source app closed (PID %lu): recording silence, waiting for it to start again", (unsigned long)d.pid);
                        InterlockedIncrement(&g_srcExits);
                        InterlockedExchange(&g_srcState, 2);
                        break;
                    }
                } else if (!default_still_same(&d)) { log_msg(L"Default output device changed (poll), switching"); break; }
            }
        }
        dev_close(&d);
        InterlockedExchange(&g_haveDevice, 0);
        conv.pl = conv.pr = 0.0f; conv.pos = 0.0;
    }

    if (notifRegistered) IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(notifEn, &g_nc);
    SAFE_RELEASE(notifEn, IMMDeviceEnumerator);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    free(conv.tmp); free(conv.out);
    CoUninitialize();
    return 0;
}

/* ================================================================== */
/* AAC (.m4a) conversion with Windows' built-in Media Foundation       */
/* ================================================================== */
/* MF is loaded dynamically so the app still starts (WAV only) on
   Windows "N" editions without the Media Feature Pack. */
#define WM_APP_CONV    (WM_APP + 3)
#define M4A_MAX_BYTES  ((double)g_maxFileMB * 1000000.0)   /* size target per output file */
#define CONV_QUEUE_MAX 64

typedef HRESULT (WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *PFN_MFShutdown)(void);
typedef HRESULT (WINAPI *PFN_MFCreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT (WINAPI *PFN_MFCreateMediaType)(IMFMediaType **);
typedef HRESULT (WINAPI *PFN_MFCreateMemoryBuffer)(DWORD, IMFMediaBuffer **);
typedef HRESULT (WINAPI *PFN_MFCreateSample)(IMFSample **);
typedef HRESULT (WINAPI *PFN_MFCreateSinkWriterFromURL)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **);
typedef HRESULT (WINAPI *PFN_MFCreateSourceReaderFromURL)(LPCWSTR, IMFAttributes *, IMFSourceReader **);

static struct {
    PFN_MFStartup                   Startup;
    PFN_MFShutdown                  Shutdown;
    PFN_MFCreateAttributes          CreateAttributes;
    PFN_MFCreateMediaType           CreateMediaType;
    PFN_MFCreateMemoryBuffer        CreateMemoryBuffer;
    PFN_MFCreateSample              CreateSample;
    PFN_MFCreateSinkWriterFromURL   CreateSinkWriterFromURL;
    PFN_MFCreateSourceReaderFromURL CreateSourceReaderFromURL;
} mf;
static int g_mfAvailable;

/* settings (loaded from settings.ini, overridable in test mode) */
static int g_compress = 1, g_aacKbps = 96, g_mono = 0, g_keepWav = 0, g_maxFileMB = 190;
static int g_convEnabled;                  /* session: g_compress && MF available */
static int g_curKbps = 96, g_curMono, g_curKeep; /* session snapshot (settings changes apply next session) */

/* queue + worker state */
static CRITICAL_SECTION g_convLock;
static HANDLE  g_convEvent, g_convQuit, g_convThread;
static int     g_convQHead, g_convQCount;
static volatile LONG g_convPending;        /* queued + in progress */
static volatile LONG g_convPct;
static volatile LONG g_convCancel;         /* abort current + skip queue (exit/session end) */
static HWND    g_convNotify;
static WCHAR   g_convCurName[MAX_PATH];
/* results of the current session (protected by g_convLock) */
static int     g_sessConvOk, g_sessConvFail;
static LONG64  g_sessM4aBytes;
static double  g_sessM4aSec;
static WCHAR   g_convLastOut[MAX_PATH * 2];   /* last good .m4a */
static LONG64  g_convLastBytes;
static WCHAR   g_convLastFailWav[MAX_PATH * 2];
static WCHAR   g_convLastErr[640];

static const int k_aacRates[4] = { 96, 128, 160, 192 };   /* rates the MS AAC encoder documents */

static void *mf_proc(HMODULE m, const char *name) { return m ? (void *)(void (*)(void))GetProcAddress(m, name) : NULL; }

static HMODULE load_sys_dll(const WCHAR *name)
{
    HMODULE h = LoadLibraryExW(name, NULL, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    return h ? h : LoadLibraryW(name);
}

static int mf_load(void)
{
    HMODULE plat = load_sys_dll(L"mfplat.dll"), rw = load_sys_dll(L"mfreadwrite.dll");
    mf.Startup                   = (PFN_MFStartup)(void (*)(void))mf_proc(plat, "MFStartup");
    mf.Shutdown                  = (PFN_MFShutdown)(void (*)(void))mf_proc(plat, "MFShutdown");
    mf.CreateAttributes          = (PFN_MFCreateAttributes)(void (*)(void))mf_proc(plat, "MFCreateAttributes");
    mf.CreateMediaType           = (PFN_MFCreateMediaType)(void (*)(void))mf_proc(plat, "MFCreateMediaType");
    mf.CreateMemoryBuffer        = (PFN_MFCreateMemoryBuffer)(void (*)(void))mf_proc(plat, "MFCreateMemoryBuffer");
    mf.CreateSample              = (PFN_MFCreateSample)(void (*)(void))mf_proc(plat, "MFCreateSample");
    mf.CreateSinkWriterFromURL   = (PFN_MFCreateSinkWriterFromURL)(void (*)(void))mf_proc(rw, "MFCreateSinkWriterFromURL");
    mf.CreateSourceReaderFromURL = (PFN_MFCreateSourceReaderFromURL)(void (*)(void))mf_proc(rw, "MFCreateSourceReaderFromURL");
    g_mfAvailable = mf.Startup && mf.Shutdown && mf.CreateAttributes && mf.CreateMediaType && mf.CreateMemoryBuffer &&
                    mf.CreateSample && mf.CreateSinkWriterFromURL && mf.CreateSourceReaderFromURL;
    if (!g_mfAvailable) log_msg(L"Media Foundation not available: recordings stay WAV");
    return g_mfAvailable;
}

/* bitrate the size limit must assume: the requested one, or the encoder rate we may fall back to */
static int aac_limit_kbps(int req)
{
    int i;
    for (i = 0; i < 4; i++) if (req <= k_aacRates[i]) return k_aacRates[i];
    return 192;
}
/* longest part that still gives an .m4a <= M4A_MAX_BYTES (3% container/headroom margin) */
static LONG64 max_part_seconds(int req)
{
    return (LONG64)(M4A_MAX_BYTES / (aac_limit_kbps(req) * 125.0 * 1.03));
}

static void conv_post(void) { if (g_convNotify) PostMessageW(g_convNotify, WM_APP_CONV, 0, 0); }

/* open one of our WAV files; returns PCM data offset and length (from the file size, so a
   WAV whose header was not finalized after a crash still converts completely) */
static HRESULT wav_open_read(const WCHAR *path, HANDLE *ph, LONG64 *dataOff, LONG64 *dataLen)
{
    BYTE hd[4096];
    DWORD got = 0, pos = 12;
    LARGE_INTEGER sz;
    int fmtOk = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    *ph = INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    if (!ReadFile(h, hd, sizeof(hd), &got, NULL) || got < 44 || memcmp(hd, "RIFF", 4) || memcmp(hd + 8, "WAVE", 4)) {
        CloseHandle(h); return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    }
    while (pos + 8 <= got) {
        DWORD csz = (DWORD)hd[pos + 4] | (DWORD)hd[pos + 5] << 8 | (DWORD)hd[pos + 6] << 16 | (DWORD)hd[pos + 7] << 24;
        if (!memcmp(hd + pos, "fmt ", 4) && pos + 24 <= got) {
            WORD tag = (WORD)(hd[pos + 8] | hd[pos + 9] << 8), ch = (WORD)(hd[pos + 10] | hd[pos + 11] << 8);
            DWORD rate = (DWORD)hd[pos + 12] | (DWORD)hd[pos + 13] << 8 | (DWORD)hd[pos + 14] << 16 | (DWORD)hd[pos + 15] << 24;
            WORD bits = (WORD)(hd[pos + 22] | hd[pos + 23] << 8);
            fmtOk = (tag == 1 && ch == OUT_CH && rate == OUT_RATE && bits == 16);
        } else if (!memcmp(hd + pos, "data", 4)) {
            if (!fmtOk || !GetFileSizeEx(h, &sz)) break;
            *dataOff = pos + 8;
            *dataLen = sz.QuadPart - (pos + 8);
            *dataLen -= *dataLen % OUT_BPF;
            if (*dataLen < 0) *dataLen = 0;
            *ph = h;
            return S_OK;
        }
        pos += 8 + csz + (csz & 1);
    }
    CloseHandle(h);
    return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
}

static HRESULT mk_audio_type(IMFMediaType **pt, const GUID *sub, int ch, UINT32 avgBytes, int pcm)
{
    IMFMediaType *t = NULL;
    HRESULT hr = mf.CreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, sub);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, OUT_RATE);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)ch);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytes);
    if (pcm) {
        if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)(ch * 2));
        if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    } else {
        if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AAC_PAYLOAD_TYPE, 0);          /* raw AAC in MP4 */
        if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29); /* AAC-LC */
    }
    if (FAILED(hr)) { if (t) IMFMediaType_Release(t); t = NULL; }
    *pt = t;
    return hr;
}

/* encode PCM [off, off+len) of an open WAV into out (.m4a). *setupFailed = 1 when the
   encoder rejected the configuration (caller may retry another bitrate). */
static HRESULT aac_encode(HANDLE hw, LONG64 off, LONG64 len, const WCHAR *out, int kbps, int mono, int *setupFailed)
{
    const DWORD CHUNK = OUT_RATE / 2;                   /* 0.5 s per sample */
    int ch = mono ? 1 : 2;
    IMFAttributes *attr = NULL;
    IMFSinkWriter *w = NULL;
    IMFMediaType *tOut = NULL, *tIn = NULL;
    DWORD si = 0;
    BYTE *buf = NULL;
    LONG64 total = len / OUT_BPF, done = 0;
    LARGE_INTEGER li;
    HRESULT hr;

    *setupFailed = 1;
    hr = mf.CreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetGUID(attr, &MF_TRANSCODE_CONTAINERTYPE, &MFTranscodeContainerType_MPEG4);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUINT32(attr, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (SUCCEEDED(hr)) hr = mf.CreateSinkWriterFromURL(out, NULL, attr, &w);
    if (SUCCEEDED(hr)) hr = mk_audio_type(&tOut, &MFAudioFormat_AAC, ch, (UINT32)kbps * 125, 0);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_AddStream(w, tOut, &si);
    if (SUCCEEDED(hr)) hr = mk_audio_type(&tIn, &MFAudioFormat_PCM, ch, (UINT32)(OUT_RATE * ch * 2), 1);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_SetInputMediaType(w, si, tIn, NULL);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_BeginWriting(w);
    if (FAILED(hr)) goto done;
    *setupFailed = 0;

    buf = (BYTE *)malloc(CHUNK * OUT_BPF);
    if (!buf) { hr = E_OUTOFMEMORY; goto done; }
    li.QuadPart = off;
    if (!SetFilePointerEx(hw, li, NULL, FILE_BEGIN)) { hr = HRESULT_FROM_WIN32(GetLastError()); goto done; }

    while (done < total) {
        DWORD n = (DWORD)((total - done) < CHUNK ? (total - done) : CHUNK), got = 0, outBytes;
        IMFMediaBuffer *mb = NULL;
        IMFSample *smp = NULL;
        BYTE *p = NULL;
        if (g_convCancel) { hr = E_ABORT; break; }
        if (!ReadFile(hw, buf, n * OUT_BPF, &got, NULL)) { hr = HRESULT_FROM_WIN32(GetLastError()); break; }
        n = got / OUT_BPF;
        if (n == 0) break;
        if (mono) {
            short *s = (short *)buf;
            DWORD i;
            for (i = 0; i < n; i++) s[i] = (short)(((int)s[2 * i] + (int)s[2 * i + 1]) / 2);
        }
        outBytes = n * (DWORD)ch * 2;
        hr = mf.CreateMemoryBuffer(outBytes, &mb);
        if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(mb, &p, NULL, NULL);
        if (SUCCEEDED(hr)) { memcpy(p, buf, outBytes); IMFMediaBuffer_Unlock(mb); hr = IMFMediaBuffer_SetCurrentLength(mb, outBytes); }
        if (SUCCEEDED(hr)) hr = mf.CreateSample(&smp);
        if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(smp, mb);
        if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(smp, done * 10000000LL / OUT_RATE);
        if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(smp, (LONG64)n * 10000000LL / OUT_RATE);
        if (SUCCEEDED(hr)) hr = IMFSinkWriter_WriteSample(w, si, smp);
        if (smp) IMFSample_Release(smp);
        if (mb) IMFMediaBuffer_Release(mb);
        if (FAILED(hr)) break;
        done += n;
        {
            LONG pct = (LONG)(done * 100 / total);
            if (pct != g_convPct) { InterlockedExchange(&g_convPct, pct); conv_post(); }
        }
    }
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_Finalize(w);
done:
    free(buf);
    if (tIn) IMFMediaType_Release(tIn);
    if (tOut) IMFMediaType_Release(tOut);
    if (w) IMFSinkWriter_Release(w);
    if (attr) IMFAttributes_Release(attr);
    return hr;
}

static HRESULT m4a_duration(const WCHAR *path, double *sec)
{
    IMFSourceReader *r = NULL;
    PROPVARIANT pv;
    HRESULT hr = mf.CreateSourceReaderFromURL(path, NULL, &r);
    *sec = 0;
    if (FAILED(hr)) return hr;
    PropVariantInit(&pv);
    hr = IMFSourceReader_GetPresentationAttribute(r, MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &pv);
    if (SUCCEEDED(hr)) {
        if (pv.vt == VT_UI8) *sec = (double)pv.uhVal.QuadPart / 1e7;
        else hr = E_UNEXPECTED;
    }
    PropVariantClear(&pv);
    IMFSourceReader_Release(r);
    return hr;
}

static LONG64 file_size(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return -1;
    return (LONG64)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
}

static const WCHAR *base_name(const WCHAR *p) { const WCHAR *s = wcsrchr(p, L'\\'); return s ? s + 1 : p; }

static void conv_fail(const WCHAR *wav, const WCHAR *what, HRESULT hr)
{
    WCHAR b[640];
    hr_str(hr, b, 640);
    log_msg(L"CONVERT FAIL %ls: %ls %ls (WAV kept)", wav, what, b);
    EnterCriticalSection(&g_convLock);
    g_sessConvFail++;
    wcsncpy(g_convLastFailWav, wav, MAX_PATH * 2 - 1);
    _snwprintf(g_convLastErr, 639, L"%ls: %ls", what, b); g_convLastErr[639] = 0;
    LeaveCriticalSection(&g_convLock);
}

static void conv_process(const WCHAR *wav)
{
    WCHAR out[MAX_PATH * 2];
    HANDLE hw;
    LONG64 off = 0, len = 0, m4aBytes;
    double wavSec, m4aSec = 0;
    HRESULT hr;
    int tries[6], nTries = 0, i, used = 0, setupFailed = 0;
    size_t L = wcslen(wav);

    if (L < 5 || L >= MAX_PATH * 2 || _wcsicmp(wav + L - 4, L".wav")) { conv_fail(wav, L"not a .wav", E_INVALIDARG); return; }
    wcscpy(out, wav);
    wcscpy(out + L - 4, L".m4a");
    EnterCriticalSection(&g_convLock);
    wcsncpy(g_convCurName, base_name(out), MAX_PATH - 1); g_convCurName[MAX_PATH - 1] = 0;
    LeaveCriticalSection(&g_convLock);
    InterlockedExchange(&g_convPct, 0);
    conv_post();

    hr = wav_open_read(wav, &hw, &off, &len);
    if (FAILED(hr)) { conv_fail(wav, L"reading WAV", hr); return; }
    wavSec = (double)(len / OUT_BPF) / OUT_RATE;
    if (len < OUT_BPF * OUT_RATE / 10) {           /* < 0.1 s: nothing worth keeping */
        CloseHandle(hw);
        DeleteFileW(wav);
        log_msg(L"CONVERT SKIP %ls: empty recording removed", wav);
        return;
    }
    /* requested rate first, then the documented encoder rates at or above it */
    tries[nTries++] = g_curKbps;
    for (i = 0; i < 4; i++) if (k_aacRates[i] > g_curKbps) tries[nTries++] = k_aacRates[i];
    if (g_curKbps > 192) tries[nTries++] = 192;
    /* encode to a temporary name (unique per attempt, so a sink that still holds a
       failed file cannot block the retry) and rename only after Finalize succeeded */
    for (i = 0; i < nTries; i++) {
        WCHAR tmp[MAX_PATH * 2 + 32];
        _snwprintf(tmp, MAX_PATH * 2 + 31, L"%.*ls.encoding%d.m4a", (int)(L - 4), wav, i + 1);
        tmp[MAX_PATH * 2 + 31] = 0;
        hr = aac_encode(hw, off, len, tmp, tries[i], g_curMono, &setupFailed);
        if (SUCCEEDED(hr)) {
            if (!MoveFileExW(tmp, out, MOVEFILE_REPLACE_EXISTING)) {
                hr = HRESULT_FROM_WIN32(GetLastError());
                DeleteFileW(tmp);
                setupFailed = 0;
                break;
            }
            used = tries[i];
            break;
        }
        DeleteFileW(tmp);
        if (!setupFailed || hr == E_ABORT) break;
        { WCHAR b[640]; log_msg(L"AAC encoder rejected %d kbps (%ls), trying next rate", tries[i], hr_str(hr, b, 640)); }
    }
    CloseHandle(hw);
    if (FAILED(hr)) { conv_fail(wav, hr == E_ABORT ? L"cancelled" : L"AAC encoding", hr); return; }

    /* verify before deleting anything */
    m4aBytes = file_size(out);
    hr = m4a_duration(out, &m4aSec);
    if (m4aBytes <= 0) { conv_fail(wav, L"verify: .m4a missing or empty", E_FAIL); return; }
    if (FAILED(hr)) { conv_fail(wav, L"verify: cannot read .m4a duration", hr); return; }
    if (fabs(m4aSec - wavSec) > (wavSec * 0.01 > 1.0 ? wavSec * 0.01 : 1.0)) {
        WCHAR m[128];
        _snwprintf(m, 127, L"verify: duration %.2f s vs WAV %.2f s", m4aSec, wavSec); m[127] = 0;
        conv_fail(wav, m, E_FAIL);
        return;
    }
    if (m4aBytes > (LONG64)M4A_MAX_BYTES)
        log_msg(L"Warning: %ls is %.1f MB (above the %.0f MB target)", out, m4aBytes / 1e6, M4A_MAX_BYTES / 1e6);
    if (!g_curKeep && !DeleteFileW(wav))
        log_msg(L"Could not delete %ls (error %lu)", wav, (unsigned long)GetLastError());
    log_msg(L"CONVERT OK file=%ls bytes=%.0f duration=%.2fs wav_duration=%.2fs kbps=%d channels=%d wav_deleted=%d",
            out, (double)m4aBytes, m4aSec, wavSec, used, g_curMono ? 1 : 2, !g_curKeep);
    EnterCriticalSection(&g_convLock);
    g_sessConvOk++;
    g_sessM4aBytes += m4aBytes;
    g_sessM4aSec += m4aSec;
    wcscpy(g_convLastOut, out);
    g_convLastBytes = m4aBytes;
    LeaveCriticalSection(&g_convLock);
}

/* ---- session part registry + final rename ----------------------------------- */
#define SESS_PARTS_MAX 256
typedef struct { int sess; WCHAR base[MAX_PATH * 2]; } SessPart;   /* base = internal path without extension */
static SessPart g_parts[SESS_PARTS_MAX];
static int      g_partCount;
static volatile LONG g_sessId;
/* summary of the last finalized session (protected by g_convLock) */
static int      g_finValid, g_finCount, g_finFailed;
static LONG64   g_finBytes;
static WCHAR    g_finFirst[MAX_PATH * 2];      /* first output file (part 1) */
static WCHAR    g_finFailWav[MAX_PATH * 2];    /* a WAV kept because its conversion failed */

static int path_exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* <dir of base>\<name>[ (k)][_partN]<ext>; returns 0 if the result would be too long */
static int build_target(WCHAR *dst, size_t cch, const WCHAR *base, const WCHAR *name, int k, int idx, const WCHAR *ext)
{
    const WCHAR *slash = wcsrchr(base, L'\\');
    WCHAR sfx[32] = L"", part[32] = L"";
    int dirLen = slash ? (int)(slash - base) : 0;
    if (k > 1) _snwprintf(sfx, 31, L" (%d)", k);
    if (idx > 0) _snwprintf(part, 31, L"_part%d", idx + 1);
    _snwprintf(dst, cch - 1, L"%.*ls\\%ls%ls%ls%ls", dirLen, base, name, sfx, part, ext);
    dst[cch - 1] = 0;
    return wcslen(dst) < MAX_PATH - 1;
}

/* rename every output file of session `sess` to <folder>\<name>[ (k)][_partN].<ext> */
static void finalize_session(int sess, const WCHAR *name)
{
    static WCHAR bases[SESS_PARTS_MAX][MAX_PATH * 2];
    static const WCHAR *exts[2] = { L".m4a", L".wav" };
    int n = 0, i, j, e, k, chosen = 0, count = 0, failed = 0;
    LONG64 bytes = 0;
    WCHAR first[MAX_PATH * 2] = L"", failWav[MAX_PATH * 2] = L"";

    EnterCriticalSection(&g_convLock);
    for (i = 0, j = 0; i < g_partCount; i++) {
        if (g_parts[i].sess == sess) { if (n < SESS_PARTS_MAX) wcscpy(bases[n++], g_parts[i].base); }
        else g_parts[j++] = g_parts[i];
    }
    g_partCount = j;
    LeaveCriticalSection(&g_convLock);

    /* pick the first suffix " (k)" for which no target of any part already exists */
    if (name && name[0] && n > 0) {
        for (k = 1; k < 1000 && !chosen; k++) {
            int clash = 0;
            for (i = 0; i < n && !clash; i++) for (e = 0; e < 2 && !clash; e++) {
                WCHAR src[MAX_PATH * 2 + 8], dst[MAX_PATH * 2];
                _snwprintf(src, MAX_PATH * 2 + 7, L"%ls%ls", bases[i], exts[e]); src[MAX_PATH * 2 + 7] = 0;
                if (!path_exists(src)) continue;
                if (!build_target(dst, MAX_PATH * 2, bases[i], name, k, i, exts[e])) clash = 2;
                else if (_wcsicmp(src, dst) != 0 && path_exists(dst)) clash = 1;
            }
            if (clash == 2) { log_msg(L"Final name too long, keeping internal names"); break; }
            if (!clash) chosen = k;
        }
    }

    for (i = 0; i < n; i++) {
        /* remove temp files of failed encoder attempts that could not be deleted earlier */
        WCHAR pat[MAX_PATH * 2 + 32];
        WIN32_FIND_DATAW fd;
        HANDLE fh;
        _snwprintf(pat, MAX_PATH * 2 + 31, L"%ls.encoding*.m4a", bases[i]); pat[MAX_PATH * 2 + 31] = 0;
        fh = FindFirstFileW(pat, &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do {
                WCHAR full[MAX_PATH * 2 + 64];
                const WCHAR *sl = wcsrchr(bases[i], L'\\');
                _snwprintf(full, MAX_PATH * 2 + 63, L"%.*ls\\%ls", sl ? (int)(sl - bases[i]) : 0, bases[i], fd.cFileName);
                full[MAX_PATH * 2 + 63] = 0;
                DeleteFileW(full);
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
    }
    for (i = 0; i < n; i++) {
        WCHAR primary[MAX_PATH * 2] = L"";
        int hasM4a = 0, hasWav = 0;
        for (e = 0; e < 2; e++) {
            WCHAR src[MAX_PATH * 2 + 8], dst[MAX_PATH * 2];
            _snwprintf(src, MAX_PATH * 2 + 7, L"%ls%ls", bases[i], exts[e]); src[MAX_PATH * 2 + 7] = 0;
            if (!path_exists(src)) continue;
            wcsncpy(dst, src, MAX_PATH * 2 - 1); dst[MAX_PATH * 2 - 1] = 0;
            if (chosen && build_target(dst, MAX_PATH * 2, bases[i], name, chosen, i, exts[e]) && _wcsicmp(src, dst) != 0) {
                if (MoveFileExW(src, dst, 0)) log_msg(L"Renamed %ls -> %ls", base_name(src), base_name(dst));
                else { log_msg(L"Rename failed (error %lu), keeping %ls", (unsigned long)GetLastError(), src); wcscpy(dst, src); }
            } else if (chosen) {
                wcsncpy(dst, src, MAX_PATH * 2 - 1);
            }
            if (e == 0) hasM4a = 1; else hasWav = 1;
            if (!primary[0]) { wcscpy(primary, dst); bytes += file_size(dst); }
            if (e == 1 && g_convEnabled && !hasM4a) { failed = 1; wcscpy(failWav, dst); }
        }
        if ((hasM4a || hasWav) && !first[0]) wcscpy(first, primary);
        if (hasM4a || hasWav) count++;
    }
    log_msg(L"Session saved: %d file(s), %.1f MB, first=%ls", count, bytes / 1048576.0, first);
    EnterCriticalSection(&g_convLock);
    g_finValid = 1; g_finCount = count; g_finFailed = failed; g_finBytes = bytes;
    wcscpy(g_finFirst, first); wcscpy(g_finFailWav, failWav);
    LeaveCriticalSection(&g_convLock);
}

/* ---- job queue: 0 = convert WAV part, 1 = finalize session (rename) ---------- */
typedef struct { int type; int sess; WCHAR *text; } ConvJob;
static ConvJob g_jobs[CONV_QUEUE_MAX];

static int job_push(int type, int sess, const WCHAR *text)
{
    WCHAR *copy = _wcsdup(text ? text : L"");
    int ok = 0;
    if (!copy) return 0;
    EnterCriticalSection(&g_convLock);
    if (g_convQCount < CONV_QUEUE_MAX) {
        ConvJob *j = &g_jobs[(g_convQHead + g_convQCount) % CONV_QUEUE_MAX];
        j->type = type; j->sess = sess; j->text = copy;
        g_convQCount++;
        InterlockedIncrement(&g_convPending);
        ok = 1;
    }
    LeaveCriticalSection(&g_convLock);
    if (!ok) { free(copy); return 0; }
    SetEvent(g_convEvent);
    conv_post();
    return 1;
}

static unsigned __stdcall conv_thread(void *arg)
{
    HANDLE waits[2];
    int mfStarted = 0;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (g_mfAvailable) {
        mfStarted = SUCCEEDED(mf.Startup(MF_VERSION, MFSTARTUP_LITE));
        if (!mfStarted) log_msg(L"MFStartup failed: conversions will fail and WAVs are kept");
    }
    waits[0] = g_convQuit; waits[1] = g_convEvent;
    for (;;) {
        ConvJob job;
        int have = 0;
        EnterCriticalSection(&g_convLock);
        if (g_convQCount) {
            job = g_jobs[g_convQHead];
            g_convQHead = (g_convQHead + 1) % CONV_QUEUE_MAX;
            g_convQCount--;
            have = 1;
        }
        LeaveCriticalSection(&g_convLock);
        if (!have) {
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            continue;
        }
        if (job.type == 1) finalize_session(job.sess, job.text);          /* fast: always runs */
        else if (g_convCancel) log_msg(L"Conversion skipped (exiting), WAV kept: %ls", job.text);
        else if (!mfStarted) conv_fail(job.text, L"MFStartup", E_FAIL);
        else conv_process(job.text);
        free(job.text);
        EnterCriticalSection(&g_convLock);
        g_convCurName[0] = 0;
        LeaveCriticalSection(&g_convLock);
        InterlockedDecrement(&g_convPending);
        conv_post();
    }
    if (mfStarted) mf.Shutdown();
    CoUninitialize();
    return 0;
}

static void conv_init(void)
{
    InitializeCriticalSection(&g_convLock);
    g_convEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_convQuit  = CreateEventW(NULL, TRUE, FALSE, NULL);
    mf_load();
    if (!g_convEvent || !g_convQuit) { g_mfAvailable = 0; return; }
    g_convThread = (HANDLE)_beginthreadex(NULL, 0, conv_thread, NULL, 0, NULL);
    if (!g_convThread) { g_mfAvailable = 0; log_msg(L"Cannot start worker thread: recordings stay WAV with internal names"); }
}

static void conv_shutdown(DWORD waitMs)
{
    if (!g_convThread) return;
    SetEvent(g_convQuit);
    if (WaitForSingleObject(g_convThread, waitMs) == WAIT_OBJECT_0) { CloseHandle(g_convThread); g_convThread = NULL; }
}

/* called by the writer thread each time a WAV part is finalized */
static void on_part_closed(const WCHAR *path, ULONG64 dataBytes)
{
    size_t L = wcslen(path);
    if (!path[0]) return;
    if (dataBytes == 0) { DeleteFileW(path); return; }          /* never leave empty parts */
    EnterCriticalSection(&g_convLock);
    if (g_partCount < SESS_PARTS_MAX && L > 4 && L < MAX_PATH * 2) {
        g_parts[g_partCount].sess = (int)g_sessId;
        wcsncpy(g_parts[g_partCount].base, path, L - 4);
        g_parts[g_partCount].base[L - 4] = 0;
        g_partCount++;
    }
    LeaveCriticalSection(&g_convLock);
    if (!g_convEnabled || !g_convThread) return;
    if (!job_push(0, (int)g_sessId, path)) log_msg(L"Conversion queue full, WAV kept: %ls", path);
}

/* queue the final rename of the current session (after its conversions) */
static int finalize_request(const WCHAR *name)
{
    if (!g_convThread) return 0;
    return job_push(1, (int)g_sessId, name);
}

/* ================================================================== */
/* Live AAC recording (Compress=1, CompressLive=1)                     */
/* ================================================================== */
/* Design: the Windows AAC encoder MFT (the same encoder the sink writer uses) is
   driven directly from the writer thread, and our own minimal MP4 muxer writes
   each part as
       ftyp | mdat header | raw AAC frames ...            (appended live)
   plus a tiny side index "<part>.m4a.lrx" (2 bytes per frame) that is appended
   after every batch of frames. At the end of a part we append the moov box
   (sample tables) and patch the mdat size: no re-encoding, no copying, so the
   post-stop wait is just a few milliseconds.
   If the process dies, every frame already handed to WriteFile is in the file
   and in the index; on the next launch recover_folder() rebuilds the moov from
   the index and the file becomes a normal playable .m4a. Every SegmentMinutes
   both files are flushed to the disk (FlushFileBuffers), which bounds the loss
   for an OS crash / power cut too. The encoder is a single continuous stream,
   so parts split at frame boundaries without gaps. */
#define MP4_FTYP_SIZE   28
#define MP4_DATA_OFF    36                  /* ftyp + 8-byte mdat header */
#define MP4_CHUNK       47                  /* frames per chunk (~1 s) */
#define LRX_HDR         64
#define AAC_FRAME       1024                /* PCM frames per AAC frame */
#define FEED_FRAMES     16384               /* PCM frames per ProcessInput */

static int g_compressLive = 1;              /* setting: 1 = "durante", 0 = "dopo" */
static int g_segMin = 5;                    /* setting: SegmentMinutes (disk flush checkpoint) */
static int g_curLive;                       /* session: live AAC active */
static int g_testSegSec;                    /* test: --segment-seconds */
static WCHAR g_fakeAacPath[MAX_PATH];       /* dev test only: --fake-aac <adts> (Wine has no AAC encoder) */
static volatile LONG g_liveNameOk;          /* session name file may be written */
static WCHAR g_liveNamePath[MAX_PATH * 2];
static CRITICAL_SECTION g_renLock;          /* live rename request + .lrn file/path */

typedef struct {
    /* encoder */
    IMFTransform *mft;
    IMFSample *outSmp; IMFMediaBuffer *outBuf;
    int providesSamples, mfStarted;
    int ch, kbps;
    BYTE asc[8]; int ascLen;
    LONG64 inFrames;                        /* PCM frames fed */
    short *tmp; size_t tmpCap;
    /* fake encoder (dev test) */
    BYTE *fake; DWORD *fakeOff; WORD *fakeLen; int fakeN, fakeI; LONG64 fakeOut;
    /* current part */
    HANDLE hd, hi;
    DWORD *sizes; size_t nf, capf;
    ULONG64 data;
    BYTE *db; size_t dlen, dcap;            /* pending frame bytes */
    BYTE *ib; size_t ilen, icap;            /* pending index entries */
    double limit;                           /* bytes per output file */
    LONG64 splitFrames;                     /* AAC frames per part (0 = none) */
    ULONG64 closedBytes;                    /* size of finished parts */
    LONG64 closedFrames;                    /* AAC frames in finished parts */
    ULONG64 closedBytesAtStart;             /* bytes recorded before this run (resume) */
    LONG64 totalFrames;
    LONG64 maxPartBytes;
    int parts;
    LONG64 lastFlushMs, flushEveryMs, lastLogMs, startMs;
    WCHAR idxPath[MAX_PATH * 2 + 8];
} LiveEnc;
static LiveEnc g_lv;

/* ---- resumable session state: %APPDATA%\LoopbackRec\session.ini ([Session]) ---- */
static WCHAR g_sessIni[MAX_PATH * 2];
static CRITICAL_SECTION g_sessLock;
static void sess_put(const WCHAR *key, const WCHAR *val)
{
    if (!g_sessIni[0]) return;
    EnterCriticalSection(&g_sessLock);
    WritePrivateProfileStringW(L"Session", key, val, g_sessIni);
    LeaveCriticalSection(&g_sessLock);
}
static void sess_put_int(const WCHAR *key, LONG64 v)
{
    WCHAR b[32];
    _snwprintf(b, 31, L"%lld", (long long)v); b[31] = 0;
    sess_put(key, b);
}
static LONG64 sess_get_int(const WCHAR *key, LONG64 def)
{
    WCHAR b[32];
    GetPrivateProfileStringW(L"Session", key, L"", b, 32, g_sessIni);
    return b[0] ? _wtoi64(b) : def;
}
/* capture thread: take a pending source-switch request. Returns 1 if the source changed. */
static int src_switch_apply(void)
{
    WCHAR e[64], n[128], oldN[128];
    if (!InterlockedExchange(&g_srcSwitchReq, 0)) return 0;
    EnterCriticalSection(&g_statusLock);
    wcscpy(e, g_reqSrcExe); wcscpy(n, g_reqSrcName);
    LeaveCriticalSection(&g_statusLock);
    if (e[0] && !n[0]) wcsncpy(n, e, 127);
    if (!_wcsicmp(e, g_curSrcExe)) { log_msg(L"Source switch to %ls ignored: already the source", e[0] ? n : L"whole system"); return 0; }
    wcscpy(oldN, g_curSrcExe[0] ? (g_srcState == 3 ? L"whole system (fallback)" : g_curSrcName) : L"whole system");
    EnterCriticalSection(&g_statusLock);
    wcscpy(g_curSrcExe, e); wcscpy(g_curSrcName, e[0] ? n : L"");
    LeaveCriticalSection(&g_statusLock);
    InterlockedExchange(&g_srcState, 0); InterlockedExchange(&g_srcEverOk, 0);
    InterlockedExchange(&g_srcPid, 0); g_srcActFails = 0; g_srcLastHr = S_OK;
    InterlockedIncrement(&g_srcSwitches);
    if (g_curLive) {                                 /* a crash now resumes with the new source */
        sess_put(L"SourceExe", e[0] ? e : NULL);
        sess_put(L"SourceName", e[0] ? n : NULL);
    }
    log_msg(L"SOURCE SWITCH #%ld at %.2f s of timeline (%.2f s in file): %ls -> %ls%ls%ls%ls", (long)g_srcSwitches,
            (double)g_framesOut / OUT_RATE, (double)atomic_read64(&g_fileFrames) / OUT_RATE, oldN,
            e[0] ? n : L"whole system", e[0] ? L" (" : L"", e, e[0] ? L")" : L"");
    return 1;
}
static void sess_clear(void)
{
    if (!g_sessIni[0]) return;
    EnterCriticalSection(&g_sessLock);
    DeleteFileW(g_sessIni);
    LeaveCriticalSection(&g_sessLock);
}

/* ---- minimal MP4 writer (when p == NULL only sizes are computed, in O(1)) ---- */
typedef struct { BYTE *p; size_t n; } Bw;
static void bw8(Bw *b, unsigned v)  { if (b->p) b->p[b->n] = (BYTE)v; b->n++; }
static void bw16(Bw *b, unsigned v) { bw8(b, v >> 8); bw8(b, v); }
static void bw32(Bw *b, DWORD v)    { bw16(b, v >> 16); bw16(b, v & 0xFFFF); }
static void bw64(Bw *b, ULONG64 v)  { bw32(b, (DWORD)(v >> 32)); bw32(b, (DWORD)v); }
static void bwz(Bw *b, size_t n)    { while (n--) bw8(b, 0); }
static void bwtag(Bw *b, const char *t) { bw8(b, (BYTE)t[0]); bw8(b, (BYTE)t[1]); bw8(b, (BYTE)t[2]); bw8(b, (BYTE)t[3]); }
static size_t bw_box(Bw *b, const char *t) { size_t at = b->n; bw32(b, 0); bwtag(b, t); return at; }
static void bw_end(Bw *b, size_t at)
{
    if (b->p) { DWORD v = (DWORD)(b->n - at); b->p[at] = (BYTE)(v >> 24); b->p[at + 1] = (BYTE)(v >> 16); b->p[at + 2] = (BYTE)(v >> 8); b->p[at + 3] = (BYTE)v; }
}
static void bw_matrix(Bw *b) { bw32(b, 0x10000); bwz(b, 12); bw32(b, 0x10000); bwz(b, 12); bw32(b, 0x40000000); }

static void mp4_ftyp(BYTE *p)
{
    Bw b; size_t at;
    b.p = p; b.n = 0;
    at = bw_box(&b, "ftyp"); bwtag(&b, "M4A "); bw32(&b, 0); bwtag(&b, "M4A "); bwtag(&b, "mp42"); bwtag(&b, "isom");
    bw_end(&b, at);
}

/* moov for n frames of sizes[] stored contiguously from file offset MP4_DATA_OFF */
static size_t mp4_moov(BYTE *p, const DWORD *sizes, size_t n, int ch, int kbps, const BYTE *asc, int ascLen)
{
    Bw b;
    size_t moov, trak, mdia, minf, stbl, x, y, z, i;
    ULONG64 dur = (ULONG64)n * AAC_FRAME, durMs = dur * 1000 / OUT_RATE;
    size_t chunks = (n + MP4_CHUNK - 1) / MP4_CHUNK, last = n % MP4_CHUNK;
    int v1 = dur > 0xFFFFFFFFULL;
    DWORD maxRate = (DWORD)kbps * 1000;
    b.p = p; b.n = 0;
    moov = bw_box(&b, "moov");
    x = bw_box(&b, "mvhd"); bw32(&b, 0); bw32(&b, 0); bw32(&b, 0); bw32(&b, 1000); bw32(&b, (DWORD)durMs);
    bw32(&b, 0x10000); bw16(&b, 0x100); bwz(&b, 10); bw_matrix(&b); bwz(&b, 24); bw32(&b, 2); bw_end(&b, x);
    trak = bw_box(&b, "trak");
    x = bw_box(&b, "tkhd"); bw32(&b, 7); bw32(&b, 0); bw32(&b, 0); bw32(&b, 1); bw32(&b, 0); bw32(&b, (DWORD)durMs);
    bwz(&b, 8); bw16(&b, 0); bw16(&b, 0); bw16(&b, 0x100); bw16(&b, 0); bw_matrix(&b); bw32(&b, 0); bw32(&b, 0); bw_end(&b, x);
    mdia = bw_box(&b, "mdia");
    x = bw_box(&b, "mdhd");
    if (v1) { bw32(&b, 0x01000000); bw64(&b, 0); bw64(&b, 0); bw32(&b, OUT_RATE); bw64(&b, dur); }
    else    { bw32(&b, 0); bw32(&b, 0); bw32(&b, 0); bw32(&b, OUT_RATE); bw32(&b, (DWORD)dur); }
    bw16(&b, 0x55C4); bw16(&b, 0); bw_end(&b, x);                        /* language "und" */
    x = bw_box(&b, "hdlr"); bw32(&b, 0); bw32(&b, 0); bwtag(&b, "soun"); bwz(&b, 12);
    { const char *nm = "SoundHandler"; while (*nm) bw8(&b, (BYTE)*nm++); bw8(&b, 0); } bw_end(&b, x);
    minf = bw_box(&b, "minf");
    x = bw_box(&b, "smhd"); bw32(&b, 0); bw32(&b, 0); bw_end(&b, x);
    x = bw_box(&b, "dinf"); y = bw_box(&b, "dref"); bw32(&b, 0); bw32(&b, 1);
    z = bw_box(&b, "url "); bw32(&b, 1); bw_end(&b, z); bw_end(&b, y); bw_end(&b, x);
    stbl = bw_box(&b, "stbl");
    x = bw_box(&b, "stsd"); bw32(&b, 0); bw32(&b, 1);
    y = bw_box(&b, "mp4a"); bwz(&b, 6); bw16(&b, 1); bwz(&b, 8); bw16(&b, (unsigned)ch); bw16(&b, 16); bw32(&b, 0);
    bw32(&b, (DWORD)OUT_RATE << 16);
    z = bw_box(&b, "esds"); bw32(&b, 0);
    bw8(&b, 0x03); bw8(&b, (unsigned)(3 + 2 + 13 + 2 + ascLen + 3)); bw16(&b, 0); bw8(&b, 0);
    bw8(&b, 0x04); bw8(&b, (unsigned)(13 + 2 + ascLen)); bw8(&b, 0x40); bw8(&b, 0x15);
    bw8(&b, 0); bw16(&b, 6144 / 8 * (unsigned)ch); bw32(&b, maxRate); bw32(&b, maxRate);
    bw8(&b, 0x05); bw8(&b, (unsigned)ascLen); for (i = 0; i < (size_t)ascLen; i++) bw8(&b, asc[i]);
    bw8(&b, 0x06); bw8(&b, 1); bw8(&b, 0x02);
    bw_end(&b, z); bw_end(&b, y); bw_end(&b, x);
    x = bw_box(&b, "stts"); bw32(&b, 0);
    if (n) { bw32(&b, 1); bw32(&b, (DWORD)n); bw32(&b, AAC_FRAME); } else bw32(&b, 0);
    bw_end(&b, x);
    x = bw_box(&b, "stsc"); bw32(&b, 0);
    if (!n) bw32(&b, 0);
    else if (!last || chunks == 1) { bw32(&b, 1); bw32(&b, 1); bw32(&b, (DWORD)(chunks == 1 ? n : MP4_CHUNK)); bw32(&b, 1); }
    else { bw32(&b, 2); bw32(&b, 1); bw32(&b, MP4_CHUNK); bw32(&b, 1); bw32(&b, (DWORD)chunks); bw32(&b, (DWORD)last); bw32(&b, 1); }
    bw_end(&b, x);
    x = bw_box(&b, "stsz"); bw32(&b, 0); bw32(&b, 0); bw32(&b, (DWORD)n);
    if (b.p) for (i = 0; i < n; i++) bw32(&b, sizes[i]); else b.n += 4 * n;
    bw_end(&b, x);
    x = bw_box(&b, "stco"); bw32(&b, 0); bw32(&b, (DWORD)chunks);
    if (b.p) {
        ULONG64 off = MP4_DATA_OFF;
        for (i = 0; i < n; i++) { if (i % MP4_CHUNK == 0) bw32(&b, (DWORD)off); off += sizes[i]; }
    } else b.n += 4 * chunks;
    bw_end(&b, x);
    bw_end(&b, stbl); bw_end(&b, minf); bw_end(&b, mdia); bw_end(&b, trak); bw_end(&b, moov);
    return b.n;
}

/* append moov after `data` bytes of frames and patch the mdat size; h positioned anywhere */
static BOOL mp4_finish(HANDLE h, const DWORD *sizes, size_t n, ULONG64 data, int ch, int kbps, const BYTE *asc, int ascLen)
{
    size_t ms = mp4_moov(NULL, sizes, n, ch, kbps, asc, ascLen);
    BYTE *buf = (BYTE *)malloc(ms), hd[MP4_DATA_OFF];
    LARGE_INTEGER li;
    BOOL ok;
    if (!buf) return FALSE;
    mp4_moov(buf, sizes, n, ch, kbps, asc, ascLen);
    li.QuadPart = (LONGLONG)(MP4_DATA_OFF + data);
    ok = SetFilePointerEx(h, li, NULL, FILE_BEGIN) && SetEndOfFile(h) && write_all(h, buf, (DWORD)ms);
    free(buf);
    if (!ok) return FALSE;
    mp4_ftyp(hd);
    {   /* big-endian mdat size */
        DWORD v = (DWORD)(8 + data);
        hd[28] = (BYTE)(v >> 24); hd[29] = (BYTE)(v >> 16); hd[30] = (BYTE)(v >> 8); hd[31] = (BYTE)v;
        memcpy(hd + 32, "mdat", 4);
    }
    li.QuadPart = 0;
    ok = SetFilePointerEx(h, li, NULL, FILE_BEGIN) && write_all(h, hd, MP4_DATA_OFF);
    return ok && FlushFileBuffers(h);
}

/* projected size of the current part if one more frame of `extra` bytes is added */
static double live_projected(size_t nf, ULONG64 data)
{
    return (double)MP4_DATA_OFF + (double)data + (double)mp4_moov(NULL, NULL, nf, g_lv.ch, g_lv.kbps, g_lv.asc, g_lv.ascLen);
}

static void live_stats(WavOut *o)
{
    InterlockedExchange64(&g_bytesWritten, (LONG64)(g_lv.closedBytes + (g_lv.hd != INVALID_HANDLE_VALUE ? live_projected(g_lv.nf, g_lv.data) : 0)));
    (void)o;
}

/* ---- part files ---- */
static BOOL live_open(WavOut *o)
{
    WCHAR path[MAX_PATH * 2];
    BYTE hd[LRX_HDR];
    int attempt;
    g_lv.hd = g_lv.hi = INVALID_HANDLE_VALUE;
    for (attempt = 0; attempt < 100; attempt++) {
        WCHAR sfx[32] = L"";
        if (o->part > 1)  _snwprintf(sfx, 31, L"_part%d", o->part);
        if (attempt > 0)  _snwprintf(sfx + wcslen(sfx), 31 - wcslen(sfx), L"_%d", attempt);
        sfx[31] = 0;
        _snwprintf(path, MAX_PATH * 2 - 1, L"%ls\\%ls%ls.m4a", o->folder, o->base, sfx);
        path[MAX_PATH * 2 - 1] = 0;
        g_lv.hd = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_lv.hd != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (g_lv.hd == INVALID_HANDLE_VALUE) {
        o->lastErr = GetLastError();
        LOG_HR(L"Creating output file", HRESULT_FROM_WIN32(o->lastErr));
        return FALSE;
    }
    _snwprintf(g_lv.idxPath, MAX_PATH * 2 + 7, L"%ls.lrx", path); g_lv.idxPath[MAX_PATH * 2 + 7] = 0;
    g_lv.hi = CreateFileW(g_lv.idxPath, GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    memset(hd, 0, sizeof(hd));
    mp4_ftyp(hd);
    memcpy(hd + 32, "mdat", 4);                     /* size patched at finish / recovery */
    if (g_lv.hi == INVALID_HANDLE_VALUE || !write_all(g_lv.hd, hd, MP4_DATA_OFF)) {
        o->lastErr = GetLastError();
        LOG_HR(L"Creating output file", HRESULT_FROM_WIN32(o->lastErr));
        if (g_lv.hi != INVALID_HANDLE_VALUE) { CloseHandle(g_lv.hi); DeleteFileW(g_lv.idxPath); }
        CloseHandle(g_lv.hd); DeleteFileW(path);
        g_lv.hd = g_lv.hi = INVALID_HANDLE_VALUE;
        return FALSE;
    }
    memset(hd, 0, sizeof(hd));
    memcpy(hd, "LRX1", 4);
    put_le32(hd + 4, OUT_RATE); put_le32(hd + 8, (DWORD)g_lv.ch); put_le32(hd + 12, (DWORD)g_lv.kbps);
    put_le32(hd + 16, MP4_DATA_OFF); put_le32(hd + 20, (DWORD)g_lv.ascLen);
    memcpy(hd + 24, g_lv.asc, (size_t)g_lv.ascLen);
    if (!write_all(g_lv.hi, hd, LRX_HDR)) LOG_HR(L"Writing recovery index", HRESULT_FROM_WIN32(GetLastError()));
    FlushFileBuffers(g_lv.hi);
    g_lv.nf = 0; g_lv.data = 0; g_lv.dlen = g_lv.ilen = 0;
    g_lv.parts++;
    wcscpy(o->path, path);
    o->h = g_lv.hd;
    EnterCriticalSection(&g_statusLock);
    wcsncpy(g_curPath, path, MAX_PATH * 2 - 1);
    LeaveCriticalSection(&g_statusLock);
    log_msg(L"Writing %ls (live AAC %d kbps %ls)", path, g_lv.kbps, g_lv.ch == 1 ? L"mono" : L"stereo");
    live_stats(o);
    return TRUE;
}

/* resume: reopen an open-ended part (left by a suspend or a crash), drop anything not covered
   by the index (or a stale moov), and continue appending. Returns FALSE if not possible. */
static BOOL live_reopen(WavOut *o)
{
    WCHAR path[MAX_PATH * 2];
    BYTE hdr[LRX_HDR];
    LARGE_INTEGER isz, dsz, li;
    DWORD got = 0;
    BYTE *raw = NULL;
    size_t cnt, i, n = 0;
    ULONG64 used = 0;
    if (o->part > 1) _snwprintf(path, MAX_PATH * 2 - 1, L"%ls\\%ls_part%d.m4a", o->folder, o->base, o->part);
    else _snwprintf(path, MAX_PATH * 2 - 1, L"%ls\\%ls.m4a", o->folder, o->base);
    path[MAX_PATH * 2 - 1] = 0;
    _snwprintf(g_lv.idxPath, MAX_PATH * 2 + 7, L"%ls.lrx", path); g_lv.idxPath[MAX_PATH * 2 + 7] = 0;
    g_lv.hd = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    g_lv.hi = CreateFileW(g_lv.idxPath, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (g_lv.hd == INVALID_HANDLE_VALUE || g_lv.hi == INVALID_HANDLE_VALUE) goto fail;
    if (!GetFileSizeEx(g_lv.hi, &isz) || !GetFileSizeEx(g_lv.hd, &dsz) || isz.QuadPart < LRX_HDR ||
        !ReadFile(g_lv.hi, hdr, LRX_HDR, &got, NULL) || got != LRX_HDR || memcmp(hdr, "LRX1", 4) ||
        (int)hdr[8] != g_lv.ch) goto fail;
    cnt = (size_t)(isz.QuadPart - LRX_HDR) / 2;
    raw = (BYTE *)malloc(cnt * 2 + 2);
    if (!raw || (cnt && (!ReadFile(g_lv.hi, raw, (DWORD)(cnt * 2), &got, NULL) || got != cnt * 2))) goto fail;
    if (cnt > g_lv.capf) {
        DWORD *p = (DWORD *)realloc(g_lv.sizes, (cnt + 65536) * sizeof(DWORD));
        if (!p) goto fail;
        g_lv.sizes = p; g_lv.capf = cnt + 65536;
    }
    for (i = 0; i < cnt; i++) {
        DWORD sz = (DWORD)(raw[2 * i] | raw[2 * i + 1] << 8);
        if (sz == 0 || MP4_DATA_OFF + used + sz > (ULONG64)dsz.QuadPart) break;
        g_lv.sizes[n++] = sz; used += sz;
    }
    free(raw); raw = NULL;
    li.QuadPart = (LONGLONG)(MP4_DATA_OFF + used);
    if (!SetFilePointerEx(g_lv.hd, li, NULL, FILE_BEGIN) || !SetEndOfFile(g_lv.hd)) goto fail;
    li.QuadPart = (LONGLONG)(LRX_HDR + 2 * n);
    if (!SetFilePointerEx(g_lv.hi, li, NULL, FILE_BEGIN) || !SetEndOfFile(g_lv.hi)) goto fail;
    g_lv.nf = n; g_lv.data = used; g_lv.dlen = g_lv.ilen = 0;
    g_lv.parts++;
    wcscpy(o->path, path);
    o->h = g_lv.hd;
    EnterCriticalSection(&g_statusLock);
    wcsncpy(g_curPath, path, MAX_PATH * 2 - 1);
    LeaveCriticalSection(&g_statusLock);
    log_msg(L"Resuming %ls at %lu frames (%.2f s, %.0f bytes; %lu index entries)", path, (unsigned long)n,
            (double)n * AAC_FRAME / OUT_RATE, (double)used, (unsigned long)cnt);
    live_stats(o);
    return TRUE;
fail:
    free(raw);
    if (g_lv.hd != INVALID_HANDLE_VALUE) CloseHandle(g_lv.hd);
    if (g_lv.hi != INVALID_HANDLE_VALUE) CloseHandle(g_lv.hi);
    g_lv.hd = g_lv.hi = INVALID_HANDLE_VALUE;
    log_msg(L"Cannot reopen %ls for appending (error %lu)", path, (unsigned long)GetLastError());
    return FALSE;
}

/* write pending frames, then their index entries (data first: the index never points past the data) */
static BOOL live_flush_batch(void)
{
    BOOL ok = TRUE;
    if (g_lv.dlen) ok = write_all(g_lv.hd, g_lv.db, (DWORD)g_lv.dlen);
    if (ok && g_lv.ilen && g_lv.hi != INVALID_HANDLE_VALUE && !write_all(g_lv.hi, g_lv.ib, (DWORD)g_lv.ilen))
        LOG_HR(L"Writing recovery index", HRESULT_FROM_WIN32(GetLastError()));
    if (!ok) LOG_HR(L"Writing audio data", HRESULT_FROM_WIN32(GetLastError()));
    g_lv.dlen = g_lv.ilen = 0;
    return ok;
}

static void live_close_part(WavOut *o)
{
    LONG64 sz;
    if (g_lv.hd == INVALID_HANDLE_VALUE) return;
    live_flush_batch();
    if (g_lv.nf == 0) {
        CloseHandle(g_lv.hd); if (g_lv.hi != INVALID_HANDLE_VALUE) CloseHandle(g_lv.hi);
        DeleteFileW(o->path); DeleteFileW(g_lv.idxPath);
        g_lv.hd = g_lv.hi = INVALID_HANDLE_VALUE; o->h = INVALID_HANDLE_VALUE;
        on_part_closed(o->path, 0);
        return;
    }
    if (mp4_finish(g_lv.hd, g_lv.sizes, g_lv.nf, g_lv.data, g_lv.ch, g_lv.kbps, g_lv.asc, g_lv.ascLen)) {
        CloseHandle(g_lv.hd);
        if (g_lv.hi != INVALID_HANDLE_VALUE) CloseHandle(g_lv.hi);
        DeleteFileW(g_lv.idxPath);              /* complete file: index no longer needed */
    } else {
        LOG_HR(L"Finalizing .m4a (kept for recovery at next start)", HRESULT_FROM_WIN32(GetLastError()));
        CloseHandle(g_lv.hd);
        if (g_lv.hi != INVALID_HANDLE_VALUE) CloseHandle(g_lv.hi);
    }
    g_lv.hd = g_lv.hi = INVALID_HANDLE_VALUE; o->h = INVALID_HANDLE_VALUE;
    sz = file_size(o->path);
    if (sz > g_lv.maxPartBytes) g_lv.maxPartBytes = sz;
    g_lv.closedBytes += (ULONG64)(sz > 0 ? sz : 0);
    g_lv.closedFrames += (LONG64)g_lv.nf;
    log_msg(L"LIVE PART file=%ls bytes=%.0f frames=%lu duration=%.2fs", o->path, (double)sz,
            (unsigned long)g_lv.nf, (double)g_lv.nf * AAC_FRAME / OUT_RATE);
    on_part_closed(o->path, g_lv.data);
}

static int ensure_bytes(BYTE **b, size_t *cap, size_t need)
{
    if (need <= *cap) return 1;
    { size_t nc = need * 2 + 4096; BYTE *p = (BYTE *)realloc(*b, nc); if (!p) return 0; *b = p; *cap = nc; }
    return 1;
}

/* one AAC frame from the encoder: roll the part if needed, then queue it */
static BOOL live_frame(WavOut *o, const BYTE *f, DWORD len)
{
    if (len == 0) return TRUE;
    if (len > 0xFFFF) len = 0xFFFF;             /* cannot happen for AAC-LC (max 1536 B/ch) */
    if (g_lv.nf > 0 && ((g_lv.splitFrames && (LONG64)g_lv.nf >= g_lv.splitFrames) ||
                        live_projected(g_lv.nf + 1, g_lv.data + len) > g_lv.limit)) {
        live_close_part(o);
        o->part++;
        log_msg(L"Size/time limit reached, starting part %d", o->part);
        if (!live_open(o)) return FALSE;
        sess_put_int(L"Part", o->part);
        sess_put_int(L"DoneFrames", g_lv.closedFrames);
        sess_put_int(L"DoneBytes", (LONG64)g_lv.closedBytes);
        if (g_gateUsed) sess_put_int(L"CutMs", cut_total_ms());
        sess_put_int(L"PausedMs", paused_total_ms());
    }
    if (g_lv.nf == g_lv.capf) {
        size_t nc = g_lv.capf ? g_lv.capf * 2 : 65536;
        DWORD *p = (DWORD *)realloc(g_lv.sizes, nc * sizeof(DWORD));
        if (!p) return FALSE;
        g_lv.sizes = p; g_lv.capf = nc;
    }
    if (!ensure_bytes(&g_lv.db, &g_lv.dcap, g_lv.dlen + len) || !ensure_bytes(&g_lv.ib, &g_lv.icap, g_lv.ilen + 2)) return FALSE;
    memcpy(g_lv.db + g_lv.dlen, f, len); g_lv.dlen += len;
    put_le16(g_lv.ib + g_lv.ilen, (WORD)len); g_lv.ilen += 2;
    g_lv.sizes[g_lv.nf++] = len;
    g_lv.data += len;
    g_lv.totalFrames++;
    return TRUE;
}

/* ---- encoder ---- */
static HRESULT live_pull(WavOut *o)
{
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob;
        DWORD st = 0;
        HRESULT hr;
        memset(&ob, 0, sizeof(ob));
        if (!g_lv.providesSamples) { IMFMediaBuffer_SetCurrentLength(g_lv.outBuf, 0); ob.pSample = g_lv.outSmp; }
        hr = IMFTransform_ProcessOutput(g_lv.mft, 0, 1, &ob, &st);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (FAILED(hr)) { if (g_lv.providesSamples && ob.pSample) IMFSample_Release(ob.pSample); return hr; }
        if (ob.pSample) {
            IMFMediaBuffer *mb = NULL;
            BYTE *p = NULL;
            DWORD len = 0;
            hr = IMFSample_ConvertToContiguousBuffer(ob.pSample, &mb);
            if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(mb, &p, NULL, &len);
            if (SUCCEEDED(hr)) {
                BOOL ok = live_frame(o, p, len);
                IMFMediaBuffer_Unlock(mb);
                if (!ok) hr = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
            }
            if (mb) IMFMediaBuffer_Release(mb);
            if (g_lv.providesSamples) IMFSample_Release(ob.pSample);
            if (hr == HRESULT_FROM_WIN32(ERROR_WRITE_FAULT)) return hr;
            if (FAILED(hr)) return hr;
        }
    }
}

static HRESULT live_feed(WavOut *o, const short *pcm, DWORD frames)
{
    DWORD bytes = frames * (DWORD)g_lv.ch * 2;
    IMFMediaBuffer *mb = NULL;
    IMFSample *smp = NULL;
    BYTE *p = NULL;
    HRESULT hr;
    if (g_lv.fake) {                         /* dev test: replay pre-encoded frames */
        g_lv.inFrames += frames;
        while ((g_lv.fakeOut + 1) * AAC_FRAME <= g_lv.inFrames) {
            int i = g_lv.fakeI;
            g_lv.fakeI = (g_lv.fakeI + 1) % g_lv.fakeN;
            g_lv.fakeOut++;
            if (!live_frame(o, g_lv.fake + g_lv.fakeOff[i], g_lv.fakeLen[i])) return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        }
        return S_OK;
    }
    hr = mf.CreateMemoryBuffer(bytes, &mb);
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(mb, &p, NULL, NULL);
    if (SUCCEEDED(hr)) { memcpy(p, pcm, bytes); IMFMediaBuffer_Unlock(mb); hr = IMFMediaBuffer_SetCurrentLength(mb, bytes); }
    if (SUCCEEDED(hr)) hr = mf.CreateSample(&smp);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(smp, mb);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(smp, g_lv.inFrames * 10000000LL / OUT_RATE);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(smp, (LONG64)frames * 10000000LL / OUT_RATE);
    if (SUCCEEDED(hr)) {
        hr = IMFTransform_ProcessInput(g_lv.mft, 0, smp, 0);
        if (hr == MF_E_NOTACCEPTING) {
            hr = live_pull(o);
            if (SUCCEEDED(hr)) hr = IMFTransform_ProcessInput(g_lv.mft, 0, smp, 0);
        }
    }
    if (SUCCEEDED(hr)) { g_lv.inFrames += frames; hr = live_pull(o); }
    if (smp) IMFSample_Release(smp);
    if (mb) IMFMediaBuffer_Release(mb);
    return hr;
}

static void live_enc_release(void)
{
    if (g_lv.outSmp) { IMFSample_Release(g_lv.outSmp); g_lv.outSmp = NULL; }
    if (g_lv.outBuf) { IMFMediaBuffer_Release(g_lv.outBuf); g_lv.outBuf = NULL; }
    if (g_lv.mft) { IMFTransform_Release(g_lv.mft); g_lv.mft = NULL; }
    if (g_lv.mfStarted) { mf.Shutdown(); g_lv.mfStarted = 0; }
    free(g_lv.fake); free(g_lv.fakeOff); free(g_lv.fakeLen);
    g_lv.fake = NULL; g_lv.fakeOff = NULL; g_lv.fakeLen = NULL; g_lv.fakeN = 0;
}

/* dev test: load an ADTS file (48 kHz, matching channel count) to stand in for the encoder */
static int live_fake_load(const WCHAR *path, int ch)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    BYTE *buf;
    DWORD got = 0, pos = 0;
    int n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 7 || sz.QuadPart > 64 * 1024 * 1024) { CloseHandle(h); return 0; }
    buf = (BYTE *)malloc((size_t)sz.QuadPart);
    if (!buf || !ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL)) { CloseHandle(h); free(buf); return 0; }
    CloseHandle(h);
    g_lv.fakeOff = (DWORD *)malloc(sizeof(DWORD) * (got / 7 + 1));
    g_lv.fakeLen = (WORD *)malloc(sizeof(WORD) * (got / 7 + 1));
    if (!g_lv.fakeOff || !g_lv.fakeLen) { free(buf); return 0; }
    while (pos + 7 <= got && buf[pos] == 0xFF && (buf[pos + 1] & 0xF6) == 0xF0) {
        int prot = !(buf[pos + 1] & 1), sf = (buf[pos + 2] >> 2) & 15, prof = buf[pos + 2] >> 6;
        int cc = ((buf[pos + 2] & 1) << 2) | (buf[pos + 3] >> 6);
        DWORD flen = ((DWORD)(buf[pos + 3] & 3) << 11) | ((DWORD)buf[pos + 4] << 3) | (buf[pos + 5] >> 5);
        DWORD hl = prot ? 9 : 7;
        if (flen <= hl || pos + flen > got || sf != 3 || cc != ch) break;
        if (n == 0) { WORD a = (WORD)(((prof + 1) << 11) | (sf << 7) | (cc << 3)); g_lv.asc[0] = (BYTE)(a >> 8); g_lv.asc[1] = (BYTE)a; g_lv.ascLen = 2; }
        g_lv.fakeOff[n] = pos + hl; g_lv.fakeLen[n] = (WORD)(flen - hl); n++;
        pos += flen;
    }
    if (n == 0) { free(buf); return 0; }
    g_lv.fake = buf; g_lv.fakeN = n; g_lv.fakeI = 0; g_lv.fakeOut = 0;
    return 1;
}

/* create + configure the AAC encoder; tries the requested rate, then higher documented rates */
static HRESULT live_enc_init(int kbps, int mono)
{
    int tries[6], nTries = 0, i;
    HRESULT hr = E_FAIL;
    MFT_OUTPUT_STREAM_INFO si;
    g_lv.ch = mono ? 1 : 2;
    g_lv.ascLen = 0;
    if (g_fakeAacPath[0]) {
        if (!live_fake_load(g_fakeAacPath, g_lv.ch)) { log_msg(L"Fake AAC source unusable: %ls", g_fakeAacPath); return E_FAIL; }
        g_lv.kbps = kbps;
        log_msg(L"Live encoder: FAKE frames from %ls (%d frames)", g_fakeAacPath, g_lv.fakeN);
        return S_OK;
    }
    if (!g_mfAvailable) return E_NOTIMPL;
    hr = mf.Startup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return hr;
    g_lv.mfStarted = 1;
    tries[nTries++] = kbps;
    for (i = 0; i < 4; i++) if (k_aacRates[i] > kbps) tries[nTries++] = k_aacRates[i];
    for (i = 0; i < nTries; i++) {
        IMFMediaType *tOut = NULL, *tIn = NULL;
        hr = CoCreateInstance(&MY_CLSID_AACMFTEncoder, NULL, CLSCTX_INPROC_SERVER, &MY_IID_IMFTransform, (void **)&g_lv.mft);
        if (FAILED(hr)) { g_lv.mft = NULL; break; }
        hr = mk_audio_type(&tOut, &MFAudioFormat_AAC, g_lv.ch, (UINT32)tries[i] * 125, 0);
        if (SUCCEEDED(hr)) hr = IMFTransform_SetOutputType(g_lv.mft, 0, tOut, 0);
        if (SUCCEEDED(hr)) hr = mk_audio_type(&tIn, &MFAudioFormat_PCM, g_lv.ch, (UINT32)(OUT_RATE * g_lv.ch * 2), 1);
        if (SUCCEEDED(hr)) hr = IMFTransform_SetInputType(g_lv.mft, 0, tIn, 0);
        if (tOut) IMFMediaType_Release(tOut);
        if (tIn) IMFMediaType_Release(tIn);
        if (SUCCEEDED(hr)) { g_lv.kbps = tries[i]; break; }
        { WCHAR b[640]; log_msg(L"Live AAC encoder rejected %d kbps (%ls)", tries[i], hr_str(hr, b, 640)); }
        IMFTransform_Release(g_lv.mft); g_lv.mft = NULL;
    }
    if (FAILED(hr) || !g_lv.mft) { live_enc_release(); return FAILED(hr) ? hr : E_FAIL; }
    {   /* AudioSpecificConfig: from MF_MT_USER_DATA (HEAACWAVEINFO tail), else computed for AAC-LC */
        IMFMediaType *cur = NULL;
        UINT32 bl = 0;
        BYTE ud[64];
        if (SUCCEEDED(IMFTransform_GetOutputCurrentType(g_lv.mft, 0, &cur))) {
            if (SUCCEEDED(IMFMediaType_GetBlob(cur, &MF_MT_USER_DATA, ud, sizeof(ud), &bl)) && bl >= 14 && bl <= 12 + 8) {
                memcpy(g_lv.asc, ud + 12, bl - 12); g_lv.ascLen = (int)(bl - 12);
            }
            IMFMediaType_Release(cur);
        }
        if (!g_lv.ascLen) {
            WORD a = (WORD)((2 << 11) | (3 << 7) | (g_lv.ch << 3));
            g_lv.asc[0] = (BYTE)(a >> 8); g_lv.asc[1] = (BYTE)a; g_lv.ascLen = 2;
        }
    }
    memset(&si, 0, sizeof(si));
    hr = IMFTransform_GetOutputStreamInfo(g_lv.mft, 0, &si);
    if (SUCCEEDED(hr)) {
        g_lv.providesSamples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        if (!g_lv.providesSamples) {
            hr = mf.CreateMemoryBuffer(si.cbSize > 8192 ? si.cbSize : 8192, &g_lv.outBuf);
            if (SUCCEEDED(hr)) hr = mf.CreateSample(&g_lv.outSmp);
            if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(g_lv.outSmp, g_lv.outBuf);
        }
    }
    if (SUCCEEDED(hr)) hr = IMFTransform_ProcessMessage(g_lv.mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr)) hr = IMFTransform_ProcessMessage(g_lv.mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) { live_enc_release(); return hr; }
    log_msg(L"Live AAC encoder ready: %d kbps, %d ch, ASC %02X%02X", g_lv.kbps, g_lv.ch, g_lv.asc[0], g_lv.asc[1]);
    return S_OK;
}

/* encoder failed mid-session: close the .m4a part and continue in WAV (converted after stop) */
static BOOL live_fallback(WavOut *o, HRESULT why)
{
    WCHAR b[640];
    log_msg(L"Live AAC encoding failed (%ls): continuing as WAV, compressed after stop", hr_str(why, b, 640));
    live_close_part(o);
    live_enc_release();
    o->live = 0;
    g_convEnabled = g_mfAvailable && g_convThread;
    o->part++;
    o->splitBytes = (ULONG64)max_part_seconds(g_curKbps) * OUT_RATE * OUT_BPF;
    return wav_open(o);
}

static BOOL live_write(WavOut *o, const BYTE *p, size_t n)
{
    size_t frames = n / OUT_BPF, done = 0;
    while (done < frames) {
        DWORD k = (DWORD)((frames - done) < FEED_FRAMES ? (frames - done) : FEED_FRAMES);
        const short *src = (const short *)(p + done * OUT_BPF);
        HRESULT hr;
        if (g_lv.ch == 1) {
            DWORD i;
            if (!ensure_cap((void **)&g_lv.tmp, &g_lv.tmpCap, k, sizeof(short))) return FALSE;
            for (i = 0; i < k; i++) g_lv.tmp[i] = (short)(((int)src[2 * i] + (int)src[2 * i + 1]) / 2);
            src = g_lv.tmp;
        }
        hr = live_feed(o, src, k);
        if (hr == HRESULT_FROM_WIN32(ERROR_WRITE_FAULT)) { live_stats(o); return FALSE; }   /* disk error */
        if (FAILED(hr)) {
            if (!live_fallback(o, hr)) return FALSE;
            return wav_write(o, p + done * OUT_BPF, (frames - done) * OUT_BPF);
        }
        done += k;
    }
    if (!live_flush_batch()) return FALSE;
    live_stats(o);
    return TRUE;
}

/* writer thread, every loop: durability checkpoint + periodic size log */
static void live_tick(WavOut *o)
{
    LONG64 t = now_ms();
    if (g_lv.hd != INVALID_HANDLE_VALUE && t - g_lv.lastFlushMs >= g_lv.flushEveryMs) {
        g_lv.lastFlushMs = t;
        FlushFileBuffers(g_lv.hd);
        if (g_lv.hi != INVALID_HANDLE_VALUE) FlushFileBuffers(g_lv.hi);
        log_msg(L"Checkpoint: part %d, %lu frames (%.1f s) on disk", o->part, (unsigned long)g_lv.nf,
                (double)g_lv.nf * AAC_FRAME / OUT_RATE);
        if (g_gateUsed) sess_put_int(L"CutMs", cut_total_ms());
        sess_put_int(L"PausedMs", paused_total_ms());
    }
    if (t - g_lv.lastLogMs >= 5000) {
        double sec = (double)(t - g_lv.startMs) / 1000.0;
        g_lv.lastLogMs = t;
        log_msg(L"LIVE t=%.0fs size=%.0f bytes part=%d part_bytes=%.0f est=%.1f MB/h", sec, (double)g_bytesWritten,
                o->part, live_projected(g_lv.nf, g_lv.data), sec > 0 ? ((double)g_bytesWritten - (double)g_lv.closedBytesAtStart) / 1e6 * 3600.0 / sec : 0.0);
    }
}

/* end of session (writer thread): drain the encoder, finish the last part.
   Stop reason 6 = suspend: the part is left open-ended (data + index flushed, no moov)
   so a later session can append to the same file. */
static void live_close(WavOut *o)
{
    int suspend = g_stopReason == 6;
    if (g_lv.mft) {
        HRESULT hr = IMFTransform_ProcessMessage(g_lv.mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (SUCCEEDED(hr)) hr = IMFTransform_ProcessMessage(g_lv.mft, MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (SUCCEEDED(hr)) hr = live_pull(o);
        if (FAILED(hr)) LOG_HR(L"Draining AAC encoder", hr);
    } else if (g_lv.fake && g_lv.inFrames > g_lv.fakeOut * AAC_FRAME) {
        int i = g_lv.fakeI;
        g_lv.fakeOut++;
        live_frame(o, g_lv.fake + g_lv.fakeOff[i], g_lv.fakeLen[i]);
    }
    if (suspend && g_lv.hd != INVALID_HANDLE_VALUE) {
        LONG64 frames = g_lv.closedFrames + (LONG64)g_lv.nf;
        BOOL ok = live_flush_batch();
        live_stats(o);                       /* size so far (no moov yet) for the UI/test summary */
        ok = FlushFileBuffers(g_lv.hd) && ok;
        if (g_lv.hi != INVALID_HANDLE_VALUE) { FlushFileBuffers(g_lv.hi); CloseHandle(g_lv.hi); }
        CloseHandle(g_lv.hd);
        g_lv.hd = g_lv.hi = INVALID_HANDLE_VALUE; o->h = INVALID_HANDLE_VALUE;
        sess_put_int(L"Part", o->part);
        sess_put_int(L"DoneFrames", g_lv.closedFrames);
        sess_put_int(L"DoneBytes", (LONG64)g_lv.closedBytes);
        sess_put_int(L"AudioMs", frames * AAC_FRAME * 1000 / OUT_RATE);
        sess_put_int(L"CutMs", cut_total_ms());
        sess_put_int(L"PausedMs", paused_total_ms());
        sess_put(L"State", L"suspended");
        log_msg(L"SUSPENDED part=%d frames_total=%lld audio=%.2fs ok=%d", o->part, (long long)frames,
                (double)frames * AAC_FRAME / OUT_RATE, ok);
        live_enc_release();
        InterlockedExchange(&g_liveNameOk, 0);
        free(g_lv.sizes); g_lv.sizes = NULL; g_lv.capf = 0;
        free(g_lv.db); g_lv.db = NULL; g_lv.dcap = 0;
        free(g_lv.ib); g_lv.ib = NULL; g_lv.icap = 0;
        free(g_lv.tmp); g_lv.tmp = NULL; g_lv.tmpCap = 0;
        return;
    }
    live_close_part(o);
    live_enc_release();
    live_stats(o);
    sess_clear();                            /* finished normally: nothing to resume */
    InterlockedExchange(&g_liveNameOk, 0);
    if (g_liveNamePath[0]) DeleteFileW(g_liveNamePath);
    free(g_lv.sizes); g_lv.sizes = NULL; g_lv.capf = 0;
    free(g_lv.db); g_lv.db = NULL; g_lv.dcap = 0;
    free(g_lv.ib); g_lv.ib = NULL; g_lv.icap = 0;
    free(g_lv.tmp); g_lv.tmp = NULL; g_lv.tmpCap = 0;
}

/* session name for crash recovery: "<folder>\<internal base>.lrn" (UTF-16, rewritten on edit) */
static void live_save_name(const WCHAR *name)
{
    HANDLE h;
    DWORD w;
    if (!g_liveNameOk || !g_liveNamePath[0]) return;
    sess_put(L"Name", name);
    EnterCriticalSection(&g_renLock);           /* the writer may be renaming the .lrn */
    h = CreateFileW(g_liveNamePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, name, (DWORD)(wcslen(name) * sizeof(WCHAR)), &w, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_renLock);
}

/* ---- crash recovery: rebuild interrupted live parts at startup ---- */
static WCHAR g_recoverMsg[MAX_PATH * 2 + 64];

static int recover_one(const WCHAR *idx, double *secOut)
{
    WCHAR data[MAX_PATH * 2];
    HANDLE hi, hd;
    BYTE hdr[LRX_HDR];
    DWORD got = 0, *sizes = NULL;
    LARGE_INTEGER isz, dsz;
    size_t n = 0, cnt, i;
    ULONG64 used = 0;
    BYTE *raw = NULL;
    int ch, kbps, ascLen, ok = 0;
    size_t L = wcslen(idx);
    *secOut = 0;
    if (L < 9 || L >= MAX_PATH * 2) return -1;
    wcsncpy(data, idx, L - 4); data[L - 4] = 0;
    hi = CreateFileW(idx, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hi == INVALID_HANDLE_VALUE) return -1;
    hd = CreateFileW(data, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hd == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        CloseHandle(hi);
        if (e == ERROR_FILE_NOT_FOUND) DeleteFileW(idx);
        return -1;                               /* in use (another instance) or gone */
    }
    if (!GetFileSizeEx(hi, &isz) || !GetFileSizeEx(hd, &dsz) || isz.QuadPart < LRX_HDR || isz.QuadPart > 256LL * 1024 * 1024 ||
        !ReadFile(hi, hdr, LRX_HDR, &got, NULL) || got != LRX_HDR || memcmp(hdr, "LRX1", 4)) goto out;
    ch = (int)(hdr[8] | hdr[9] << 8); kbps = (int)(hdr[12] | hdr[13] << 8); ascLen = (int)hdr[20];
    if ((ch != 1 && ch != 2) || ascLen < 2 || ascLen > 8) goto out;
    cnt = (size_t)(isz.QuadPart - LRX_HDR) / 2;
    raw = (BYTE *)malloc(cnt * 2 + 1);
    sizes = (DWORD *)malloc((cnt + 1) * sizeof(DWORD));
    if (!raw || !sizes || (cnt && (!ReadFile(hi, raw, (DWORD)(cnt * 2), &got, NULL) || got != cnt * 2))) goto out;
    for (i = 0; i < cnt; i++) {
        DWORD s = (DWORD)(raw[2 * i] | raw[2 * i + 1] << 8);
        if (s == 0 || MP4_DATA_OFF + used + s > (ULONG64)dsz.QuadPart) break;   /* index beyond the data on disk */
        sizes[n++] = s; used += s;
    }
    if (n == 0) { ok = 2; goto out; }
    if (!mp4_finish(hd, sizes, n, used, ch, kbps, hdr + 24, ascLen)) goto out;
    *secOut = (double)n * AAC_FRAME / OUT_RATE;
    log_msg(L"RECOVER file=%ls frames=%lu duration=%.2fs bytes=%.0f (index had %lu frames)", data, (unsigned long)n,
            *secOut, (double)file_size(data), (unsigned long)cnt);
    ok = 1;
out:
    free(raw); free(sizes);
    CloseHandle(hi); CloseHandle(hd);
    if (ok == 2) { DeleteFileW(data); DeleteFileW(idx); log_msg(L"RECOVER %ls: no audio, removed", data); return 0; }
    if (ok == 1) { DeleteFileW(idx); return 1; }
    log_msg(L"RECOVER FAIL %ls (left untouched)", data);
    return -1;
}

static void sanitize_name(const WCHAR *in, WCHAR *out, size_t cch);

/* returns number of recovered sessions; *files / *seconds totals; first output in g_recoverMsg */
static int recover_folder(const WCHAR *folder, const WCHAR *exclude, int *filesOut, double *secOut)
{
    WCHAR pat[MAX_PATH * 2], sess[64][64];
    WIN32_FIND_DATAW fd;
    HANDLE fh;
    int nSess = 0, s, files = 0;
    double total = 0;
    g_recoverMsg[0] = 0;
    *filesOut = 0; *secOut = 0;
    /* 1. rebuild every interrupted part */
    _snwprintf(pat, MAX_PATH * 2 - 1, L"%ls\\*.m4a.lrx", folder); pat[MAX_PATH * 2 - 1] = 0;
    fh = FindFirstFileW(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return 0;
    do {
        WCHAR full[MAX_PATH * 2], b[MAX_PATH];
        double sec = 0;
        size_t bl;
        _snwprintf(full, MAX_PATH * 2 - 1, L"%ls\\%ls", folder, fd.cFileName); full[MAX_PATH * 2 - 1] = 0;
        /* session base: file name without a trailing ".m4a.lrx" and "_partN" (names may contain either) */
        wcsncpy(b, fd.cFileName, MAX_PATH - 1); b[MAX_PATH - 1] = 0;
        bl = wcslen(b);
        if (bl > 8 && !_wcsicmp(b + bl - 8, L".m4a.lrx")) b[bl -= 8] = 0;
        {
            size_t q = bl;
            while (q > 0 && b[q - 1] >= L'0' && b[q - 1] <= L'9') q--;
            if (q < bl && q >= 5 && !wcsncmp(b + q - 5, L"_part", 5)) b[q - 5] = 0;
        }
        if (exclude && exclude[0] && !_wcsicmp(b, exclude)) continue;   /* resumable session */
        if (wcslen(b) >= 64) { log_msg(L"RECOVER skipped %ls: name too long", fd.cFileName); continue; }
        if (recover_one(full, &sec) < 0) continue;
        files++; total += sec;
        for (s = 0; s < nSess && wcscmp(sess[s], b); s++) {}
        if (s == nSess && nSess < 64) wcscpy(sess[nSess++], b);
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    /* 2. rename each session (all its parts, including ones finished before the crash) */
    for (s = 0; s < nSess; s++) {
        WCHAR lrn[MAX_PATH * 2], raw[256] = L"", name[300], clean[256];
        HANDLE h;
        int part, id = -1000 - s;
        _snwprintf(lrn, MAX_PATH * 2 - 1, L"%ls\\%ls.lrn", folder, sess[s]); lrn[MAX_PATH * 2 - 1] = 0;
        h = CreateFileW(lrn, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            if (ReadFile(h, raw, sizeof(raw) - sizeof(WCHAR), &got, NULL)) raw[got / sizeof(WCHAR)] = 0;
            CloseHandle(h);
        }
        sanitize_name(raw, clean, 256);
        _snwprintf(name, 299, L"%ls (recuperata)", clean[0] ? clean : sess[s]); name[299] = 0;
        EnterCriticalSection(&g_convLock);
        for (part = 1; part <= SESS_PARTS_MAX && g_partCount < SESS_PARTS_MAX; part++) {
            WCHAR base[MAX_PATH * 2], m4a[MAX_PATH * 2 + 8];
            if (part == 1) _snwprintf(base, MAX_PATH * 2 - 1, L"%ls\\%ls", folder, sess[s]);
            else _snwprintf(base, MAX_PATH * 2 - 1, L"%ls\\%ls_part%d", folder, sess[s], part);
            base[MAX_PATH * 2 - 1] = 0;
            _snwprintf(m4a, MAX_PATH * 2 + 7, L"%ls.m4a", base); m4a[MAX_PATH * 2 + 7] = 0;
            if (!path_exists(m4a)) { if (part > 1) break; else continue; }
            g_parts[g_partCount].sess = id;
            wcscpy(g_parts[g_partCount].base, base);
            g_partCount++;
        }
        LeaveCriticalSection(&g_convLock);
        finalize_session(id, name);
        DeleteFileW(lrn);
        EnterCriticalSection(&g_convLock);
        if (!g_recoverMsg[0] && g_finFirst[0]) wcsncpy(g_recoverMsg, g_finFirst, MAX_PATH * 2 - 1);
        log_msg(L"RECOVERED session %ls -> %ls (%d file(s))", sess[s], g_finFirst, g_finCount);
        g_finValid = 0;
        LeaveCriticalSection(&g_convLock);
    }
    *filesOut = files; *secOut = total;
    return nSess;
}

/* ================================================================== */
/* Session control (shared by GUI and test mode)                       */
/* ================================================================== */
#define WM_APP_STOPPED (WM_APP + 1)
#define WM_APP_TRAY    (WM_APP + 2)

static WavOut g_wav;
static HANDLE g_writerThread;
static HWND   g_notifyHwnd;            /* receives WM_APP_STOPPED */
static int    g_testSplitSec;          /* test mode: --split-seconds */
static volatile LONG g_recording;      /* 1 from start until finalized */
static LONG64 g_sessStartMs, g_sessEndMs;

static unsigned __stdcall supervisor_thread(void *arg)
{
    (void)arg;
    WaitForSingleObject(g_captureThread, INFINITE);
    g_sessEndMs = now_ms();                          /* timer stops when capture stops */
    WaitForSingleObject(g_writerThread, INFINITE);   /* writer finalizes the file */
    CloseHandle(g_writerThread);  g_writerThread = NULL;
    CloseHandle(g_captureThread); g_captureThread = NULL;
    InterlockedExchange64(&g_silRunF, 0);
    log_msg(L"Session finished: %.1f MB, %.1f s synthesized silence, %.1f s dropped, %ld reconnects",
            (double)g_bytesWritten / (1024.0 * 1024.0), (double)g_framesPadded / OUT_RATE,
            (double)g_framesDropped / OUT_RATE, (long)g_reconnects);
    if (g_gateUsed)
        log_msg(L"Silence cut: %.1f s removed in %ld cut(s) this run, %.1f s total for the session "
                L"(%d dBFS, min %d s, margin %d ms)", (double)g_cutFrames / OUT_RATE, (long)g_gateCuts,
                cut_total_ms() / 1000.0, g_gateDb, g_gateMinSec, g_gatePadMs);
    gate_free();
    InterlockedExchange(&g_recording, 0);
    SetEvent(g_doneEvent);
    if (g_notifyHwnd) PostMessageW(g_notifyHwnd, WM_APP_STOPPED, 0, 0);
    return 0;
}

static int make_folder(const WCHAR *in, WCHAR *out, WCHAR *err, size_t cch)
{
    DWORD n = GetFullPathNameW(in, MAX_PATH, out, NULL);
    size_t L;
    int r;
    if (n == 0 || n >= MAX_PATH) { _snwprintf(err, cch - 1, L"Cartella non valida:\n%ls", in); err[cch - 1] = 0; return 0; }
    L = wcslen(out);
    while (L > 3 && (out[L - 1] == L'\\' || out[L - 1] == L'/')) out[--L] = 0;
    r = SHCreateDirectoryExW(NULL, out, NULL);
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) {
        WCHAR b[640];
        _snwprintf(err, cch - 1, L"Impossibile creare la cartella:\n%ls\n\n%ls", out, hr_str(HRESULT_FROM_WIN32((DWORD)r), b, 640));
        err[cch - 1] = 0;
        return 0;
    }
    return 1;
}

static LONG64 g_resumeBaseMs;         /* audio already recorded before this run (resumed session) */
static LONG64 g_fileBaseMs;           /* audio already in the file(s) before this run (resumed session) */

/* resume = 1: continue the session described in session.ini (suspended or crashed) */
static int rec_start(const WCHAR *folder, int splitMin, int resume, WCHAR *err, size_t cch)
{
    SYSTEMTIME st;
    HANDLE sup;
    WCHAR rFolder[MAX_PATH] = L"";
    int kbps = g_aacKbps, mono = g_mono;
    if (g_recording) return 0;
    InterlockedExchange(&g_pauseReq, 0); InterlockedExchange(&g_pausedAck, 0);
    g_pausedTotalMs = 0; g_pauseStartMs = 0; g_pausedBaseMs = 0; g_resumeBaseMs = 0; g_cutBaseMs = 0; g_fileBaseMs = 0;
    gate_init();
    /* reset per-session state */
    g_ringW = 0; g_ringR = 0;
    g_framesOut = g_framesPadded = g_framesDropped = g_bytesWritten = 0;
    g_reconnects = g_discont = g_haveDevice = g_writeError = 0;
    g_stopReason = 0; g_peakNow = g_peakMax = 0; g_rmsNow = 0;
    g_srcState = 0; g_srcEverOk = 0; g_srcNotice = 0; g_srcAttaches = g_srcActFails = g_srcExits = 0; g_srcPid = 0; g_srcLastHr = S_OK;
    g_srcSwitchReq = 0; g_srcSwitches = 0; ResetEvent(g_srcSwitchEvent);
    ResetEvent(g_stopEvent); ResetEvent(g_doneEvent); ResetEvent(g_devChangedEvent);
    EnterCriticalSection(&g_statusLock);
    wcscpy(g_devName, L"(none)"); g_fmtDesc[0] = 0; g_curPath[0] = 0;
    LeaveCriticalSection(&g_statusLock);

    memset(&g_wav, 0, sizeof(g_wav));
    g_wav.h = INVALID_HANDLE_VALUE;
    if (resume) {
        GetPrivateProfileStringW(L"Session", L"Folder", L"", rFolder, MAX_PATH, g_sessIni);
        GetPrivateProfileStringW(L"Session", L"Base", L"", g_wav.base, 64, g_sessIni);
        if (!rFolder[0] || !g_wav.base[0]) {
            _snwprintf(err, cch - 1, L"Nessuna sessione da riprendere."); err[cch - 1] = 0;
            SetEvent(g_doneEvent); return 0;
        }
        folder = rFolder;
        /* the resumed session keeps its source (absent in sessions of <= 1.3.x: whole system) */
        GetPrivateProfileStringW(L"Session", L"SourceExe", L"", g_curSrcExe, 64, g_sessIni);
        GetPrivateProfileStringW(L"Session", L"SourceName", L"", g_curSrcName, 128, g_sessIni);
        kbps = (int)sess_get_int(L"Kbps", g_aacKbps);
        mono = sess_get_int(L"Mono", 0) != 0;
    }
    if (!resume) { wcscpy(g_curSrcExe, g_startSrcExe); wcscpy(g_curSrcName, g_startSrcName); }
    if (g_curSrcExe[0] && !g_curSrcName[0]) wcsncpy(g_curSrcName, g_curSrcExe, 127);
    if (!g_curSrcExe[0]) g_curSrcName[0] = 0;
    if (!make_folder(folder, g_wav.folder, err, cch)) { SetEvent(g_doneEvent); return 0; }
    if (!resume) {
        GetLocalTime(&st);
        _snwprintf(g_wav.base, 63, L"loopback_%04u-%02u-%02u_%02u-%02u-%02u",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        g_wav.base[63] = 0;
        g_wav.part = 1;
    } else {
        g_wav.part = (int)sess_get_int(L"Part", 1);
        if (g_wav.part < 1 || g_wav.part > SESS_PARTS_MAX) g_wav.part = 1;
    }
    g_curKbps = kbps; g_curMono = mono; g_curKeep = g_keepWav;
    /* live AAC (default) when possible; otherwise WAV + conversion after stop */
    memset(&g_lv, 0, sizeof(g_lv));
    g_lv.hd = g_lv.hi = INVALID_HANDLE_VALUE;
    g_curLive = 0;
    g_liveNamePath[0] = 0;
    if (resume || (g_compress && g_compressLive && (g_mfAvailable || g_fakeAacPath[0]))) {
        HRESULT hr = live_enc_init(kbps, mono);
        if (SUCCEEDED(hr)) {
            g_curLive = 1;
            g_curKbps = g_lv.kbps;
            g_wav.live = 1;
            g_lv.limit = M4A_MAX_BYTES;
            g_lv.splitFrames = g_testSplitSec > 0 ? (LONG64)g_testSplitSec * OUT_RATE / AAC_FRAME
                             : splitMin > 0 ? (LONG64)splitMin * 60 * OUT_RATE / AAC_FRAME : 0;
            if (resume) {           /* keep the limits the session was started with */
                g_lv.limit = (double)sess_get_int(L"MaxFileMB", g_maxFileMB) * 1000000.0;
                g_lv.splitFrames = sess_get_int(L"SplitFrames", g_lv.splitFrames);
                g_lv.closedFrames = sess_get_int(L"DoneFrames", 0);
                g_lv.closedBytes = (ULONG64)sess_get_int(L"DoneBytes", 0);
                g_lv.parts = g_wav.part - 1;
            }
            g_lv.flushEveryMs = g_testSegSec > 0 ? (LONG64)g_testSegSec * 1000 : (LONG64)g_segMin * 60000;
            g_lv.startMs = g_lv.lastFlushMs = g_lv.lastLogMs = now_ms();
            _snwprintf(g_liveNamePath, MAX_PATH * 2 - 1, L"%ls\\%ls.lrn", g_wav.folder, g_wav.base);
            g_liveNamePath[MAX_PATH * 2 - 1] = 0;
        } else {
            WCHAR b[640];
            if (resume) {
                _snwprintf(err, cch - 1, L"Impossibile riprendere: codificatore AAC non disponibile.\n\n%ls", hr_str(hr, b, 640));
                err[cch - 1] = 0;
                SetEvent(g_doneEvent); return 0;
            }
            log_msg(L"Live AAC encoder unavailable (%ls): recording WAV, compressing after stop", hr_str(hr, b, 640));
        }
    }
    g_convEnabled = g_compress && g_mfAvailable && g_convThread && !g_curLive;
    g_wav.splitBytes = splitMin > 0 ? (ULONG64)splitMin * 60 * OUT_RATE * OUT_BPF : 0;
    {
        /* keep every output file under MaxFileMB: from the AAC bitrate, or the WAV size itself */
        ULONG64 cap = g_convEnabled ? (ULONG64)max_part_seconds(g_aacKbps) * OUT_RATE * OUT_BPF
                                    : (ULONG64)(M4A_MAX_BYTES - 44.0);
        cap -= cap % OUT_BPF;
        if (cap >= OUT_RATE * OUT_BPF && (!g_wav.splitBytes || cap < g_wav.splitBytes)) g_wav.splitBytes = cap;
    }
    InterlockedIncrement(&g_sessId);
    if (g_testSplitSec > 0) g_wav.splitBytes = (ULONG64)g_testSplitSec * OUT_RATE * OUT_BPF;
    EnterCriticalSection(&g_convLock);
    g_sessConvOk = g_sessConvFail = 0; g_sessM4aBytes = 0; g_sessM4aSec = 0;
    g_convLastOut[0] = 0; g_convLastFailWav[0] = 0; g_convLastErr[0] = 0;
    g_finValid = 0; g_finCount = 0; g_finFailed = 0; g_finBytes = 0; g_finFirst[0] = 0; g_finFailWav[0] = 0;
    LeaveCriticalSection(&g_convLock);
    if (resume) {
        int k;
        /* parts finished in earlier runs belong to this session too */
        EnterCriticalSection(&g_convLock);
        for (k = 1; k < g_wav.part && g_partCount < SESS_PARTS_MAX; k++) {
            if (k == 1) _snwprintf(g_parts[g_partCount].base, MAX_PATH * 2 - 1, L"%ls\\%ls", g_wav.folder, g_wav.base);
            else _snwprintf(g_parts[g_partCount].base, MAX_PATH * 2 - 1, L"%ls\\%ls_part%d", g_wav.folder, g_wav.base, k);
            g_parts[g_partCount].base[MAX_PATH * 2 - 1] = 0;
            g_parts[g_partCount].sess = (int)g_sessId;
            g_partCount++;
        }
        LeaveCriticalSection(&g_convLock);
    }
    if (resume && !live_reopen(&g_wav)) {
        /* the open part is gone or was completed: continue with a new part */
        WCHAR cur[MAX_PATH * 2];
        if (g_wav.part > 1) _snwprintf(cur, MAX_PATH * 2 - 1, L"%ls\\%ls_part%d.m4a", g_wav.folder, g_wav.base, g_wav.part);
        else _snwprintf(cur, MAX_PATH * 2 - 1, L"%ls\\%ls.m4a", g_wav.folder, g_wav.base);
        cur[MAX_PATH * 2 - 1] = 0;
        if (path_exists(cur)) {
            EnterCriticalSection(&g_convLock);
            if (g_partCount < SESS_PARTS_MAX) {
                wcsncpy(g_parts[g_partCount].base, cur, wcslen(cur) - 4);
                g_parts[g_partCount].base[wcslen(cur) - 4] = 0;
                g_parts[g_partCount].sess = (int)g_sessId;
                g_partCount++;
            }
            LeaveCriticalSection(&g_convLock);
            g_wav.part++;
        }
        g_wav.live = 1;
        if (!wav_open(&g_wav)) {
            WCHAR b[640];
            live_enc_release();
            _snwprintf(err, cch - 1, L"Impossibile riprendere nella cartella:\n%ls\n\n%ls", g_wav.folder,
                       hr_str(HRESULT_FROM_WIN32(g_wav.lastErr), b, 640));
            err[cch - 1] = 0;
            SetEvent(g_doneEvent);
            return 0;
        }
    } else if (!resume && !wav_open(&g_wav)) {
        WCHAR b[640];
        if (g_curLive) live_enc_release();
        _snwprintf(err, cch - 1, L"Impossibile creare il file nella cartella:\n%ls\n\n%ls", g_wav.folder,
                   hr_str(HRESULT_FROM_WIN32(g_wav.lastErr), b, 640));
        err[cch - 1] = 0;
        SetEvent(g_doneEvent);
        return 0;
    }
    if (g_curLive) {
        InterlockedExchange(&g_liveNameOk, 1);
        g_lv.totalFrames = g_lv.closedFrames + (LONG64)g_lv.nf;
        g_resumeBaseMs = g_lv.totalFrames * AAC_FRAME * 1000 / OUT_RATE;
        g_fileBaseMs = resume ? g_resumeBaseMs : 0;   /* the big timer continues from the file's length */
        if (resume) {               /* the timer shows real time: add silence cut in earlier runs */
            g_cutBaseMs = sess_get_int(L"CutMs", 0);
            if (g_cutBaseMs < 0) g_cutBaseMs = 0;
            g_resumeBaseMs += g_cutBaseMs;
            g_pausedBaseMs = sess_get_int(L"PausedMs", 0);   /* absent in sessions of <= 1.3.7: 0 */
            if (g_pausedBaseMs < 0) g_pausedBaseMs = 0;
        } else { sess_put_int(L"CutMs", 0); sess_put_int(L"PausedMs", 0); }
        g_lv.closedBytesAtStart = (ULONG64)g_bytesWritten;
        sess_put(L"State", L"active");
        if (!resume) {
            sess_put(L"Folder", g_wav.folder);
            sess_put(L"Base", g_wav.base);
            sess_put_int(L"Kbps", g_lv.kbps);
            sess_put_int(L"Mono", mono);
            sess_put_int(L"MaxFileMB", g_maxFileMB);
            sess_put_int(L"SplitFrames", g_lv.splitFrames);
            sess_put_int(L"Part", 1);
            sess_put_int(L"DoneFrames", 0);
            sess_put_int(L"DoneBytes", 0);
            sess_put(L"SourceExe", g_curSrcExe[0] ? g_curSrcExe : NULL);
            sess_put(L"SourceName", g_curSrcExe[0] ? g_curSrcName : NULL);
        } else {
            log_msg(L"RESUMED session %ls part %d, %.2f s already recorded (cut %.2f s, paused %.2f s before)", g_wav.base,
                    g_wav.part, g_resumeBaseMs / 1000.0, g_cutBaseMs / 1000.0, g_pausedBaseMs / 1000.0);
        }
        log_msg(L"Recording started: LIVE AAC %d kbps %ls, max %d MB/file, split %d min, checkpoint every %.0f s",
                g_lv.kbps, g_mono ? L"mono" : L"stereo", g_maxFileMB, splitMin, g_lv.flushEveryMs / 1000.0);
    } else
    log_msg(L"Recording started (split: %d min, part limit %.0f s, AAC after stop: %ls %d kbps %ls, keep WAV %d)",
            splitMin, (double)g_wav.splitBytes / (OUT_RATE * OUT_BPF), g_convEnabled ? L"on" : L"off",
            g_aacKbps, g_mono ? L"mono" : L"stereo", g_keepWav);
    if (g_curSrcExe[0]) log_msg(L"Source: app %ls (%ls), process loopback incl. child processes", g_curSrcName, g_curSrcExe);
    else log_msg(L"Source: whole system (default output device)");
    g_start100ns = now_100ns();
    g_sessStartMs = now_ms(); g_sessEndMs = 0;
    InterlockedExchange(&g_recording, 1);

    g_captureThread = (HANDLE)_beginthreadex(NULL, 0, capture_thread, NULL, 0, NULL);
    if (!g_captureThread) goto fail_nothreads;
    g_writerThread = (HANDLE)_beginthreadex(NULL, 0, writer_thread, &g_wav, 0, NULL);
    if (!g_writerThread) {
        request_stop(4);
        WaitForSingleObject(g_captureThread, INFINITE);
        CloseHandle(g_captureThread); g_captureThread = NULL;
        goto fail_nothreads;
    }
    sup = (HANDLE)_beginthreadex(NULL, 0, supervisor_thread, NULL, 0, NULL);
    if (!sup) {
        /* cannot supervise: stop synchronously */
        request_stop(4);
        WaitForSingleObject(g_captureThread, INFINITE);
        WaitForSingleObject(g_writerThread, INFINITE);
        CloseHandle(g_writerThread); g_writerThread = NULL;
        CloseHandle(g_captureThread); g_captureThread = NULL;
        InterlockedExchange(&g_recording, 0);
        SetEvent(g_doneEvent);
        _snwprintf(err, cch - 1, L"Impossibile avviare i thread di registrazione."); err[cch - 1] = 0;
        return 0;
    }
    CloseHandle(sup);
    return 1;

fail_nothreads:
    wav_close(&g_wav);
    InterlockedExchange(&g_recording, 0);
    SetEvent(g_doneEvent);
    _snwprintf(err, cch - 1, L"Impossibile avviare i thread di registrazione."); err[cch - 1] = 0;
    return 0;
}

static LONG64 free_bytes(const WCHAR *folder)
{
    ULARGE_INTEGER avail;
    WCHAR root[MAX_PATH];
    if (GetDiskFreeSpaceExW(folder, &avail, NULL, NULL)) return (LONG64)avail.QuadPart;
    /* folder may not exist yet: ask its volume instead */
    if (GetVolumePathNameW(folder, root, MAX_PATH) && GetDiskFreeSpaceExW(root, &avail, NULL, NULL))
        return (LONG64)avail.QuadPart;
    return -1;
}
/* ---- pause / resume (UI + test mode) ---- */
static void pause_set(int on)
{
    if (!g_recording || on == (g_pauseReq != 0)) return;
    if (on) { g_pauseStartMs = now_ms(); InterlockedExchange(&g_pauseReq, 1); log_msg(L"Pause requested"); }
    else {
        g_pausedTotalMs += now_ms() - g_pauseStartMs;
        g_pauseStartMs = 0;
        InterlockedExchange(&g_pauseReq, 0);
        InterlockedExchange(&g_silMeterReset, 1);
        if (g_curLive) sess_put_int(L"PausedMs", paused_total_ms());
        log_msg(L"Resume requested (paused %.1f s this run, %.1f s in the session)", g_pausedTotalMs / 1000.0,
                paused_total_ms() / 1000.0);
    }
}

/* ---- pending (suspended or crashed) session ---- */
static int    g_pending, g_pendCrashed;
static WCHAR  g_pendName[256], g_pendFolder[MAX_PATH], g_pendBase[64];
static LONG64 g_pendMs;
static LONG64 g_pendCutMs;            /* silence cut in the pending session (timer = audio + cut) */

/* frames recoverable from an open-ended part (index entries covered by data) */
static LONG64 part_frames_on_disk(const WCHAR *m4a)
{
    WCHAR idx[MAX_PATH * 2 + 8];
    HANDLE h;
    LARGE_INTEGER isz;
    LONG64 dsz = file_size(m4a), n = 0;
    ULONG64 used = 0;
    BYTE buf[8192];
    DWORD got;
    _snwprintf(idx, MAX_PATH * 2 + 7, L"%ls.lrx", m4a); idx[MAX_PATH * 2 + 7] = 0;
    h = CreateFileW(idx, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE || dsz < MP4_DATA_OFF) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); return 0; }
    if (GetFileSizeEx(h, &isz) && isz.QuadPart > LRX_HDR) {
        LARGE_INTEGER li; li.QuadPart = LRX_HDR;
        SetFilePointerEx(h, li, NULL, FILE_BEGIN);
        while (ReadFile(h, buf, sizeof(buf), &got, NULL) && got >= 2) {
            DWORD i;
            for (i = 0; i + 1 < got; i += 2) {
                DWORD sz = (DWORD)(buf[i] | buf[i + 1] << 8);
                if (sz == 0 || MP4_DATA_OFF + used + sz > (ULONG64)dsz) { CloseHandle(h); return n; }
                used += sz; n++;
            }
        }
    }
    CloseHandle(h);
    return n;
}

static void sess_part_path(WCHAR *out, const WCHAR *folder, const WCHAR *base, int part, const WCHAR *ext)
{
    if (part > 1) _snwprintf(out, MAX_PATH * 2 - 1, L"%ls\\%ls_part%d%ls", folder, base, part, ext);
    else _snwprintf(out, MAX_PATH * 2 - 1, L"%ls\\%ls%ls", folder, base, ext);
    out[MAX_PATH * 2 - 1] = 0;
}

/* ---- live rename: the files carry the chosen name while recording ------------------------------
   The UI (debounced) or test mode queues a name; the writer thread, which owns the open handles,
   renames every file of the session to <name>[ (k)][_partN] - the same scheme the final rename
   uses, so finalize_session() later finds src == dst and does nothing.
   - open parts are renamed through their own handle (opened with DELETE access, FileRenameInfo);
   - finished live parts, the .lrx index and the .lrn name file are renamed with MoveFileExW;
   - live sessions are journaled in session.ini (PrevBase + Base) so a crash in the middle is
     completed at the next start (sess_fix_journal), and any failure is rolled back. */
#define LIVE_NAME_MAX 55
static WCHAR g_renReqName[256];

static void rename_request(const WCHAR *raw)
{
    WCHAR clean[256];
    if (!g_recording) return;
    sanitize_name(raw, clean, 256);
    EnterCriticalSection(&g_renLock);
    wcscpy(g_renReqName, clean);
    LeaveCriticalSection(&g_renLock);
    InterlockedIncrement(&g_renReqVer);
}

static BOOL rename_by_handle(HANDLE h, const WCHAR *dst)
{
    size_t L = wcslen(dst), sz = sizeof(FILE_RENAME_INFO) + (L + 1) * sizeof(WCHAR);
    FILE_RENAME_INFO *ri = (FILE_RENAME_INFO *)calloc(1, sz);
    BOOL ok;
    if (!ri) { SetLastError(ERROR_OUTOFMEMORY); return FALSE; }
    ri->ReplaceIfExists = FALSE;
    ri->RootDirectory = NULL;
    ri->FileNameLength = (DWORD)(L * sizeof(WCHAR));
    memcpy(ri->FileName, dst, L * sizeof(WCHAR));
    ok = SetFileInformationByHandle(h, FileRenameInfo, ri, (DWORD)sz);
    free(ri);
    return ok;
}

typedef struct { WCHAR src[MAX_PATH * 2], dst[MAX_PATH * 2]; HANDLE h; int isLrn; } RenOp;

static int ren_do(RenOp *op)
{
    if (op->h != INVALID_HANDLE_VALUE) return rename_by_handle(op->h, op->dst);
    return MoveFileExW(op->src, op->dst, 0);
}
static void ren_undo(RenOp *op)
{
    BOOL ok = op->h != INVALID_HANDLE_VALUE ? rename_by_handle(op->h, op->src) : MoveFileExW(op->dst, op->src, 0);
    if (!ok) log_msg(L"LIVE RENAME rollback failed for %ls (error %lu)", op->dst, (unsigned long)GetLastError());
}

static void rename_apply(WavOut *o)
{
    static RenOp ops[2 * SESS_PARTS_MAX + 4];
    WCHAR name[256], cand[80] = L"", old[64];
    int n = 0, i, k, p, chosen = 0;
    DWORD err = 0;
    LONG ver = g_renReqVer;
    if (ver == g_renAppliedVer) return;
    g_renAppliedVer = ver;
    EnterCriticalSection(&g_renLock);
    wcscpy(name, g_renReqName);
    LeaveCriticalSection(&g_renLock);
    if (!name[0]) return;
    if (wcslen(name) > LIVE_NAME_MAX) { log_msg(L"LIVE RENAME skipped: name longer than %d characters (renamed at stop)", LIVE_NAME_MAX); return; }
    if (o->live ? g_lv.hd == INVALID_HANDLE_VALUE : o->h == INVALID_HANDLE_VALUE) return;
    if (o->part < 1 || o->part > SESS_PARTS_MAX) return;
    wcscpy(old, o->base);
    /* the first " (k)" for which no file of this session would land on somebody else's file */
    for (k = 1; k < 100 && !chosen; k++) {
        int clash = 0;
        if (k == 1) _snwprintf(cand, 79, L"%ls", name); else _snwprintf(cand, 79, L"%ls (%d)", name, k);
        cand[79] = 0;
        if (!wcscmp(cand, old)) return;                       /* already named like this */
        for (p = (o->live ? 1 : o->part); p <= o->part && !clash; p++) {
            static const WCHAR *exts[3] = { L".m4a", L".m4a.lrx", L".wav" };
            int e;
            for (e = 0; e < 3 && !clash; e++) {
                WCHAR s[MAX_PATH * 2], d[MAX_PATH * 2];
                sess_part_path(s, o->folder, old, p, exts[e]);
                sess_part_path(d, o->folder, cand, p, exts[e]);
                if (wcslen(d) >= MAX_PATH - 12) clash = 2;
                else if (_wcsicmp(s, d) && path_exists(d)) clash = 1;
            }
        }
        if (o->live && !clash) {
            WCHAR s[MAX_PATH * 2], d[MAX_PATH * 2];
            sess_part_path(s, o->folder, old, 1, L".lrn");
            sess_part_path(d, o->folder, cand, 1, L".lrn");
            if (_wcsicmp(s, d) && path_exists(d)) clash = 1;
        }
        if (clash == 2) { log_msg(L"LIVE RENAME skipped: path too long (renamed at stop)"); return; }
        if (!clash) chosen = k;
    }
    if (!chosen) { log_msg(L"LIVE RENAME skipped: no free name for \"%ls\" (renamed at stop)", name); return; }

    /* plan: finished parts, then the open part (index, data), then the name file */
    for (p = (o->live ? 1 : o->part); p <= o->part; p++) {
        static const WCHAR *lext[2] = { L".m4a.lrx", L".m4a" };
        int e;
        if (!o->live) {                                        /* WAV: only the open part */
            RenOp *op = &ops[n];
            wcscpy(op->src, o->path);
            sess_part_path(op->dst, o->folder, cand, p, L".wav");
            op->h = o->h; op->isLrn = 0;
            if (wcscmp(op->src, op->dst)) n++;
            continue;
        }
        for (e = 0; e < 2; e++) {
            RenOp *op = &ops[n];
            int cur = (p == o->part);
            if (cur) wcscpy(op->src, e == 0 ? g_lv.idxPath : o->path);
            else sess_part_path(op->src, o->folder, old, p, lext[e]);
            sess_part_path(op->dst, o->folder, cand, p, lext[e]);
            op->h = cur ? (e == 0 ? g_lv.hi : g_lv.hd) : INVALID_HANDLE_VALUE;
            op->isLrn = 0;
            if (cur && op->h == INVALID_HANDLE_VALUE) continue;
            if (!cur && !path_exists(op->src)) continue;      /* finished parts have no index */
            n++;
        }
    }
    if (o->live) {
        RenOp *op = &ops[n];
        sess_part_path(op->src, o->folder, old, 1, L".lrn");
        sess_part_path(op->dst, o->folder, cand, 1, L".lrn");
        op->h = INVALID_HANDLE_VALUE; op->isLrn = 1;
        n++;
    }
    if (o->live) { sess_put(L"PrevBase", old); sess_put(L"Base", cand); }      /* journal */
    EnterCriticalSection(&g_renLock);           /* the UI must not rewrite the .lrn meanwhile */
    for (i = 0; i < n; i++) {
        if (ops[i].isLrn && !path_exists(ops[i].src)) continue;
        if (!ren_do(&ops[i])) { err = GetLastError(); break; }
    }
    if (i < n) {
        int j;
        log_msg(L"LIVE RENAME failed (%ls -> %ls, error %lu): keeping \"%ls\", the final name is set at stop",
                base_name(ops[i].src), base_name(ops[i].dst), (unsigned long)err, old);
        for (j = i - 1; j >= 0; j--) if (!ops[j].isLrn || path_exists(ops[j].dst)) ren_undo(&ops[j]);
        LeaveCriticalSection(&g_renLock);
        if (o->live) { sess_put(L"Base", old); sess_put(L"PrevBase", NULL); }
        return;
    }
    if (o->live) sess_part_path(g_liveNamePath, o->folder, cand, 1, L".lrn");
    LeaveCriticalSection(&g_renLock);
    if (o->live) sess_put(L"PrevBase", NULL);
    /* in-memory state follows the files */
    wcscpy(o->base, cand);
    if (o->live) {
        sess_part_path(o->path, o->folder, cand, o->part, L".m4a");
        _snwprintf(g_lv.idxPath, MAX_PATH * 2 + 7, L"%ls.lrx", o->path); g_lv.idxPath[MAX_PATH * 2 + 7] = 0;
    } else sess_part_path(o->path, o->folder, cand, o->part, L".wav");
    EnterCriticalSection(&g_statusLock);
    wcsncpy(g_curPath, o->path, MAX_PATH * 2 - 1);
    LeaveCriticalSection(&g_statusLock);
    if (o->live) {
        EnterCriticalSection(&g_convLock);
        for (i = 0; i < g_partCount; i++) {
            if (g_parts[i].sess != (int)g_sessId) continue;
            for (p = 1; p < o->part; p++) {
                WCHAR ob[MAX_PATH * 2];
                sess_part_path(ob, o->folder, old, p, L"");
                if (!_wcsicmp(g_parts[i].base, ob)) { sess_part_path(g_parts[i].base, o->folder, cand, p, L""); break; }
            }
        }
        LeaveCriticalSection(&g_convLock);
    }
    log_msg(L"LIVE RENAME OK: %ls -> %ls (%d file(s))", old, cand, n);
}

/* next start: complete a rename that was interrupted by a crash (roll forward to Base) */
static void sess_fix_journal(void)
{
    WCHAR prev[64], base[64], folder[MAX_PATH], s[MAX_PATH * 2], d[MAX_PATH * 2];
    int part, p, e, moved = 0;
    static const WCHAR *exts[2] = { L".m4a", L".m4a.lrx" };
    GetPrivateProfileStringW(L"Session", L"PrevBase", L"", prev, 64, g_sessIni);
    if (!prev[0]) return;
    GetPrivateProfileStringW(L"Session", L"Base", L"", base, 64, g_sessIni);
    GetPrivateProfileStringW(L"Session", L"Folder", L"", folder, MAX_PATH, g_sessIni);
    part = (int)sess_get_int(L"Part", 1);
    if (base[0] && folder[0] && wcscmp(prev, base) && part >= 1 && part <= SESS_PARTS_MAX) {
        for (p = 1; p <= part; p++) for (e = 0; e < 2; e++) {
            sess_part_path(s, folder, prev, p, exts[e]);
            sess_part_path(d, folder, base, p, exts[e]);
            if (path_exists(s) && !path_exists(d)) { if (MoveFileExW(s, d, 0)) moved++; else log_msg(L"Journal: cannot move %ls (error %lu)", s, (unsigned long)GetLastError()); }
        }
        sess_part_path(s, folder, prev, 1, L".lrn");
        sess_part_path(d, folder, base, 1, L".lrn");
        if (path_exists(s) && !path_exists(d) && MoveFileExW(s, d, 0)) moved++;
        log_msg(L"Interrupted live rename completed: %ls -> %ls (%d file(s) moved)", prev, base, moved);
    }
    sess_put(L"PrevBase", NULL);
}

/* look for a session left by "Chiudi e riprendi dopo", logoff or a crash */
static int sess_load_pending(void)
{
    WCHAR state[32], m4a[MAX_PATH * 2];
    int part, k, any = 0;
    g_pending = 0;
    if (!g_sessIni[0] || !path_exists(g_sessIni)) return 0;
    sess_fix_journal();
    GetPrivateProfileStringW(L"Session", L"State", L"", state, 32, g_sessIni);
    GetPrivateProfileStringW(L"Session", L"Folder", L"", g_pendFolder, MAX_PATH, g_sessIni);
    GetPrivateProfileStringW(L"Session", L"Base", L"", g_pendBase, 64, g_sessIni);
    GetPrivateProfileStringW(L"Session", L"Name", L"", g_pendName, 256, g_sessIni);
    part = (int)sess_get_int(L"Part", 1);
    for (k = 1; k <= part; k++) { sess_part_path(m4a, g_pendFolder, g_pendBase, k, L".m4a"); if (path_exists(m4a)) any = 1; }
    if (!state[0] || !g_pendFolder[0] || !g_pendBase[0] || !any) {
        log_msg(L"Stale session.ini removed (no files)");
        sess_clear();
        return 0;
    }
    sess_part_path(m4a, g_pendFolder, g_pendBase, part, L".m4a");
    g_pendMs = (sess_get_int(L"DoneFrames", 0) + part_frames_on_disk(m4a)) * AAC_FRAME * 1000 / OUT_RATE;
    g_pendCutMs = sess_get_int(L"CutMs", 0);
    if (g_pendCutMs < 0) g_pendCutMs = 0;
    g_pendCrashed = wcscmp(state, L"suspended") != 0;
    g_pending = 1;
    log_msg(L"Pending session: %ls \"%ls\" %.1f s (%ls)", g_pendBase, g_pendName, g_pendMs / 1000.0,
            g_pendCrashed ? L"interrupted" : L"suspended");
    return 1;
}

/* "Salva ora": complete the open part and rename the whole session */
static void sess_save_now(void)
{
    WCHAR m4a[MAX_PATH * 2], idx[MAX_PATH * 2 + 8], clean[256];
    int part = (int)sess_get_int(L"Part", 1), k, id = -500;
    double sec = 0;
    if (!g_pending) return;
    for (k = 1; k <= part; k++) {
        sess_part_path(m4a, g_pendFolder, g_pendBase, k, L".m4a");
        _snwprintf(idx, MAX_PATH * 2 + 7, L"%ls.lrx", m4a); idx[MAX_PATH * 2 + 7] = 0;
        if (path_exists(idx)) recover_one(idx, &sec);
    }
    EnterCriticalSection(&g_convLock);
    for (k = 1; k <= part && g_partCount < SESS_PARTS_MAX; k++) {
        sess_part_path(m4a, g_pendFolder, g_pendBase, k, L"");
        g_parts[g_partCount].sess = id;
        wcscpy(g_parts[g_partCount].base, m4a);
        g_partCount++;
    }
    g_finValid = 0;
    LeaveCriticalSection(&g_convLock);
    sanitize_name(g_pendName, clean, 256);
    finalize_session(id, clean);
    sess_part_path(m4a, g_pendFolder, g_pendBase, 1, L".lrn");
    DeleteFileW(m4a);
    sess_clear();
    g_pending = 0;
    log_msg(L"Pending session saved: %ls", g_finFirst);
}

/* send one file to the Recycle Bin (falls back to a plain delete if the bin is unavailable) */
static void recycle_file(const WCHAR *path)
{
    WCHAR buf[MAX_PATH * 2 + 2];
    SHFILEOPSTRUCTW op;
    size_t L = wcslen(path);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    if (L + 2 > sizeof(buf) / sizeof(buf[0])) { DeleteFileW(path); return; }
    memcpy(buf, path, L * sizeof(WCHAR));
    buf[L] = 0; buf[L + 1] = 0;                    /* double-NUL terminated list */
    memset(&op, 0, sizeof(op));
    op.hwnd = NULL;
    op.wFunc = FO_DELETE;
    op.pFrom = buf;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
        log_msg(L"Recycle Bin failed for %ls, deleting", path);
        DeleteFileW(path);
    }
}

/* "Scarta": move every file of the pending session to the Recycle Bin */
static void sess_discard(void)
{
    WCHAR f[MAX_PATH * 2];
    int part = (int)sess_get_int(L"Part", 1), k;
    static const WCHAR *exts[3] = { L".m4a", L".m4a.lrx", L".lrn" };
    int e;
    if (!g_pending) return;
    for (k = 1; k <= part; k++) for (e = 0; e < 3; e++) {
        sess_part_path(f, g_pendFolder, g_pendBase, k, exts[e]);
        recycle_file(f);
    }
    sess_clear();
    g_pending = 0;
    log_msg(L"Pending session %ls moved to the Recycle Bin", g_pendBase);
}

#define LOW_DISK_WARN   (2000LL * 1024 * 1024)
#define LOW_DISK_STOP   (300LL * 1024 * 1024)

static double peak_db(LONG v) { return v > 0 ? 20.0 * log10(v / 1000000.0) : -120.0; }

/* ================================================================== */
/* Settings (%APPDATA%\LoopbackRec\settings.ini)                        */
/* ================================================================== */
static const GUID MY_FOLDERID_RoamingAppData = {0x3EB685DB,0x65F9,0x4CF6,{0xA0,0x3A,0xE3,0xEF,0x65,0x72,0x9F,0x3D}};
static const GUID MY_CLSID_FileOpenDialog    = {0xDC1C5A9C,0xE88A,0x4DDE,{0xA5,0xA1,0x60,0xF8,0x2A,0x20,0xAE,0xF7}};
static const GUID MY_IID_IFileOpenDialog     = {0xD57C7288,0xD4AD,0x4768,{0xBE,0x02,0x9D,0x96,0x95,0x32,0xD9,0x60}};
static const GUID MY_IID_IShellItem          = {0x43826D1E,0xE718,0x42EE,{0xBC,0x55,0xA1,0xE2,0x61,0xC3,0x7B,0xFE}};

static WCHAR g_appDir[MAX_PATH], g_iniPath[MAX_PATH];
static WCHAR g_saveFolder[MAX_PATH];
static int   g_splitMin;
static int   g_winX = CW_USEDEFAULT, g_winY = CW_USEDEFAULT;
static WCHAR g_template[256] = L"{data} {ora}", g_templateCustom[256];
static WCHAR g_srcExe[64], g_srcName[128];   /* chosen source: "" = Tutto il sistema */

static void default_folder(WCHAR *out)
{
    PWSTR docs = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&MY_FOLDERID_Documents, 0, NULL, &docs))) {
        _snwprintf(out, MAX_PATH - 1, L"%ls\\Recordings", docs);
        CoTaskMemFree(docs);
    } else {
        GetCurrentDirectoryW(MAX_PATH, out);
    }
    out[MAX_PATH - 1] = 0;
}

static void settings_init(void)
{
    PWSTR ad = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&MY_FOLDERID_RoamingAppData, 0, NULL, &ad))) {
        _snwprintf(g_appDir, MAX_PATH - 1, L"%ls\\LoopbackRec", ad);
        CoTaskMemFree(ad);
    } else {
        GetTempPathW(MAX_PATH, g_appDir);
        wcsncat(g_appDir, L"LoopbackRec", MAX_PATH - wcslen(g_appDir) - 1);
    }
    g_appDir[MAX_PATH - 1] = 0;
    SHCreateDirectoryExW(NULL, g_appDir, NULL);
    _snwprintf(g_iniPath, MAX_PATH - 1, L"%ls\\settings.ini", g_appDir);   g_iniPath[MAX_PATH - 1] = 0;
    _snwprintf(g_logPath, MAX_PATH - 1, L"%ls\\log.txt", g_appDir);       g_logPath[MAX_PATH - 1] = 0;
    _snwprintf(g_sessIni, MAX_PATH - 1, L"%ls\\session.ini", g_appDir);   g_sessIni[MAX_PATH - 1] = 0;
    if (g_testMode) return;          /* headless test: leave the GUI's log and settings alone */
    {
        /* keep the log small: rotate at 1 MB */
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 1024 * 1024)) {
            WCHAR old[MAX_PATH];
            _snwprintf(old, MAX_PATH - 1, L"%ls\\log.old.txt", g_appDir); old[MAX_PATH - 1] = 0;
            MoveFileExW(g_logPath, old, MOVEFILE_REPLACE_EXISTING);
        }
    }
    /* a UTF-16 BOM makes the *PrivateProfile*W APIs store Unicode paths correctly */
    if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(g_iniPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            static const BYTE bom[2] = {0xFF, 0xFE};
            DWORD n;
            WriteFile(h, bom, 2, &n, NULL);
            CloseHandle(h);
        }
    }
}

static void settings_load(void)
{
    WCHAR buf[MAX_PATH];
    GetPrivateProfileStringW(L"Settings", L"SaveFolder", L"", buf, MAX_PATH, g_iniPath);
    if (buf[0]) wcscpy(g_saveFolder, buf); else default_folder(g_saveFolder);
    g_splitMin = (int)GetPrivateProfileIntW(L"Settings", L"SplitMinutes", 0, g_iniPath);
    if (g_splitMin < 0 || g_splitMin > 24 * 60) g_splitMin = 0;
    g_compress = GetPrivateProfileIntW(L"Settings", L"Compress", 1, g_iniPath) != 0;
    g_aacKbps  = aac_limit_kbps((int)GetPrivateProfileIntW(L"Settings", L"AacKbps", 96, g_iniPath)); /* 96/128/160/192 */
    g_maxFileMB = (int)GetPrivateProfileIntW(L"Settings", L"MaxFileMB", 190, g_iniPath);
    if (g_maxFileMB < 20 || g_maxFileMB > 4000) g_maxFileMB = 190;
    GetPrivateProfileStringW(L"Settings", L"NameTemplate", L"{data} {ora}", g_template, 256, g_iniPath);
    if (!g_template[0]) wcscpy(g_template, L"{data} {ora}");
    GetPrivateProfileStringW(L"Settings", L"NameTemplateCustom", L"", g_templateCustom, 256, g_iniPath);
    g_mono     = GetPrivateProfileIntW(L"Settings", L"Mono", 0, g_iniPath) != 0;
    g_keepWav  = GetPrivateProfileIntW(L"Settings", L"KeepWav", 0, g_iniPath) != 0;
    g_compressLive = GetPrivateProfileIntW(L"Settings", L"CompressLive", 1, g_iniPath) != 0;
    g_segMin = (int)GetPrivateProfileIntW(L"Settings", L"SegmentMinutes", 5, g_iniPath);
    if (g_segMin < 1 || g_segMin > 60) g_segMin = 5;
    g_silCut    = GetPrivateProfileIntW(L"Settings", L"SilenceCut", 0, g_iniPath) != 0;
    g_silMinSec = (int)GetPrivateProfileIntW(L"Settings", L"SilenceMinSec", 3, g_iniPath);
    if (g_silMinSec < 1 || g_silMinSec > 60) g_silMinSec = 3;
    g_silPadMs  = (int)GetPrivateProfileIntW(L"Settings", L"SilencePadMs", 500, g_iniPath);
    if (g_silPadMs < 0 || g_silPadMs > 5000) g_silPadMs = 500;
    g_silDb     = (int)GetPrivateProfileIntW(L"Settings", L"SilenceDb", -45, g_iniPath);
    if (g_silDb < -70 || g_silDb > -20) g_silDb = -45;
    GetPrivateProfileStringW(L"Settings", L"SourceExe", L"", g_srcExe, 64, g_iniPath);
    GetPrivateProfileStringW(L"Settings", L"SourceName", L"", g_srcName, 128, g_iniPath);
    if (!g_srcExe[0]) g_srcName[0] = 0;
    else if (!g_srcName[0]) wcsncpy(g_srcName, g_srcExe, 127);
    {
        int x = (int)GetPrivateProfileIntW(L"Window", L"X", -32000, g_iniPath);
        int y = (int)GetPrivateProfileIntW(L"Window", L"Y", -32000, g_iniPath);
        POINT p; p.x = x + 40; p.y = y + 15;
        if (x != -32000 && MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) { g_winX = x; g_winY = y; }
    }
}

static void ini_int(const WCHAR *sec, const WCHAR *key, int v)
{
    WCHAR b[32];
    _snwprintf(b, 31, L"%d", v); b[31] = 0;
    WritePrivateProfileStringW(sec, key, b, g_iniPath);
}

static void settings_save(HWND hwnd)
{
    WritePrivateProfileStringW(L"Settings", L"SaveFolder", g_saveFolder, g_iniPath);
    ini_int(L"Settings", L"SplitMinutes", g_splitMin);
    ini_int(L"Settings", L"Compress", g_compress);
    ini_int(L"Settings", L"AacKbps", g_aacKbps);
    ini_int(L"Settings", L"Mono", g_mono);
    ini_int(L"Settings", L"KeepWav", g_keepWav);
    ini_int(L"Settings", L"MaxFileMB", g_maxFileMB);
    ini_int(L"Settings", L"CompressLive", g_compressLive);
    ini_int(L"Settings", L"SegmentMinutes", g_segMin);
    ini_int(L"Settings", L"SilenceCut", g_silCut);
    ini_int(L"Settings", L"SilenceMinSec", g_silMinSec);
    ini_int(L"Settings", L"SilencePadMs", g_silPadMs);
    ini_int(L"Settings", L"SilenceDb", g_silDb);
    WritePrivateProfileStringW(L"Settings", L"NameTemplate", g_template, g_iniPath);
    WritePrivateProfileStringW(L"Settings", L"NameTemplateCustom", g_templateCustom, g_iniPath);
    WritePrivateProfileStringW(L"Settings", L"SourceExe", g_srcExe, g_iniPath);
    WritePrivateProfileStringW(L"Settings", L"SourceName", g_srcExe[0] ? g_srcName : L"", g_iniPath);
    if (hwnd) {
        WINDOWPLACEMENT wp;
        wp.length = sizeof(wp);
        if (GetWindowPlacement(hwnd, &wp)) {
            ini_int(L"Window", L"X", wp.rcNormalPosition.left);
            ini_int(L"Window", L"Y", wp.rcNormalPosition.top);
        }
    }
}

/* ================================================================== */
/* GUI                                                                 */
/* ================================================================== */
#define APP_TITLE   L"Loopback Rec"
#define WND_CLASS   L"LoopbackRecGuiWnd"
#define CLIENT_W    460
#define CLIENT_H    370
#define IDC_TOGGLE  101
#define IDC_FOLDER  102
#define IDC_OPEN    103
#define IDC_NAME    105
#define IDC_TEMPLATE 106
#define IDC_SETTINGS 107
#define IDC_PAUSE   108
#define IDC_DISCARD 109
#define IDC_TPLBTN  110
#define IDC_SRCBTN  111
#define IDM_TPL0    500
#define IDM_SRC_SYS 600         /* "Tutto il sistema" */
#define IDM_SRC0    601         /* apps: IDM_SRC0 + index */
#define IDM_SRC_KEEP 690        /* chosen app that is not running now */
#define IDM_SRC_REFRESH 691
#define HOTKEY_PAUSE 2
#define HOTKEY_ID   1
#define TIMER_UI    1
#define TIMER_DISK  2
#define TIMER_TRAY  3
#define TIMER_RENAME 4          /* live rename debounce after typing in the name box */
#define RENAME_DEBOUNCE_MS 1700
#define WM_APP_TPL  (WM_APP + 4)
#define WM_APP_TPL_FOCUS (WM_APP + 5)

/* settings dialog (resource in loopback_rec_gui.rc) */
#define IDD_SETTINGS    200
#define IDC_S_COMPRESS  201
#define IDC_S_QUALITY   202
#define IDC_S_CHANNELS  203
#define IDC_S_KEEPWAV   204
#define IDC_S_MAXMB     205
#define IDC_S_SPLIT     206
#define IDC_S_NOTE      207
#define IDC_S_INFO      208
#define IDC_S_MODE      209
#define IDC_S_SEG       210
#define IDC_S_SILCUT    211
#define IDC_S_SILMIN    212
#define IDC_S_SILPAD    213
#define IDC_S_SILDB     214

static HINSTANCE g_hinst;
static HWND  g_hwnd, g_btnToggle, g_btnFolder, g_btnOpen, g_btnSettings, g_editName, g_cbTemplate, g_btnTpl;
static int   g_tplCustomShown;    /* custom pattern row visible */
static HWND  g_btnPause, g_btnDiscard, g_btnSrc;
static int   g_hotkeyPOk, g_layoutPending = -1;
static UINT  g_dpi = 96;
static HFONT g_fUI, g_fSmall, g_fBig, g_fBtn, g_fBold;
static HICON g_icoBig, g_icoSmall;
static HICON g_icoRecBig, g_icoRecSmall, g_icoPauBig, g_icoPauSmall, g_icoCurSmall;
static int   g_iconState = -1;
static HICON g_ovlRec, g_ovlPau, g_ovlSil;   /* taskbar overlay badges (like OBS); gray dot = silence being cut */
static ITaskbarList3 *g_taskbar;
static UINT  g_msgTbButton;       /* 0 idle, 1 rec, 2 paused */
static SYSTEMTIME g_tplST;          /* recording start time used for {data}/{ora} while recording */
static int   g_tplUseST;            /* 1 while recording: render with g_tplST and the committed {n} */
static int   g_hotkeyOk;
static int   g_stopping, g_closeAfterStop, g_trayAdded;
static UINT  g_msgTaskbarCreated;
static double g_meterDb = -120.0;
static int    g_meterStale;            /* UI ticks without a new level block */
static LONG64 g_freeNow = -1;
static WCHAR g_lastFile[MAX_PATH * 2];
static WCHAR g_lastTitle[128];
static int   g_notifyAfterConv;   /* show the "saved" notification when the queue drains */
static int   g_exitWhenIdle;      /* close requested: exit once conversions finish */
static int   g_settingName;       /* programmatic SetWindowText on the name box */
static int   g_nameEdited;        /* user typed in the name box since the last render */
static LONG  g_finalizedSess;     /* session whose rename was already queued */

typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef BOOL (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
static PFN_GetDpiForWindow          p_GetDpiForWindow;
static PFN_AdjustWindowRectExForDpi p_AdjustWindowRectExForDpi;

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static void dpi_init(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    PFN_SetProcessDpiAwarenessContext setCtx = NULL;
    if (u) {
        setCtx = (PFN_SetProcessDpiAwarenessContext)(void (*)(void))GetProcAddress(u, "SetProcessDpiAwarenessContext");
        p_GetDpiForWindow = (PFN_GetDpiForWindow)(void (*)(void))GetProcAddress(u, "GetDpiForWindow");
        p_AdjustWindowRectExForDpi = (PFN_AdjustWindowRectExForDpi)(void (*)(void))GetProcAddress(u, "AdjustWindowRectExForDpi");
    }
    /* the manifest already requests PerMonitorV2; this is the runtime fallback chain */
    if (!(setCtx && (setCtx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */) || GetLastError() == ERROR_ACCESS_DENIED)))
        SetProcessDPIAware();
}

static UINT window_dpi(HWND h)
{
    if (p_GetDpiForWindow) { UINT d = p_GetDpiForWindow(h); if (d) return d; }
    {
        HDC dc = GetDC(h);
        UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(h, dc);
        return d ? d : 96;
    }
}

static HFONT mk_font(int pt, int weight)
{
    return CreateFontW(-MulDiv(pt, (int)g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void fonts_create(void)
{
    HFONT old[5] = { g_fUI, g_fSmall, g_fBig, g_fBtn, g_fBold };
    int i;
    g_fUI    = mk_font(9, FW_NORMAL);
    g_fSmall = mk_font(8, FW_NORMAL);
    g_fBold  = mk_font(9, FW_SEMIBOLD);
    g_fBig   = mk_font(26, FW_SEMIBOLD);
    g_fBtn   = mk_font(11, FW_SEMIBOLD);
    for (i = 0; i < 5; i++) if (old[i]) DeleteObject(old[i]);
}

static void set_window_size(HWND h)
{
    RECT r;
    DWORD style = (DWORD)GetWindowLongPtrW(h, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE);
    r.left = 0; r.top = 0; r.right = S(CLIENT_W); r.bottom = S(CLIENT_H);
    if (p_AdjustWindowRectExForDpi) p_AdjustWindowRectExForDpi(&r, style, FALSE, ex, g_dpi);
    else AdjustWindowRectEx(&r, style, FALSE, ex);
    SetWindowPos(h, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void layout(void)
{
    int ny = g_tplCustomShown ? 166 : 148;
    MoveWindow(g_cbTemplate, S(116), S(134), S(324), S(300), TRUE);
    ShowWindow(g_cbTemplate, g_tplCustomShown ? SW_SHOW : SW_HIDE);
    MoveWindow(g_editName,   S(116), S(ny), S(256), S(25), TRUE);
    MoveWindow(g_btnTpl,     S(374), S(ny), S(24), S(25), TRUE);
    SendMessageW(g_btnTpl, WM_SETFONT, (WPARAM)g_fSmall, TRUE);
    g_layoutPending = g_pending && !g_recording;
    if (g_layoutPending) {      /* Riprendi sessione | Salva ora | Scarta */
        MoveWindow(g_btnToggle,  S(20),  S(204), S(212), S(46), TRUE);
        MoveWindow(g_btnPause,   S(238), S(204), S(100), S(46), TRUE);
        MoveWindow(g_btnDiscard, S(344), S(204), S(96),  S(46), TRUE);
    } else {                    /* Avvia/Ferma | Pausa */
        MoveWindow(g_btnToggle,  S(20),  S(204), S(310), S(46), TRUE);
        MoveWindow(g_btnPause,   S(336), S(204), S(104), S(46), TRUE);
    }
    ShowWindow(g_btnDiscard, g_layoutPending ? SW_SHOW : SW_HIDE);
    SendMessageW(g_btnPause, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    SendMessageW(g_btnDiscard, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    MoveWindow(g_btnFolder,  S(20),  S(310), S(104), S(28), TRUE);
    MoveWindow(g_btnOpen,    S(130), S(310), S(112), S(28), TRUE);
    MoveWindow(g_btnSettings,S(326), S(310), S(114), S(28), TRUE);
    SendMessageW(g_btnSrc, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    SendMessageW(g_btnToggle, WM_SETFONT, (WPARAM)g_fBtn, TRUE);
    SendMessageW(g_btnFolder, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    SendMessageW(g_btnOpen,   WM_SETFONT, (WPARAM)g_fUI, TRUE);
    SendMessageW(g_btnSettings, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    SendMessageW(g_editName,  WM_SETFONT, (WPARAM)g_fUI, TRUE);
    SendMessageW(g_cbTemplate,WM_SETFONT, (WPARAM)g_fUI, TRUE);
}

static void fmt_hms(WCHAR *b, size_t cch, LONG64 ms)
{
    LONG64 s = ms / 1000;
    _snwprintf(b, cch - 1, L"%02d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    b[cch - 1] = 0;
}

/* compact duration: m:ss, or h:mm:ss from one hour */
static void fmt_dur(WCHAR *b, size_t cch, LONG64 ms)
{
    LONG64 s = ms > 0 ? ms / 1000 : 0;
    if (s >= 3600) _snwprintf(b, cch - 1, L"%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else _snwprintf(b, cch - 1, L"%d:%02d", (int)(s / 60), (int)(s % 60));
    b[cch - 1] = 0;
}

/* silence length: tenths of a second under 5 s ("3.2 s"), m:ss from 5 s */
static void fmt_sil(WCHAR *b, size_t cch, LONG64 ms)
{
    if (ms < 0) ms = 0;
    if (ms < 5000) { _snwprintf(b, cch - 1, L"%d.%d s", (int)(ms / 1000), (int)(ms / 100 % 10)); b[cch - 1] = 0; }
    else fmt_dur(b, cch, ms);
}

static void fmt_size(WCHAR *b, size_t cch, LONG64 bytes)
{
    if (bytes < 0) wcsncpy(b, L"?", cch);
    else if (bytes >= 1024LL * 1024 * 1024) _snwprintf(b, cch - 1, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else _snwprintf(b, cch - 1, L"%.0f MB", bytes / (1024.0 * 1024));
    b[cch - 1] = 0;
}

/* recorded time: excludes pauses, includes audio recorded before a resume */
static LONG64 elapsed_ms(void)
{
    LONG64 end, p;
    if (!g_recording && !g_sessEndMs) return 0;
    end = g_sessEndMs ? g_sessEndMs : now_ms();     /* g_sessEndMs: capture stopped (file being finalized) */
    p = g_pausedTotalMs + (g_pauseReq && g_pauseStartMs ? end - g_pauseStartMs : 0);
    return g_resumeBaseMs + end - g_sessStartMs - p;
}

/* length of the audio in the file(s): what the big timer shows. Counts only frames already
   handed to the encoder (g_fileFrames, writer thread). Silence held back by the cut's lookahead
   is not counted until it is known to be kept, so the value never goes backward: during a
   silence it stops after the trailing padding; if the silence turns out shorter than the
   minimum (kept) or the sound resumes (lead-in kept) it catches up forward. Frozen while paused. */
static LONG64 file_ms(void)
{
    if (!g_recording && !g_sessEndMs) return 0;
    return g_fileBaseMs + atomic_read64(&g_fileFrames) * 1000 / OUT_RATE;
}

/* ---------------- file name templates ---------------- */
static const WCHAR *k_tplPresets[] = {
    L"{data} {ora}", L"Chiamata {data} {ora}", L"Riunione - {titolo} - {data}", L"{titolo} {data}", L"Lezione {data}"
};
#define TPL_PRESETS 5
static const WCHAR *k_giorni[7] = { L"domenica", L"luned\u00EC", L"marted\u00EC", L"mercoled\u00EC",
                                    L"gioved\u00EC", L"venerd\u00EC", L"sabato" };

static void today_str(WCHAR *b, const SYSTEMTIME *st)
{
    _snwprintf(b, 15, L"%04u-%02u-%02u", st->wYear, st->wMonth, st->wDay);
    b[15] = 0;
}

/* {n}: progressive counter per day, stored in settings.ini [Counter] */
static int counter_next(const SYSTEMTIME *st)
{
    WCHAR d[16], today[16];
    today_str(today, st);
    GetPrivateProfileStringW(L"Counter", L"Date", L"", d, 16, g_iniPath);
    if (wcscmp(d, today) != 0) return 1;
    return (int)GetPrivateProfileIntW(L"Counter", L"N", 0, g_iniPath) + 1;
}

static void counter_commit(void)
{
    SYSTEMTIME st;
    WCHAR today[16];
    int n;
    GetLocalTime(&st);
    n = counter_next(&st);
    today_str(today, &st);
    WritePrivateProfileStringW(L"Counter", L"Date", today, g_iniPath);
    ini_int(L"Counter", L"N", n);
}

static void render_template(const WCHAR *tpl, WCHAR *out, size_t cch)
{
    SYSTEMTIME st;
    size_t o = 0;
    const WCHAR *p = tpl;
    if (g_tplUseST) st = g_tplST; else GetLocalTime(&st);
    while (*p && o + 1 < cch) {
        if (*p == L'{') {
            const WCHAR *e = wcschr(p, L'}');
            if (e && e - p - 1 < 31) {
                WCHAR tok[32], val[64] = L"";
                int known = 1;
                size_t L = (size_t)(e - p - 1), i;
                wcsncpy(tok, p + 1, L); tok[L] = 0;
                if (!_wcsicmp(tok, L"data"))        _snwprintf(val, 63, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
                else if (!_wcsicmp(tok, L"ora"))    _snwprintf(val, 63, L"%02u-%02u", st.wHour, st.wMinute);
                else if (!_wcsicmp(tok, L"giorno")) wcscpy(val, k_giorni[st.wDayOfWeek % 7]);
                else if (!_wcsicmp(tok, L"titolo")) wcscpy(val, L"Titolo");
                else if (!_wcsicmp(tok, L"n"))      { int nn = counter_next(&st) - (g_tplUseST ? 1 : 0); _snwprintf(val, 63, L"%d", nn < 1 ? 1 : nn); }
                else known = 0;
                if (known) {
                    val[63] = 0;
                    for (i = 0; val[i] && o + 1 < cch; i++) out[o++] = val[i];
                    p = e + 1;
                    continue;
                }
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* make a safe file name (no extension): invalid chars -> '-', trimmed, <=120 chars,
   no reserved device names, a typed .m4a/.wav extension is dropped */
static void sanitize_name(const WCHAR *in, WCHAR *out, size_t cch)
{
    static const WCHAR *reserved[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5",
        L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
    WCHAR t[256];
    size_t i, n = 0, L;
    for (i = 0; in[i] && n < 255; i++) {
        WCHAR c = in[i];
        t[n++] = (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'-' : c;
    }
    t[n] = 0;
    for (;;) {
        L = wcslen(t);
        while (L && (t[L - 1] == L' ' || t[L - 1] == L'.')) t[--L] = 0;
        if (L >= 4 && (!_wcsicmp(t + L - 4, L".m4a") || !_wcsicmp(t + L - 4, L".wav"))) { t[L - 4] = 0; continue; }
        break;
    }
    i = 0;
    while (t[i] == L' ') i++;
    if (wcslen(t + i) > 120) { t[i + 120] = 0; L = wcslen(t); while (L && (t[L - 1] == L' ' || t[L - 1] == L'.')) t[--L] = 0; }
    {
        WCHAR stem[16];
        size_t k;
        for (k = 0; t[i + k] && t[i + k] != L'.' && k < 15; k++) stem[k] = t[i + k];
        stem[k] = 0;
        for (k = 0; k < sizeof(reserved) / sizeof(reserved[0]); k++)
            if (!_wcsicmp(stem, reserved[k])) { _snwprintf(out, cch - 1, L"_%ls", t + i); out[cch - 1] = 0; return; }
    }
    wcsncpy(out, t + i, cch - 1);
    out[cch - 1] = 0;
}

static void set_name(const WCHAR *s, int selectTitle)
{
    g_settingName = 1;
    SetWindowTextW(g_editName, s);
    g_settingName = 0;
    g_nameEdited = 0;
    if (selectTitle) {                   /* pre-select the "Titolo" placeholder (2 = also focus) */
        const WCHAR *t = wcsstr(s, L"Titolo");
        if (t) {
            if (selectTitle == 2) SetFocus(g_editName);
            SendMessageW(g_editName, EM_SETSEL, (WPARAM)(t - s), (LPARAM)(t - s + 6));
        }
    }
}

static void refresh_name(int force, int selectTitle)
{
    WCHAR r[256], cur[256];
    if (!force && g_nameEdited) return;
    render_template(g_template, r, 256);
    GetWindowTextW(g_editName, cur, 256);
    if (force || wcscmp(r, cur) != 0) set_name(r, selectTitle);
}

static int template_is_preset(const WCHAR *t)
{
    int i;
    for (i = 0; i < TPL_PRESETS; i++) if (!wcscmp(t, k_tplPresets[i])) return 1;
    return 0;
}

/* queue the rename of the session that just ended, using the name box content */
static void queue_finalize(void)
{
    WCHAR raw[256], name[256];
    if (g_finalizedSess == g_sessId) return;
    g_finalizedSess = g_sessId;
    GetWindowTextW(g_editName, raw, 256);
    sanitize_name(raw, name, 256);
    if (!finalize_request(name)) log_msg(L"Could not queue final rename (keeping internal names)");
}

/* ---------------- settings dialog ---------------- */
static const int k_splitChoices[5] = { 0, 30, 60, 90, 120 };

static void dlg_fill_quality(HWND d, int selKbps)
{
    HWND cb = GetDlgItem(d, IDC_S_QUALITY);
    BOOL ok = FALSE;
    int mb = (int)GetDlgItemInt(d, IDC_S_MAXMB, &ok, FALSE), i, sel = 0;
    if (!ok || mb < 20) mb = 190;
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < 4; i++) {
        WCHAR t[128];
        int r = k_aacRates[i];
        int h10 = (int)(mb * 1000000.0 / (r * 125.0 * 1.03) / 3600.0 * 10.0);   /* rounded down */
        _snwprintf(t, 127, L"%d kbps  \u00B7  ~%d MB/h  \u00B7  max %d,%d h per file",
                   r, (int)(r * 125.0 * 3600.0 / 1000000.0 + 0.5), h10 / 10, h10 % 10);
        t[127] = 0;
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
        if (r == selKbps) sel = i;
    }
    SendMessageW(cb, CB_SETCURSEL, (WPARAM)sel, 0);
}

static const int k_segChoices[5] = { 1, 2, 5, 10, 15 };

static void dlg_enable(HWND d)
{
    BOOL on = IsDlgButtonChecked(d, IDC_S_COMPRESS) == BST_CHECKED;
    BOOL live = SendDlgItemMessageW(d, IDC_S_MODE, CB_GETCURSEL, 0, 0) == 0;
    EnableWindow(GetDlgItem(d, IDC_S_MODE), on);
    EnableWindow(GetDlgItem(d, IDC_S_QUALITY), on);
    EnableWindow(GetDlgItem(d, IDC_S_CHANNELS), on);
    EnableWindow(GetDlgItem(d, IDC_S_KEEPWAV), on && !live);    /* live mode writes no WAV */
    EnableWindow(GetDlgItem(d, IDC_S_SEG), on && live);
    {
        BOOL sc = IsDlgButtonChecked(d, IDC_S_SILCUT) == BST_CHECKED;
        EnableWindow(GetDlgItem(d, IDC_S_SILMIN), sc);
        EnableWindow(GetDlgItem(d, IDC_S_SILPAD), sc);
        EnableWindow(GetDlgItem(d, IDC_S_SILDB), sc);
    }
}

static INT_PTR CALLBACK settings_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (m) {
    case WM_INITDIALOG: {
        HWND cbc = GetDlgItem(d, IDC_S_CHANNELS), cbs = GetDlgItem(d, IDC_S_SPLIT);
        int i, sel = 0, n = 5;
        CheckDlgButton(d, IDC_S_COMPRESS, g_compress ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_S_KEEPWAV, g_keepWav ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(d, IDC_S_MAXMB, (UINT)g_maxFileMB, FALSE);
        SendDlgItemMessageW(d, IDC_S_MAXMB, EM_LIMITTEXT, 4, 0);
        dlg_fill_quality(d, g_aacKbps);
        SendMessageW(cbc, CB_ADDSTRING, 0, (LPARAM)L"Stereo");
        SendMessageW(cbc, CB_ADDSTRING, 0, (LPARAM)L"Mono");
        SendMessageW(cbc, CB_SETCURSEL, g_mono ? 1 : 0, 0);
        SendDlgItemMessageW(d, IDC_S_MODE, CB_ADDSTRING, 0, (LPARAM)L"durante la registrazione (consigliato)");
        SendDlgItemMessageW(d, IDC_S_MODE, CB_ADDSTRING, 0, (LPARAM)L"dopo la registrazione (WAV temporaneo)");
        SendDlgItemMessageW(d, IDC_S_MODE, CB_SETCURSEL, g_compressLive ? 0 : 1, 0);
        {
            int sg = 2;
            for (i = 0; i < 5; i++) {
                WCHAR t[32];
                _snwprintf(t, 31, L"%d min", k_segChoices[i]); t[31] = 0;
                SendDlgItemMessageW(d, IDC_S_SEG, CB_ADDSTRING, 0, (LPARAM)t);
                if (k_segChoices[i] == g_segMin) sg = i;
            }
            if (g_segMin != k_segChoices[sg]) {
                WCHAR t[32];
                _snwprintf(t, 31, L"%d min", g_segMin); t[31] = 0;
                SendDlgItemMessageW(d, IDC_S_SEG, CB_ADDSTRING, 0, (LPARAM)t);
                sg = 5;
            }
            SendDlgItemMessageW(d, IDC_S_SEG, CB_SETCURSEL, (WPARAM)sg, 0);
        }
        for (i = 0; i < 5; i++) {
            WCHAR t[32];
            if (k_splitChoices[i] == 0) wcscpy(t, L"No (solo limite dimensione)");
            else _snwprintf(t, 31, L"%d min", k_splitChoices[i]);
            t[31] = 0;
            SendMessageW(cbs, CB_ADDSTRING, 0, (LPARAM)t);
            if (k_splitChoices[i] == g_splitMin) sel = i;
        }
        if (g_splitMin && !sel) {         /* custom value from the ini */
            WCHAR t[32];
            _snwprintf(t, 31, L"%d min", g_splitMin); t[31] = 0;
            SendMessageW(cbs, CB_ADDSTRING, 0, (LPARAM)t);
            sel = n;
        }
        SendMessageW(cbs, CB_SETCURSEL, (WPARAM)sel, 0);
        SetDlgItemTextW(d, IDC_S_INFO, L"Le registrazioni pi\u00F9 lunghe del limite vengono divise in parti "
                                        L"(_part2, _part3\u2026). Senza compressione il limite vale per i WAV.");
        SetDlgItemTextW(d, IDC_S_NOTE, g_recording
            ? L"Taglia silenzi: vale subito. Il resto: dalla prossima registrazione."
            : L"Le modifiche valgono dalla prossima registrazione.");
        CheckDlgButton(d, IDC_S_SILCUT, g_silCut ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(d, IDC_S_SILMIN, (UINT)g_silMinSec, FALSE);
        SetDlgItemInt(d, IDC_S_SILPAD, (UINT)g_silPadMs, FALSE);
        SetDlgItemInt(d, IDC_S_SILDB, (UINT)g_silDb, TRUE);
        SendDlgItemMessageW(d, IDC_S_SILMIN, EM_LIMITTEXT, 2, 0);
        SendDlgItemMessageW(d, IDC_S_SILPAD, EM_LIMITTEXT, 4, 0);
        SendDlgItemMessageW(d, IDC_S_SILDB, EM_LIMITTEXT, 3, 0);
        if (!g_mfAvailable) {
            CheckDlgButton(d, IDC_S_COMPRESS, BST_UNCHECKED);
            EnableWindow(GetDlgItem(d, IDC_S_COMPRESS), FALSE);
            SetDlgItemTextW(d, IDC_S_COMPRESS, L"Compressione non disponibile (Media Foundation assente)");
        }
        dlg_enable(d);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_S_COMPRESS: dlg_enable(d); return TRUE;
        case IDC_S_SILCUT: dlg_enable(d); return TRUE;
        case IDC_S_MODE: if (HIWORD(wp) == CBN_SELCHANGE) dlg_enable(d); return TRUE;
        case IDC_S_MAXMB:
            if (HIWORD(wp) == EN_CHANGE) {
                LRESULT q = SendDlgItemMessageW(d, IDC_S_QUALITY, CB_GETCURSEL, 0, 0);
                dlg_fill_quality(d, k_aacRates[q >= 0 && q < 4 ? q : 0]);
            }
            return TRUE;
        case IDOK: {
            BOOL ok = FALSE;
            int mb = (int)GetDlgItemInt(d, IDC_S_MAXMB, &ok, FALSE);
            LRESULT q = SendDlgItemMessageW(d, IDC_S_QUALITY, CB_GETCURSEL, 0, 0);
            LRESULT sp = SendDlgItemMessageW(d, IDC_S_SPLIT, CB_GETCURSEL, 0, 0);
            if (!ok || mb < 20 || mb > 4000) {
                MessageBoxW(d, L"Inserisci una dimensione massima tra 20 e 4000 MB.", APP_TITLE, MB_ICONWARNING);
                SetFocus(GetDlgItem(d, IDC_S_MAXMB));
                return TRUE;
            }
            {
                BOOL okMin = FALSE, okPad = FALSE, okDb = FALSE;
                int sc = IsDlgButtonChecked(d, IDC_S_SILCUT) == BST_CHECKED;
                int smin = (int)GetDlgItemInt(d, IDC_S_SILMIN, &okMin, FALSE);
                int spad = (int)GetDlgItemInt(d, IDC_S_SILPAD, &okPad, FALSE);
                int sdb  = (int)GetDlgItemInt(d, IDC_S_SILDB, &okDb, TRUE);
                if (okDb && sdb > 0) sdb = -sdb;          /* "45" means -45 dB */
                if (sc) {
                    if (!okMin || smin < 1 || smin > 60) {
                        MessageBoxW(d, L"Silenzio minimo: inserisci un valore tra 1 e 60 secondi.", APP_TITLE, MB_ICONWARNING);
                        SetFocus(GetDlgItem(d, IDC_S_SILMIN)); return TRUE;
                    }
                    if (!okPad || spad < 0 || spad > 5000) {
                        MessageBoxW(d, L"Margine: inserisci un valore tra 0 e 5000 ms.", APP_TITLE, MB_ICONWARNING);
                        SetFocus(GetDlgItem(d, IDC_S_SILPAD)); return TRUE;
                    }
                    if (!okDb || sdb < -70 || sdb > -20) {
                        MessageBoxW(d, L"Soglia silenzio: inserisci un valore tra -70 e -20 dB.", APP_TITLE, MB_ICONWARNING);
                        SetFocus(GetDlgItem(d, IDC_S_SILDB)); return TRUE;
                    }
                }
                g_silCut = sc;
                if (okMin && smin >= 1 && smin <= 60) g_silMinSec = smin;
                if (okPad && spad >= 0 && spad <= 5000) g_silPadMs = spad;
                if (okDb && sdb >= -70 && sdb <= -20) g_silDb = sdb;
            }
            if (g_mfAvailable) g_compress = IsDlgButtonChecked(d, IDC_S_COMPRESS) == BST_CHECKED;
            g_keepWav = IsDlgButtonChecked(d, IDC_S_KEEPWAV) == BST_CHECKED;
            g_aacKbps = k_aacRates[q >= 0 && q < 4 ? q : 0];
            g_mono = SendDlgItemMessageW(d, IDC_S_CHANNELS, CB_GETCURSEL, 0, 0) == 1;
            g_maxFileMB = mb;
            if (sp >= 0 && sp < 5) g_splitMin = k_splitChoices[sp];
            g_compressLive = SendDlgItemMessageW(d, IDC_S_MODE, CB_GETCURSEL, 0, 0) != 1;
            {
                LRESULT sg = SendDlgItemMessageW(d, IDC_S_SEG, CB_GETCURSEL, 0, 0);
                if (sg >= 0 && sg < 5) g_segMin = k_segChoices[sg];
            }
            settings_save(NULL);
            if (g_recording) gate_request_update();   /* silence cut applies to the running recording */
            EndDialog(d, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(d, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---------------- tray ---------------- */
static void tray_set_ex(int add, const WCHAR *tip, const WCHAR *balloonTitle, const WCHAR *balloon, DWORD infoFlags)
{
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_icoCurSmall ? g_icoCurSmall : g_icoSmall;
    wcsncpy(nid.szTip, tip ? tip : APP_TITLE, 127);
    if (balloon) {
        nid.uFlags |= NIF_INFO;
        wcsncpy(nid.szInfoTitle, balloonTitle, 63);
        wcsncpy(nid.szInfo, balloon, 255);
        nid.dwInfoFlags = infoFlags;
    }
    if (add && !g_trayAdded) { if (Shell_NotifyIconW(NIM_ADD, &nid)) g_trayAdded = 1; }
    else if (g_trayAdded) Shell_NotifyIconW(NIM_MODIFY, &nid);
}
static void tray_set(int add, const WCHAR *tip, const WCHAR *balloonTitle, const WCHAR *balloon)
{
    tray_set_ex(add, tip, balloonTitle, balloon, NIIF_INFO);
}

static void tray_remove(void)
{
    NOTIFYICONDATAW nid;
    if (!g_trayAdded) return;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = 0;
}

/* ---------------- source picker ---------------- */
typedef struct { DWORD pid; WCHAR exe[64]; WCHAR name[128]; int session, active; } SrcApp;
#define SRC_MAX 40
static SrcApp g_srcApps[SRC_MAX];
static int    g_srcAppN;
static WCHAR  g_srcBtnText[200];
static LONG   g_srcBtnState = -1;
static WCHAR  g_srcMsg[220];          /* non-blocking line after a live source switch (UI thread only) */
static LONG64 g_srcMsgUntil;
static int    g_srcMsgWarn;
static void src_msg(int warn, LONG64 ms, const WCHAR *fmt, const WCHAR *a)
{
    _snwprintf(g_srcMsg, 219, fmt, a); g_srcMsg[219] = 0;
    g_srcMsgWarn = warn; g_srcMsgUntil = now_ms() + ms;
}
/* the source in effect right now: the running session's (or its pending switch), the pending
   session's, else the one chosen for the next recording */
static void src_current(WCHAR *exe, WCHAR *name)
{
    exe[0] = name[0] = 0;
    if (g_recording) {
        EnterCriticalSection(&g_statusLock);
        if (g_srcSwitchReq) { wcscpy(exe, g_reqSrcExe); wcscpy(name, g_reqSrcName); }
        else { wcscpy(exe, g_curSrcExe); wcscpy(name, g_curSrcName); }
        LeaveCriticalSection(&g_statusLock);
    } else if (g_pending) {
        GetPrivateProfileStringW(L"Session", L"SourceExe", L"", exe, 64, g_sessIni);
        GetPrivateProfileStringW(L"Session", L"SourceName", L"", name, 128, g_sessIni);
    } else { wcscpy(exe, g_srcExe); wcscpy(name, g_srcName); }
    if (exe[0] && !name[0]) wcsncpy(name, exe, 127);
    if (!exe[0]) name[0] = 0;
}

typedef HRESULT (WINAPI *PFN_DwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD);
typedef struct { DWORD pids[256]; int n; PFN_DwmGetWindowAttribute dwm; } WinPids;
static BOOL CALLBACK src_win_cb(HWND h, LPARAM lp)
{
    WinPids *w = (WinPids *)lp;
    DWORD pid = 0, cloaked = 0;
    int i;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
    if (GetWindowTextLengthW(h) <= 0) return TRUE;
    if (w->dwm && SUCCEEDED(w->dwm(h, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == GetCurrentProcessId()) return TRUE;
    for (i = 0; i < w->n; i++) if (w->pids[i] == pid) return TRUE;
    if (w->n < 256) w->pids[w->n++] = pid;
    return TRUE;
}
static void src_add(const PInfo *p, int n, int idx, int session, int active)
{
    int r = proc_root_idx(p, n, idx), i;
    if (r < 0 || p[r].pid == GetCurrentProcessId() || proc_hidden(p[r].exe)) return;
    for (i = 0; i < g_srcAppN; i++) if (!_wcsicmp(g_srcApps[i].exe, p[r].exe)) {
        if (session) g_srcApps[i].session = 1;
        if (active) g_srcApps[i].active = 1;
        return;
    }
    if (g_srcAppN >= SRC_MAX) return;
    {
        SrcApp *a = &g_srcApps[g_srcAppN++];
        a->pid = p[r].pid; a->session = session; a->active = active;
        wcsncpy(a->exe, p[r].exe, 63); a->exe[63] = 0;
        proc_friendly(p[r].pid, p[r].exe, a->name, 128);
    }
}
static int src_cmp(const SrcApp *a, const SrcApp *b)
{
    if (a->session != b->session) return b->session - a->session;
    if (a->active != b->active) return b->active - a->active;
    return _wcsicmp(a->name, b->name);
}
/* apps with an audio session (any state but expired) + apps with a visible window */
static void src_enum(void)
{
    PInfo *p = NULL;
    int n = proc_snap(&p), i, j;
    DWORD sp[64]; int sa[64], ns = sess_pids(sp, sa, 64);
    WinPids *w = (WinPids *)calloc(1, sizeof(WinPids));
    g_srcAppN = 0;
    for (i = 0; i < ns; i++) src_add(p, n, pi_find(p, n, sp[i]), 1, sa[i]);
    if (w) {
        HMODULE dm = LoadLibraryW(L"dwmapi.dll");
        w->dwm = dm ? (PFN_DwmGetWindowAttribute)(void (*)(void))GetProcAddress(dm, "DwmGetWindowAttribute") : NULL;
        EnumWindows(src_win_cb, (LPARAM)w);
        for (i = 0; i < w->n; i++) src_add(p, n, pi_find(p, n, w->pids[i]), 0, 0);
        free(w);
    }
    for (i = 1; i < g_srcAppN; i++)        /* insertion sort: playing, then with audio, then by name */
        for (j = i; j > 0 && src_cmp(&g_srcApps[j], &g_srcApps[j - 1]) < 0; j--) {
            SrcApp t = g_srcApps[j]; g_srcApps[j] = g_srcApps[j - 1]; g_srcApps[j - 1] = t;
        }
    free(p);
}
/* the source the window shows: the running/pending session's, else the chosen one */
static void src_label(WCHAR *out, size_t cch)
{
    if (g_recording) {
        EnterCriticalSection(&g_statusLock);
        if (!g_curSrcExe[0]) wcsncpy(out, L"Tutto il sistema", cch - 1);
        else if (g_srcState == 3) _snwprintf(out, cch - 1, L"Tutto il sistema (%ls non supportato)", g_curSrcName);
        else if (g_srcState == 2) _snwprintf(out, cch - 1, L"%ls (in attesa\u2026)", g_curSrcName);
        else wcsncpy(out, g_curSrcName, cch - 1);
        LeaveCriticalSection(&g_statusLock);
    } else if (g_pending) {
        WCHAR e[64] = L"", nm[128] = L"";
        GetPrivateProfileStringW(L"Session", L"SourceExe", L"", e, 64, g_sessIni);
        GetPrivateProfileStringW(L"Session", L"SourceName", L"", nm, 128, g_sessIni);
        _snwprintf(out, cch - 1, L"%ls (sessione)", e[0] ? (nm[0] ? nm : e) : L"Tutto il sistema");
    } else wcsncpy(out, g_srcExe[0] ? g_srcName : L"Tutto il sistema", cch - 1);
    out[cch - 1] = 0;
}
static void src_button_update(void)
{
    WCHAR lab[160], t[200];
    if (!g_btnSrc) return;
    src_label(lab, 160);
    _snwprintf(t, 199, L"%ls  \u25BE", lab); t[199] = 0;
    g_srcBtnState = g_srcState;
    if (wcscmp(t, g_srcBtnText)) {
        HDC dc = GetDC(g_btnSrc);
        HGDIOBJ of = SelectObject(dc, g_fUI);
        SIZE sz = {0, 0};
        int wpx;
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
        SelectObject(dc, of);
        ReleaseDC(g_btnSrc, dc);
        wpx = sz.cx + S(18);
        if (wpx > S(366)) wpx = S(366);
        if (wpx < S(60)) wpx = S(60);
        wcscpy(g_srcBtnText, t);
        SetWindowTextW(g_btnSrc, t);
        MoveWindow(g_btnSrc, S(74), S(258), wpx, S(22), TRUE);
    }
}
static void src_menu(HWND h)
{
    for (;;) {
        HMENU m = CreatePopupMenu(), sub = NULL;
        RECT br;
        int i, cmd, anyS = 0, anyW = 0, chosenListed = 0;
        WCHAR it[200], curE[64], curN[128], newE[64], newN[128];
        src_current(curE, curN);
        src_enum();
        AppendMenuW(m, MF_STRING | (!curE[0] ? MF_CHECKED : 0), IDM_SRC_SYS, L"Tutto il sistema");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        for (i = 0; i < g_srcAppN; i++) {
            int chosen = curE[0] && !_wcsicmp(curE, g_srcApps[i].exe);
            if (chosen) chosenListed = 1;
            if (g_srcApps[i].session) {
                _snwprintf(it, 199, L"%ls%ls", g_srcApps[i].name, g_srcApps[i].active ? L"\t\u266A in riproduzione" : L"");
                it[199] = 0;
                AppendMenuW(m, MF_STRING | (chosen ? MF_CHECKED : 0), IDM_SRC0 + i, it);
                anyS = 1;
            } else {
                if (!sub) sub = CreatePopupMenu();
                AppendMenuW(sub, MF_STRING | (chosen ? MF_CHECKED : 0), IDM_SRC0 + i, g_srcApps[i].name);
                anyW = 1;
            }
        }
        if (!anyS) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(nessuna app sta usando l'audio)");
        if (curE[0] && !chosenListed) {
            _snwprintf(it, 199, L"%ls (non in esecuzione)", curN); it[199] = 0;
            AppendMenuW(m, MF_STRING | MF_CHECKED, IDM_SRC_KEEP, it);
        }
        if (anyW) AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Altre app aperte");
        else if (sub) DestroyMenu(sub);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, IDM_SRC_REFRESH, L"Aggiorna elenco");
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Una singola scheda del browser non si pu\u00F2 separare");
        GetWindowRect(g_btnSrc, &br);
        cmd = (int)TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, br.left, br.bottom, 0, h, NULL);
        DestroyMenu(m);
        if (cmd == IDM_SRC_REFRESH) continue;
        if (g_stopping) return;                        /* state changed while the menu was open */
        if (cmd == IDM_SRC_SYS) { newE[0] = 0; newN[0] = 0; }
        else if (cmd >= IDM_SRC0 && cmd < IDM_SRC0 + g_srcAppN) {
            wcscpy(newE, g_srcApps[cmd - IDM_SRC0].exe);
            wcscpy(newN, g_srcApps[cmd - IDM_SRC0].name);
        } else return;
        {   /* compare with what is in effect now (the menu may have been open across a start/stop) */
            WCHAR e2[64], n2[128];
            src_current(e2, n2);
            if (!_wcsicmp(newE, e2)) { if (!g_recording && !g_pending) { wcscpy(g_srcExe, newE); wcscpy(g_srcName, newN); } return; }
        }
        wcscpy(g_srcExe, newE); wcscpy(g_srcName, newN);   /* also the choice for the next recordings */
        settings_save(NULL);
        if (g_recording) {                     /* live: the capture thread switches, same file */
            log_msg(L"Source change requested while recording: %ls%ls", newE[0] ? newN : L"whole system",
                    g_pauseReq ? L" (paused)" : L"");
            src_switch_request(newE, newN);
            if (newE[0] && !src_find_root(newE))
                src_msg(1, 8000, L"Sorgente cambiata: %ls non \u00E8 in esecuzione, registra silenzio finch\u00E9 non parte", newN);
            else src_msg(0, 5000, L"Sorgente cambiata: %ls", newE[0] ? newN : L"Tutto il sistema");
        } else if (g_pending) {                /* the resumed session will use it */
            sess_put(L"SourceExe", newE[0] ? newE : NULL);
            sess_put(L"SourceName", newE[0] ? newN : NULL);
            log_msg(L"Source of the pending session set to %ls", newE[0] ? newN : L"whole system");
        } else log_msg(L"Source set to %ls", g_srcExe[0] ? g_srcName : L"whole system");
        src_button_update();
        InvalidateRect(h, NULL, FALSE);
        return;
    }
}

/* ---------------- state ---------------- */
static void update_controls(void)
{
    int rec = g_recording != 0, pend = g_pending && !rec;
    if (pend != g_layoutPending) layout();
    SetWindowTextW(g_btnToggle, g_stopping ? L"Salvataggio\u2026" : rec ? L"\u25A0  Ferma e salva"
                   : pend ? L"Riprendi sessione" : L"\u25CF  Avvia registrazione");
    SetWindowTextW(g_btnPause, pend ? L"Salva ora" : (rec && g_pauseReq) ? L"Riprendi" : L"Pausa");
    SetWindowTextW(g_btnDiscard, L"Scarta\u2026");
    EnableWindow(g_btnToggle, !g_stopping && !g_exitWhenIdle);
    EnableWindow(g_btnPause, !g_stopping && !g_exitWhenIdle && (pend || rec));
    EnableWindow(g_btnDiscard, pend && !g_exitWhenIdle);
    EnableWindow(g_btnFolder, !rec && !pend);
    EnableWindow(g_cbTemplate, !pend && !g_stopping);
    EnableWindow(g_btnTpl, !pend && !g_stopping);
    EnableWindow(g_btnSrc, !g_stopping && !g_exitWhenIdle);   /* 1.4.1: also while recording / paused / pending */
    src_button_update();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* app icon with a status badge in the bottom-right corner: red dot = recording, two bars = paused */
static HICON make_badge_icon(int size, int kind, int badgeOnly)
{
    BITMAPINFO bi;
    void *bits = NULL;
    DWORD *px;
    HDC dc;
    HBITMAP col, mask, oldb;
    ICONINFO ii;
    HICON ic;
    int x, y;
    double r = badgeOnly ? size / 2.0 - 1.5 : size * (size <= 20 ? 0.34 : 0.30);
    double cx = badgeOnly ? size / 2.0 : size - r - 1.0, cy = cx;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size; bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    col = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!col) { DeleteDC(dc); return NULL; }
    px = (DWORD *)bits;
    memset(px, 0, (size_t)size * size * 4);
    oldb = (HBITMAP)SelectObject(dc, col);
    if (!badgeOnly) DrawIconEx(dc, 0, 0, g_icoBig, size, size, 0, NULL, DI_NORMAL);
    SelectObject(dc, oldb);
    GdiFlush();
    for (y = 0; y < size; y++) for (x = 0; x < size; x++) {
        double dx = x + 0.5 - cx, dy = y + 0.5 - cy, d = sqrt(dx * dx + dy * dy);
        double a = r + 0.5 - d;                     /* antialiased disc edge */
        double ring = r + 1.5 * size / 32.0 + 0.5 - d; /* white outline */
        DWORD c;
        int R, G, B;
        if (ring <= 0) continue;
        if (a > 0) {
            if (kind == 1 || kind == 3) {  /* dark badge with a bright red (3: gray) dot, readable on the red app icon */
                if (d <= r * 0.58) { if (kind == 1) { R = 255; G = 40; B = 40; } else { R = 160; G = 170; B = 185; } }
                else { R = 35; G = 35; B = 40; }
            }
            else {
                double bw = r * 0.30, gap = r * 0.22, bh = r * 0.62;
                int inBar = fabs(dy) <= bh && ((dx >= -gap - bw && dx <= -gap) || (dx >= gap && dx <= gap + bw));
                if (inBar) { R = 255; G = 255; B = 255; } else { R = 240; G = 150; B = 0; }
            }
            if (a > 1) a = 1;
        } else { R = G = B = 255; a = ring > 1 ? 1 : ring; }
        {
            DWORD o = px[y * size + x];
            double oa = ((o >> 24) & 255) / 255.0, na = a + oa * (1 - a);
            int oR = (o >> 16) & 255, oG = (o >> 8) & 255, oB = o & 255;   /* premultiplied */
            int nR = (int)(R * a + oR * (1 - a)), nG = (int)(G * a + oG * (1 - a)), nB = (int)(B * a + oB * (1 - a));
            (void)na;
            c = ((DWORD)(int)(na * 255) << 24) | ((DWORD)nR << 16) | ((DWORD)nG << 8) | (DWORD)nB;
            px[y * size + x] = c;
        }
    }
    /* DIB is premultiplied; icons want straight alpha */
    for (y = 0; y < size * size; y++) {
        DWORD o = px[y]; int A = (o >> 24) & 255;
        if (A && A < 255) {
            int R = ((o >> 16) & 255) * 255 / A, G = ((o >> 8) & 255) * 255 / A, B = (o & 255) * 255 / A;
            px[y] = ((DWORD)A << 24) | ((DWORD)(R > 255 ? 255 : R) << 16) | ((DWORD)(G > 255 ? 255 : G) << 8) | (DWORD)(B > 255 ? 255 : B);
        }
    }
    mask = CreateBitmap(size, size, 1, 1, NULL);
    ii.fIcon = TRUE; ii.xHotspot = ii.yHotspot = 0; ii.hbmMask = mask; ii.hbmColor = col;
    ic = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(col); DeleteDC(dc);
    return ic;
}

static void apply_state_icon(void)
{
    int st = g_recording ? (g_pauseReq ? 2 : (g_gateOn && g_gateCutting ? 3 : 1)) : 0;
    HICON b, sm;
    if (st == g_iconState) return;
    g_iconState = st;
    if (st && !g_icoRecBig) {
        int bs = GetSystemMetrics(SM_CXICON), ss = GetSystemMetrics(SM_CXSMICON);
        g_icoRecBig = make_badge_icon(bs, 1, 0); g_icoRecSmall = make_badge_icon(ss, 1, 0);
        g_icoPauBig = make_badge_icon(bs, 2, 0); g_icoPauSmall = make_badge_icon(ss, 2, 0);
        g_ovlRec = make_badge_icon(ss, 1, 1); g_ovlPau = make_badge_icon(ss, 2, 1); g_ovlSil = make_badge_icon(ss, 3, 1);
    }
    b  = (st == 1 || st == 3) ? g_icoRecBig : st == 2 ? g_icoPauBig : g_icoBig;
    sm = (st == 1 || st == 3) ? g_icoRecSmall : st == 2 ? g_icoPauSmall : g_icoSmall;
    if (!b) b = g_icoBig;
    if (!sm) sm = g_icoSmall;
    g_icoCurSmall = sm;
    SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)b);
    SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)sm);
    if (g_trayAdded) tray_set(0, g_lastTitle[0] ? g_lastTitle : APP_TITLE, NULL, NULL);
    if (g_taskbar)                       /* badge on the taskbar button: works even for pinned/shortcut icons */
        g_taskbar->lpVtbl->SetOverlayIcon(g_taskbar, g_hwnd, st == 1 ? g_ovlRec : st == 2 ? g_ovlPau : st == 3 ? g_ovlSil : NULL,
                                          st == 1 ? L"Registrazione in corso" : st == 2 ? L"In pausa"
                                          : st == 3 ? L"Registrazione in corso, silenzio tagliato" : L"");
}

static void update_title(void)
{
    WCHAR t[128], hms[32], sx[140] = L"";
    if (g_recording && g_curSrcExe[0] && g_srcState != 3) { _snwprintf(sx, 139, L" \u00B7 %ls", g_curSrcName); sx[139] = 0; }
    if (g_recording && g_pauseReq) {
        WCHAR pd[24];
        LONG64 st = g_pauseStartMs;
        fmt_dur(pd, 24, st ? now_ms() - st : 0);
        fmt_hms(hms, 32, file_ms());
        _snwprintf(t, 127, L"PAUSA %ls \u00B7 REC %ls%ls - " APP_TITLE, pd, hms, sx);
    }
    else if (g_recording && g_gateOn && g_gateCutting) {
        LONG64 ss = atomic_read64(&g_gateRunF) / OUT_RATE;
        fmt_hms(hms, 32, file_ms());
        _snwprintf(t, 127, L"SILENZIO %d:%02d \u00B7 REC %ls%ls - " APP_TITLE, (int)(ss / 60), (int)(ss % 60), hms, sx);
    }
    else if (g_recording) {
        LONG64 sr = atomic_read64(&g_silRunF) * 1000 / OUT_RATE;
        fmt_hms(hms, 32, file_ms());
        if (sr >= 1000 && !g_stopping) {
            WCHAR sd[24];
            fmt_dur(sd, 24, sr);
            _snwprintf(t, 127, L"\u25CF REC %ls \u00B7 silenzio %ls%ls - " APP_TITLE, hms, sd, sx);
        } else _snwprintf(t, 127, L"\u25CF REC %ls%ls - " APP_TITLE, hms, sx);
    }
    else wcscpy(t, APP_TITLE);
    t[127] = 0;
    if (wcscmp(t, g_lastTitle) != 0) {
        wcscpy(g_lastTitle, t);
        SetWindowTextW(g_hwnd, t);
        if (g_recording) tray_set(0, t, NULL, NULL);
    }
    apply_state_icon();
}

static void refresh_free(void) { g_freeNow = free_bytes(g_recording ? g_wav.folder : g_saveFolder); }

static void start_recording(int resume)
{
    WCHAR err[1024];
    if (g_recording || g_stopping || g_exitWhenIdle) return;
    if (!resume && g_pending) resume = 1;       /* a pending session must be resolved first */
    refresh_free();
    if (g_freeNow >= 0 && g_freeNow < LOW_DISK_STOP) {
        MessageBoxW(g_hwnd, L"Spazio su disco insufficiente (meno di 300 MB liberi).\nScegli un'altra cartella.",
                    APP_TITLE, MB_ICONWARNING);
        return;
    }
    if (!resume) {
        wcscpy(g_startSrcExe, g_srcExe); wcscpy(g_startSrcName, g_srcName);
        if (g_srcExe[0] && !src_find_root(g_srcExe)) {
            WCHAR m[600];
            int r;
            _snwprintf(m, 599, L"%ls non \u00E8 in esecuzione.\n\n"
                       L"S\u00EC = registra tutto il sistema (solo questa volta)\n"
                       L"No = avvia comunque: registra silenzio finch\u00E9 %ls non parte, poi la sua audio\n"
                       L"Annulla = non avviare", g_srcName, g_srcName);
            m[599] = 0;
            r = MessageBoxW(g_hwnd, m, APP_TITLE L" - sorgente", MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON1);
            if (r == IDCANCEL) return;
            if (r == IDYES) { g_startSrcExe[0] = 0; g_startSrcName[0] = 0; }
        }
    }
    g_tplUseST = 0;
    GetLocalTime(&g_tplST);
    if (!resume) {
        if (!g_nameEdited) refresh_name(1, 0);       /* fresh date/time in the name */
        if (wcsstr(g_template, L"{n}")) counter_commit();
    }
    if (!rec_start(g_saveFolder, g_splitMin, resume, err, 1024)) {
        MessageBoxW(g_hwnd, err, APP_TITLE L" - errore", MB_ICONERROR);
        update_controls();
        return;
    }
    if (resume) g_pending = 0;
    g_recoverMsg[0] = 0;
    g_tplUseST = !resume || !wcsstr(g_template, L"{n}");
    {
        WCHAR nm[256];
        GetWindowTextW(g_editName, nm, 256);
        live_save_name(nm);
        rename_request(nm);              /* the files carry the chosen name from the start */
        if (g_curLive) sess_put(L"Template", g_template);
    }
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    KillTimer(g_hwnd, TIMER_TRAY);
    tray_set(1, L"\u25CF REC - " APP_TITLE, NULL, NULL);
    g_lastTitle[0] = 0;
    update_controls();
    update_title();
}

static void stop_recording(LONG reason)
{
    if (g_pauseReq && reason != 6) pause_set(0);  /* stopping while paused: nothing more to skip */
    if (!g_recording || g_stopping) return;
    g_stopping = 1;
    g_tplUseST = 0;
    request_stop(reason);
    update_controls();
}

/* UI thread: react to conversion / rename progress and completion */
static void conv_check_idle(void)
{
    WCHAR title[64], text[MAX_PATH * 2 + 64];
    int ok;
    if (g_convPending != 0 || g_recording) { InvalidateRect(g_hwnd, NULL, FALSE); return; }
    if (g_notifyAfterConv) {
        g_notifyAfterConv = 0;
        EnterCriticalSection(&g_convLock);
        ok = g_finValid && g_finCount > 0 && !g_finFailed;
        if (ok) {
            wcscpy(title, L"Registrazione salvata");
            if (g_finCount > 1) _snwprintf(text, MAX_PATH * 2 + 63, L"%ls + altre %d parti (%.1f MB totali)",
                                           base_name(g_finFirst), g_finCount - 1, g_finBytes / 1048576.0);
            else _snwprintf(text, MAX_PATH * 2 + 63, L"%ls (%.1f MB)", base_name(g_finFirst), g_finBytes / 1048576.0);
        } else if (g_finFailed) {
            wcscpy(title, L"Compressione non riuscita");
            _snwprintf(text, MAX_PATH * 2 + 63, L"WAV mantenuto: %ls", base_name(g_finFailWav));
        } else {
            wcscpy(title, L"Registrazione salvata");
            _snwprintf(text, MAX_PATH * 2 + 63, L"%ls", g_wav.folder);
        }
        text[MAX_PATH * 2 + 63] = 0;
        if (g_finFailed && g_finFailWav[0]) wcscpy(g_lastFile, g_finFailWav);
        else if (g_finFirst[0]) wcscpy(g_lastFile, g_finFirst);
        LeaveCriticalSection(&g_convLock);
        if (!g_exitWhenIdle) {
            tray_set_ex(1, APP_TITLE, title, text, ok || !g_finFailed ? NIIF_INFO : NIIF_ERROR);
            SetTimer(g_hwnd, TIMER_TRAY, 15000, NULL);
        }
    }
    if (g_exitWhenIdle) { DestroyWindow(g_hwnd); return; }
    update_controls();
}

static void on_stopped(void)
{
    g_stopping = 0;
    SetThreadExecutionState(ES_CONTINUOUS);
    if (g_stopReason == 6) {                 /* suspended: nothing to finalize or rename */
        g_lastTitle[0] = 0;
        update_title();
        sess_load_pending();
        if (g_pending) set_name(g_pendName, 0), g_nameEdited = 1;
        if (g_closeAfterStop) { DestroyWindow(g_hwnd); return; }
        update_controls();
        return;
    }
    EnterCriticalSection(&g_statusLock);
    wcscpy(g_lastFile, g_curPath);
    LeaveCriticalSection(&g_statusLock);
    queue_finalize();                    /* rename to the name box content, after conversions */
    refresh_name(1, 0);                  /* next session's name */
    g_lastTitle[0] = 0;
    update_title();
    tray_set(1, (g_convEnabled ? L"Compressione in corso - " APP_TITLE : APP_TITLE), NULL, NULL);
    g_notifyAfterConv = 1;
    if (g_closeAfterStop) g_exitWhenIdle = 1;
    update_controls();
    conv_check_idle();                   /* may already be done */
    if (g_closeAfterStop) return;
    if (g_stopReason == 5) {
        WCHAR m[MAX_PATH * 2 + 200];
        _snwprintf(m, MAX_PATH * 2 + 199, L"Spazio su disco quasi esaurito (meno di 300 MB).\n"
                   L"La registrazione \u00E8 stata fermata e salvata in:\n\n%ls", g_wav.folder);
        m[MAX_PATH * 2 + 199] = 0;
        MessageBoxW(g_hwnd, m, APP_TITLE, MB_ICONWARNING);
    } else if (g_stopReason == 4 || g_writeError) {
        WCHAR m[MAX_PATH * 2 + 200];
        _snwprintf(m, MAX_PATH * 2 + 199, L"Errore di scrittura su disco: la registrazione \u00E8 stata interrotta.\n"
                   L"Quanto registrato fino a quel punto \u00E8 salvato in:\n\n%ls\n\nDettagli in:\n%ls", g_wav.folder, g_logPath);
        m[MAX_PATH * 2 + 199] = 0;
        MessageBoxW(g_hwnd, m, APP_TITLE, MB_ICONERROR);
    }
}

static void pick_folder(void)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *it = NULL;
    DWORD opt = 0;
    HRESULT hr = CoCreateInstance(&MY_CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &MY_IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr)) { WCHAR b[640]; MessageBoxW(g_hwnd, hr_str(hr, b, 640), APP_TITLE, MB_ICONERROR); return; }
    if (SUCCEEDED(IFileOpenDialog_GetOptions(dlg, &opt)))
        IFileOpenDialog_SetOptions(dlg, opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(dlg, L"Scegli la cartella delle registrazioni");
    SHCreateDirectoryExW(NULL, g_saveFolder, NULL);
    if (SUCCEEDED(SHCreateItemFromParsingName(g_saveFolder, NULL, &MY_IID_IShellItem, (void **)&it))) {
        IFileOpenDialog_SetFolder(dlg, it);
        IShellItem_Release(it); it = NULL;
    }
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, g_hwnd)) && SUCCEEDED(IFileOpenDialog_GetResult(dlg, &it))) {
        PWSTR p = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p)) && p) {
            if (wcslen(p) < MAX_PATH - 64) {
                wcscpy(g_saveFolder, p);
                settings_save(NULL);
                refresh_free();
            } else MessageBoxW(g_hwnd, L"Percorso troppo lungo.", APP_TITLE, MB_ICONWARNING);
            CoTaskMemFree(p);
        }
        IShellItem_Release(it);
    }
    IFileOpenDialog_Release(dlg);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void open_folder(void)
{
    const WCHAR *folder = g_recording ? g_wav.folder : g_saveFolder;
    if (!g_recording && g_lastFile[0] && GetFileAttributesW(g_lastFile) != INVALID_FILE_ATTRIBUTES) {
        WCHAR args[MAX_PATH * 2 + 16];
        _snwprintf(args, MAX_PATH * 2 + 15, L"/select,\"%ls\"", g_lastFile);
        args[MAX_PATH * 2 + 15] = 0;
        ShellExecuteW(g_hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        return;
    }
    SHCreateDirectoryExW(NULL, folder, NULL);
    ShellExecuteW(g_hwnd, L"open", L"explorer.exe", folder, NULL, SW_SHOWNORMAL);
}

/* ---------------- painting ---------------- */
static void draw_text(HDC dc, HFONT f, COLORREF c, int l, int t, int r, int b, const WCHAR *s, UINT fl)
{
    RECT rc;
    rc.left = S(l); rc.top = S(t); rc.right = S(r); rc.bottom = S(b);
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &rc, fl | DT_SINGLELINE | DT_NOPREFIX);
}

static void fill_rect(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc; HBRUSH br = CreateSolidBrush(c);
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

#define SIL_HOLD_MS     150          /* gray silence band: hold after the run ends */
#define SIL_LAST_MIN_MS 300          /* "Ultimo silenzio": ignore shorter dips */
static void paint(HDC dc, const RECT *cl)
{
    const COLORREF bg = RGB(255, 255, 255), fg = RGB(32, 32, 32), gray = RGB(110, 110, 110);
    const COLORREF red = RGB(220, 38, 38), orange = RGB(217, 119, 6), green = RGB(21, 128, 61);
    WCHAR b1[512], b2[64], b3[64], dev[256], fmt[128];
    int rec = g_recording != 0;
    int paused = rec && g_pauseReq, pend = g_pending && !rec;
    LONG64 el = pend ? g_pendMs : file_ms();            /* big timer = length of the audio in the file */
    LONG64 tl = elapsed_ms();                           /* timeline (real time minus pauses): drift, MB/h */
    int lowDisk = g_freeNow >= 0 && g_freeNow < LOW_DISK_WARN;
    int nextConv = rec ? (g_convEnabled || g_curLive) : (pend || (g_compress && g_mfAvailable));
    const COLORREF blue = RGB(37, 99, 235);
    int silent = rec && !paused && !g_stopping && g_gateOn && g_gateCutting;     /* silence being cut */
    int cutInfo = rec;                                                           /* Tagliati / Pause totals */
    LONG64 silMs = silent ? atomic_read64(&g_gateRunF) * 1000 / OUT_RATE : 0;
    /* metered silence (always on, also with the cut off): shown as soon as the level is below SilenceDb.
       When a run ends the band holds its last value for SIL_HOLD_MS, so brief pops don't strobe it. */
    int meterOn = rec && !paused && !g_stopping && !silent;
    LONG64 quietMs = meterOn ? atomic_read64(&g_silRunF) * 1000 / OUT_RATE : 0;
    LONG64 prevEnd = atomic_read64(&g_silPrevEndMs), prevMs = atomic_read64(&g_silPrevF) * 1000 / OUT_RATE;
    int quiet = quietMs > 0;
    if (meterOn && !quiet && prevEnd && now_ms() - prevEnd < SIL_HOLD_MS && prevMs > 0) { quiet = 1; quietMs = prevMs; }
    LONG64 pauseNowMs = (paused && g_pauseStartMs) ? now_ms() - g_pauseStartMs : 0;

    fill_rect(dc, cl->left, cl->top, cl->right, cl->bottom, bg);
    SetBkMode(dc, TRANSPARENT);

    /* recording dot (blinks while recording) */
    {
        int on = rec && ((now_ms() / 600) % 2 == 0 || g_stopping);
        HBRUSH br = CreateSolidBrush(paused ? RGB(150, 150, 150) : silent ? blue : rec ? (on ? red : RGB(250, 200, 200)) : RGB(200, 200, 200));
        HPEN pen = (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
        Ellipse(dc, S(22), S(22), S(42), S(42));
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(br);
    }
    fmt_hms(b1, 64, el);
    draw_text(dc, g_fBig, (rec && !paused) ? fg : gray, 52, 6, 290, 58, b1, DT_LEFT | DT_VCENTER);
    {
        WCHAR lab[48];
        if (silent) { WCHAR sd[24]; fmt_sil(sd, 24, silMs); _snwprintf(lab, 47, L"SILENZIO %ls", sd); lab[47] = 0; }
        else if (paused && !g_stopping) { WCHAR pd[24]; fmt_dur(pd, 24, pauseNowMs); _snwprintf(lab, 47, L"PAUSA %ls", pd); lab[47] = 0; }
        else wcscpy(lab, g_stopping ? L"SALVATAGGIO\u2026" : paused ? L"PAUSA" : rec ? L"REC" : pend ? L"IN SOSPESO"
                         : g_convPending ? L"COMPRESSIONE\u2026" : (g_sessEndMs ? L"SALVATO" : L"PRONTO"));
        draw_text(dc, g_fBold, paused || pend ? orange : silent ? blue : rec ? red : gray, 280, 6, 440, 58,
                  lab, DT_RIGHT | DT_VCENTER);
    }

    /* level meter */
    draw_text(dc, g_fSmall, gray, 20, 64, 76, 82, L"Livello", DT_LEFT | DT_VCENTER);
    {
        /* scale -70..0 dBFS (the whole SilenceDb range); bar = current 10 ms RMS level */
        int x0 = S(78), x1 = S(384), y0 = S(66), y1 = S(75);
        int live = rec && !paused;
        double db = live ? g_meterDb : -120.0;
        double frac = (db + 70.0) / 70.0, tf = (g_silDb + 70.0) / 70.0;
        int below = db < (double)g_silDb;
        COLORREF c = below ? RGB(148, 163, 184) : db > -3.0 ? red : db > -12.0 ? RGB(234, 179, 8) : RGB(34, 160, 80);
        int tx;
        WCHAR tl[16];
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        if (tf < 0) tf = 0;
        if (tf > 1) tf = 1;
        fill_rect(dc, x0, y0, x1, y1, RGB(232, 232, 232));
        tx = x0 + (int)((x1 - x0) * tf);
        fill_rect(dc, x0, y0, tx, y1, RGB(214, 219, 227));          /* "silence" zone, slightly darker */
        if (frac > 0) fill_rect(dc, x0, y0, x0 + (int)((x1 - x0) * frac), y1, c);
        fill_rect(dc, tx - S(1), y0 - S(3), tx + S(1), y1 + S(2), RGB(30, 41, 59));   /* threshold mark */
        _snwprintf(tl, 15, L"%d", g_silDb); tl[15] = 0;
        {   /* label under the mark (draw_text takes 96-dpi units) */
            int lx = MulDiv(tx, 96, (int)g_dpi);
            draw_text(dc, g_fSmall, RGB(71, 85, 105), lx - 20, 76, lx + 20, 90, tl, DT_CENTER | DT_TOP);
        }
        if (live && db > -70.0) _snwprintf(b1, 63, L"%.0f dB", db);
        else if (live) wcscpy(b1, L"< -70 dB");
        else wcscpy(b1, L"\u2013");
        b1[63] = 0;
        draw_text(dc, g_fSmall, live && below ? RGB(100, 116, 139) : gray, 388, 64, 440, 80, b1, DT_RIGHT | DT_VCENTER);
    }

    /* status line: device | MB | free */
    EnterCriticalSection(&g_statusLock);
    wcscpy(dev, g_devName); wcscpy(fmt, g_fmtDesc);
    LeaveCriticalSection(&g_statusLock);
    fmt_size(b3, 64, g_freeNow);
    if (rec && g_curLive) {
        LONG64 runMs = tl - g_resumeBaseMs;
        double mbh = runMs > 5000 ? (double)(g_bytesWritten - (LONG64)g_lv.closedBytesAtStart) / 1e6 * 3600000.0 / runMs
                                  : g_curKbps * 125.0 * 3600.0 / 1e6;
        _snwprintf(b1, 511, L"Dimensione: ~%.1f MB (compressi)  \u00B7  ~%.0f MB/h  \u00B7  parte %d  \u00B7  liberi: %ls%ls",
                   g_bytesWritten / 1e6, mbh, g_wav.part, b3, lowDisk ? L"  (spazio basso!)" : L"");
    } else if (rec) {
        fmt_size(b2, 64, g_bytesWritten);
        _snwprintf(b1, 511, L"%ls  \u00B7  %ls  \u00B7  liberi: %ls%ls",
                   g_haveDevice ? dev : L"Nessun dispositivo, riprovo\u2026", b2, b3,
                   lowDisk ? L"  (spazio basso!)" : L"");
    } else {
        if (nextConv) _snwprintf(b2, 63, L"AAC %d kbps %ls (%ls)", g_aacKbps, g_mono ? L"mono" : L"stereo",
                                 g_compressLive ? L"durante" : L"dopo");
        else wcscpy(b2, L"WAV");
        b2[63] = 0;
        _snwprintf(b1, 511, L"Pronto  \u00B7  %ls, max %d MB/file  \u00B7  liberi: %ls%ls", b2, g_maxFileMB, b3,
                   lowDisk ? L"  (spazio basso!)" : L"");
    }
    b1[511] = 0;
    draw_text(dc, g_fUI, lowDisk ? orange : fg, 20, 90, 440, 108, b1, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    /* info line */
    b1[0] = 0;
    if (g_convPending) {
        WCHAR cur[MAX_PATH];
        EnterCriticalSection(&g_convLock);
        wcscpy(cur, g_convCurName);
        LeaveCriticalSection(&g_convLock);
        if (cur[0])
            _snwprintf(b1, 511, L"%ls%ls\u2026 %ld%%  \u00B7  %ls", g_exitWhenIdle ? L"Chiusura al termine. " : L"",
                       rec ? L"Compressione parte precedente" : L"Compressione", (long)g_convPct, cur);
        else
            _snwprintf(b1, 511, L"%lsSalvataggio\u2026", g_exitWhenIdle ? L"Chiusura al termine. " : L"");
        b1[511] = 0;
        draw_text(dc, g_fSmall, fg, 20, 108, 440, 126, b1, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        b1[0] = 0;
    } else if (paused) {
        WCHAR pd[24], pt[24];
        fmt_dur(pd, 24, pauseNowMs); fmt_dur(pt, 24, paused_total_ms());
        if (now_ms() < g_srcMsgUntil)
            _snwprintf(b1, 511, L"%ls  \u00B7  In pausa da %ls  \u00B7  Pause totali %ls", g_srcMsg, pd, pt);
        else
        _snwprintf(b1, 511, L"In pausa da %ls  \u00B7  Pause totali %ls  \u00B7  non registra (Ctrl+Alt+P riprende)",
                   pd, pt);
        b1[511] = 0;
        draw_text(dc, g_fSmall, orange, 20, 108, 440, 126, b1, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        b1[0] = 0;
    } else if (rec) {
        double drift = (double)g_framesOut / OUT_RATE - (tl - g_resumeBaseMs) / 1000.0;
        WCHAR cut[128] = L"";
        WCHAR last[64] = L"";
        if (cutInfo) {                     /* left out of the file (the big timer is the file length) */
            LONG64 cms = cut_total_ms(), pms = paused_total_ms();
            WCHAR d[24];
            int k = 0;
            if (cms >= 1000) { fmt_dur(d, 24, cms); k += _snwprintf(cut + k, 127 - k, L"Tagliati %ls  \u00B7  ", d); }
            if (pms >= 1000 && k >= 0 && k < 100) { fmt_dur(d, 24, pms); _snwprintf(cut + k, 127 - k, L"Pause %ls  \u00B7  ", d); }
            cut[127] = 0;
        }
        {                                  /* length of the silence that just ended, for a few seconds */
            if (prevEnd && !silent && !quiet && now_ms() - prevEnd < 6000 && prevMs >= SIL_LAST_MIN_MS) {
                WCHAR d[24];
                fmt_sil(d, 24, prevMs);
                _snwprintf(last, 63, L"Ultimo silenzio %ls  \u00B7  ", d); last[63] = 0;
            }
        }
        if (g_curLive)
            _snwprintf(b1, 511, L"%ls%ls%ls  \u00B7  riconnessioni %ld  \u00B7  deriva %+.2f s", cut, last,
                       g_haveDevice ? dev : L"Nessun dispositivo, riprovo\u2026", (long)g_reconnects, drift);
        else
            _snwprintf(b1, 511, L"%ls%ls%ls  \u00B7  parte %d  \u00B7  riconnessioni %ld  \u00B7  deriva %+.2f s", cut, last,
                       (g_haveDevice && fmt[0]) ? fmt : L"in attesa del dispositivo", g_wav.part, (long)g_reconnects, drift);
        if (g_curSrcExe[0] && g_srcState == 2) {   /* the chosen app is not running: warn (silence is recorded) */
            WCHAR bn[220];
            int y0 = S(108), y1 = S(126);
            _snwprintf(bn, 219, g_srcEverOk ? L"App chiusa: in attesa di %ls\u2026 (registra silenzio)"
                                            : now_ms() < g_srcMsgUntil ? L"Sorgente cambiata: %ls non \u00E8 in esecuzione, in attesa\u2026 (registra silenzio)"
                                            : L"%ls non \u00E8 in esecuzione: in attesa\u2026 (registra silenzio)", g_curSrcName);
            bn[219] = 0;
            fill_rect(dc, S(16), y0, S(444), y1, RGB(255, 237, 213));
            fill_rect(dc, S(16), y0, S(19), y1, orange);
            draw_text(dc, g_fSmall, RGB(154, 52, 18), 24, 108, 440, 126, bn, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            b1[0] = 0;
        } else if (now_ms() < g_srcMsgUntil) {   /* "Sorgente cambiata: ..." for a few seconds */
            int y0 = S(108), y1 = S(126);
            fill_rect(dc, S(16), y0, S(444), y1, g_srcMsgWarn ? RGB(255, 237, 213) : RGB(220, 252, 231));
            fill_rect(dc, S(16), y0, S(19), y1, g_srcMsgWarn ? orange : green);
            draw_text(dc, g_fSmall, g_srcMsgWarn ? RGB(154, 52, 18) : RGB(22, 101, 52), 24, 108, 440, 126, g_srcMsg,
                      DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            b1[0] = 0;
        } else if (silent) {                      /* clear blue banner, distinct from the orange manual pause */
            WCHAR bn[200];
            size_t cl2 = wcslen(cut);
            int y0 = S(108), y1 = S(126);
            if (cl2 > 5) cut[cl2 - 5] = 0;           /* drop the trailing separator */
            WCHAR sd[24];
            fmt_sil(sd, 24, silMs);
            _snwprintf(bn, 199, L"Silenzio \u00B7 %ls  \u2014  non salvato%ls%ls", sd,
                       cut[0] ? L"  \u00B7  " : L"", cut);
            bn[199] = 0;
            fill_rect(dc, S(16), y0, S(444), y1, RGB(219, 234, 254));
            fill_rect(dc, S(16), y0, S(19), y1, blue);
            draw_text(dc, g_fSmall, RGB(30, 64, 175), 24, 108, 440, 126, bn, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            b1[0] = 0;
        } else if (quiet) {                /* below the threshold, nothing cut: neutral gray band */
            WCHAR bn[200], d[24];
            size_t cl2 = wcslen(cut);
            int y0 = S(108), y1 = S(126);
            if (cl2 > 5) cut[cl2 - 5] = 0;
            fmt_sil(d, 24, quietMs);
            _snwprintf(bn, 199, L"Silenzio \u00B7 %ls%ls%ls", d, cut[0] ? L"  \u00B7  " : L"", cut);
            bn[199] = 0;
            fill_rect(dc, S(16), y0, S(444), y1, RGB(241, 245, 249));
            fill_rect(dc, S(16), y0, S(19), y1, RGB(100, 116, 139));
            draw_text(dc, g_fSmall, RGB(51, 65, 85), 24, 108, 440, 126, bn, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            b1[0] = 0;
        }
    } else if (pend) {
        WCHAR hms[32];
        fmt_hms(hms, 32, g_pendMs);
        _snwprintf(b1, 511, L"%ls: %ls (%ls)", g_pendCrashed ? L"Registrazione interrotta, in sospeso" : L"Sessione in sospeso",
                   g_pendName[0] ? g_pendName : g_pendBase, hms);
        b1[511] = 0;
        draw_text(dc, g_fBold, orange, 20, 108, 440, 126, b1, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        b1[0] = 0;
    } else if (g_finValid) {
        EnterCriticalSection(&g_convLock);
        if (g_finFailed)
            _snwprintf(b1, 511, L"Compressione fallita, WAV mantenuto: %ls (%ls)", base_name(g_finFailWav), g_convLastErr);
        else if (g_finCount > 1)
            _snwprintf(b1, 511, L"Salvato: %ls + %d parti (%.1f MB totali)", base_name(g_finFirst), g_finCount - 1,
                       g_finBytes / 1048576.0);
        else if (g_finCount == 1)
            _snwprintf(b1, 511, L"Salvato: %ls (%.1f MB)", base_name(g_finFirst), g_finBytes / 1048576.0);
        else
            wcscpy(b1, L"Nessun audio salvato.");
        b1[511] = 0;
        LeaveCriticalSection(&g_convLock);
        draw_text(dc, g_fSmall, g_finFailed ? red : green, 20, 108, 440, 126, b1, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        b1[0] = 0;
    } else if (g_recoverMsg[0]) {
        _snwprintf(b1, 511, L"Recuperata registrazione interrotta: %ls", base_name(g_recoverMsg));
        b1[511] = 0;
        draw_text(dc, g_fSmall, green, 20, 108, 440, 126, b1, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        b1[0] = 0;
    } else {
        if (g_srcExe[0]) _snwprintf(b1, 511, L"Registra solo l'audio di %ls (anche i suoi sottoprocessi). Il microfono no.", g_srcName);
        else wcscpy(b1, L"Registra l'audio di sistema (browser, chiamate). Il microfono non viene registrato.");
        b1[511] = 0;
    }
    if (b1[0]) draw_text(dc, g_fSmall, gray, 20, 108, 440, 126, b1, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    /* name rows */
    {
    int ny = g_tplCustomShown ? 166 : 148;
    if (g_tplCustomShown) draw_text(dc, g_fSmall, gray, 20, 134, 112, 160, L"Modello:", DT_LEFT | DT_VCENTER);
    draw_text(dc, g_fUI, fg, 20, ny, 112, ny + 25, L"Nome file:", DT_LEFT | DT_VCENTER);
    draw_text(dc, g_fUI, gray, 402, ny, 440, ny + 25, nextConv ? L".m4a" : L".wav", DT_LEFT | DT_VCENTER);
    }

    /* source (button drawn by the control) + save folder */
    draw_text(dc, g_fSmall, gray, 20, 258, 74, 280, L"Sorgente:", DT_LEFT | DT_VCENTER);
    draw_text(dc, g_fSmall, gray, 20, 284, 74, 304, L"Salva in:", DT_LEFT | DT_VCENTER);
    draw_text(dc, g_fUI, fg, 74, 284, 440, 304, rec ? g_wav.folder : g_saveFolder, DT_LEFT | DT_VCENTER | DT_PATH_ELLIPSIS);

    /* hints */
    draw_text(dc, g_fSmall, gray, 20, 344, 440, 364,
              g_tplCustomShown ? L"Segnaposto: {data} {ora} {giorno} {titolo} {n}"
              : g_hotkeyOk ? L"Ctrl+Alt+R avvia/ferma  \u00B7  Ctrl+Alt+P pausa"
                         : L"Ctrl+Alt+R non disponibile",
              DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}

static void on_paint(HWND h)
{
    PAINTSTRUCT ps;
    RECT cl;
    HDC dc = BeginPaint(h, &ps), mem;
    HBITMAP bmp;
    HGDIOBJ old, oldFont;
    GetClientRect(h, &cl);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, cl.right, cl.bottom);
    old = SelectObject(mem, bmp);
    oldFont = SelectObject(mem, g_fUI);
    paint(mem, &cl);
    BitBlt(dc, 0, 0, cl.right, cl.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

static void fill_template_combo(void)
{
    int i, sel = -1;
    for (i = 0; i < TPL_PRESETS; i++) {
        SendMessageW(g_cbTemplate, CB_ADDSTRING, 0, (LPARAM)k_tplPresets[i]);
        if (!wcscmp(g_template, k_tplPresets[i])) sel = i;
    }
    SendMessageW(g_cbTemplate, CB_ADDSTRING, 0, (LPARAM)L"Personalizzato\u2026");
    if (sel >= 0) SendMessageW(g_cbTemplate, CB_SETCURSEL, (WPARAM)sel, 0);
    else SetWindowTextW(g_cbTemplate, g_template);
}

static void template_changed(int fromList)
{
    WCHAR t[256];
    GetWindowTextW(g_cbTemplate, t, 256);
    if (!t[0]) return;
    wcscpy(g_template, t);
    if (!template_is_preset(t)) wcscpy(g_templateCustom, t);
    settings_save(NULL);
    refresh_name(1, fromList && wcsstr(t, L"{titolo}") != NULL);
    if (g_recording) {                   /* rename the running recording too */
        WCHAR nm[256];
        GetWindowTextW(g_editName, nm, 256);
        live_save_name(nm);
        KillTimer(g_hwnd, TIMER_RENAME);
        rename_request(nm);              /* picking a template renames the files at once */
    }
}

/* close while recording: 1 = suspend (resume later), 2 = stop and save, 0 = cancel */
typedef HRESULT (WINAPI *PFN_TaskDialogIndirect)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
static int ask_close(HWND h)
{
    PFN_TaskDialogIndirect td = (PFN_TaskDialogIndirect)(void (*)(void))GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "TaskDialogIndirect");
    if (!g_curLive) {            /* WAV mode (compression "dopo"): no resumable session */
        return MessageBoxW(h, L"La registrazione \u00E8 in corso.\n\nFermare e salvare?", APP_TITLE,
                           MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) == IDYES ? 2 : 0;
    }
    if (td) {
        TASKDIALOG_BUTTON btn[2];
        TASKDIALOGCONFIG cfg;
        int r = 0;
        btn[0].nButtonID = 100; btn[0].pszButtonText = L"Chiudi e riprendi dopo\nLa registrazione resta in sospeso e continua nello stesso file alla prossima apertura.";
        btn[1].nButtonID = 101; btn[1].pszButtonText = L"Ferma e salva\nTermina la registrazione e salva il file .m4a.";
        memset(&cfg, 0, sizeof(cfg));
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = h;
        cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        cfg.pszWindowTitle = APP_TITLE;
        cfg.pszMainInstruction = L"La registrazione \u00E8 in corso";
        cfg.pszContent = L"Cosa vuoi fare?";
        cfg.cButtons = 2;
        cfg.pButtons = btn;
        cfg.nDefaultButton = 100;
        if (SUCCEEDED(td(&cfg, &r, NULL, NULL))) return r == 100 ? 1 : r == 101 ? 2 : 0;
    }
    {
        int r = MessageBoxW(h, L"La registrazione \u00E8 in corso.\n\nS\u00EC = Chiudi e riprendi dopo\nNo = Ferma e salva\nAnnulla = continua a registrare",
                            APP_TITLE, MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);
        return r == IDYES ? 1 : r == IDNO ? 2 : 0;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == g_msgTbButton && g_msgTbButton) {   /* taskbar button exists: (re)attach the overlay badge */
        if (!g_taskbar && FAILED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&g_taskbar)))
            g_taskbar = NULL;
        if (g_taskbar && FAILED(g_taskbar->lpVtbl->HrInit(g_taskbar))) { g_taskbar->lpVtbl->Release(g_taskbar); g_taskbar = NULL; }
        g_iconState = -1;
        apply_state_icon();
        return 0;
    }
    if (m == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        if (g_trayAdded) { g_trayAdded = 0; tray_set(1, g_lastTitle[0] ? g_lastTitle : APP_TITLE, NULL, NULL); }
        return 0;
    }
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        g_dpi = window_dpi(h);
        fonts_create();
        g_cbTemplate = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
                                       0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_TEMPLATE, g_hinst, NULL);
        g_editName  = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_NAME, g_hinst, NULL);
        g_btnToggle = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_TOGGLE, g_hinst, NULL);
        g_btnFolder = CreateWindowExW(0, L"BUTTON", L"Cartella\u2026", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_FOLDER, g_hinst, NULL);
        g_btnOpen   = CreateWindowExW(0, L"BUTTON", L"Apri cartella", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_OPEN, g_hinst, NULL);
        g_btnSettings = CreateWindowExW(0, L"BUTTON", L"Impostazioni\u2026", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_SETTINGS, g_hinst, NULL);
        g_btnPause  = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_PAUSE, g_hinst, NULL);
        g_btnDiscard = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_DISCARD, g_hinst, NULL);
        SendMessageW(g_editName, EM_LIMITTEXT, 200, 0);
        g_btnTpl    = CreateWindowExW(0, L"BUTTON", L"\u25BE", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_TPLBTN, g_hinst, NULL);
        g_btnSrc    = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_LEFT,
                                      S(74), S(258), S(160), S(22), h, (HMENU)(INT_PTR)IDC_SRCBTN, g_hinst, NULL);
        g_tplCustomShown = !template_is_preset(g_template);
        SendMessageW(g_cbTemplate, CB_LIMITTEXT, 200, 0);
        fill_template_combo();
        set_window_size(h);
        layout();
        refresh_name(1, 0);
        if (g_pending) { set_name(g_pendName, 0); g_nameEdited = 1; }
        g_hotkeyPOk = RegisterHotKey(h, HOTKEY_PAUSE, MOD_CONTROL | MOD_ALT | 0x4000, 'P');
        if (!g_hotkeyPOk) g_hotkeyPOk = RegisterHotKey(h, HOTKEY_PAUSE, MOD_CONTROL | MOD_ALT, 'P');
        g_hotkeyOk = RegisterHotKey(h, HOTKEY_ID, MOD_CONTROL | MOD_ALT | 0x4000 /* MOD_NOREPEAT */, 'R');
        if (!g_hotkeyOk) g_hotkeyOk = RegisterHotKey(h, HOTKEY_ID, MOD_CONTROL | MOD_ALT, 'R');
        SetTimer(h, TIMER_UI, 100, NULL);
        SetTimer(h, TIMER_DISK, 2000, NULL);
        refresh_free();
        update_controls();
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        g_dpi = HIWORD(wp);
        fonts_create();
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        set_window_size(h);
        layout();
        g_srcBtnText[0] = 0;
        src_button_update();
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TOGGLE: if (g_recording) stop_recording(1); else start_recording(g_pending); return 0;
        case IDC_PAUSE:
            if (g_recording) { pause_set(!g_pauseReq); update_controls(); update_title(); }
            else if (g_pending) {
                GetWindowTextW(g_editName, g_pendName, 256);      /* the name box wins */
                sess_save_now();
                g_notifyAfterConv = 1;
                g_sessEndMs = 0;
                refresh_name(1, 0);
                conv_check_idle();
            }
            return 0;
        case IDC_DISCARD:
            if (g_pending && MessageBoxW(h, L"Scartare la registrazione in sospeso?\n\nI file audio vanno nel Cestino: se cambi idea puoi ripristinarli da l\u00EC.",
                                         APP_TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                sess_discard();
                refresh_name(1, 0);
                update_controls();
            }
            return 0;
        case IDC_FOLDER: if (!g_recording) pick_folder(); return 0;
        case IDC_SRCBTN: if (!g_stopping && !g_exitWhenIdle) src_menu(h); return 0;
        case IDC_OPEN:   open_folder(); return 0;
        case IDC_SETTINGS:
            DialogBoxParamW(g_hinst, MAKEINTRESOURCEW(IDD_SETTINGS), h, settings_proc, 0);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        case IDC_NAME:
            if (HIWORD(wp) == EN_CHANGE && !g_settingName) {
                g_nameEdited = 1;
                if (g_recording) {                   /* keep the crash/suspend copy of the name current */
                    WCHAR nm[256];
                    GetWindowTextW(g_editName, nm, 256);
                    live_save_name(nm);
                    SetTimer(h, TIMER_RENAME, RENAME_DEBOUNCE_MS, NULL);   /* rename the files when typing pauses */
                }
            }
            return 0;
        case IDC_TPLBTN: {           /* small menu of name templates */
            HMENU m = CreatePopupMenu();
            RECT br; int i, cmd;
            for (i = 0; i < TPL_PRESETS; i++)
                AppendMenuW(m, MF_STRING | (!wcscmp(g_template, k_tplPresets[i]) ? MF_CHECKED : 0), IDM_TPL0 + i, k_tplPresets[i]);
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING | (!template_is_preset(g_template) ? MF_CHECKED : 0), IDM_TPL0 + TPL_PRESETS, L"Personalizzato\u2026");
            GetWindowRect(g_btnTpl, &br);
            cmd = (int)TrackPopupMenu(m, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, br.right, br.bottom, 0, h, NULL);
            DestroyMenu(m);
            if (cmd >= IDM_TPL0 && cmd <= IDM_TPL0 + TPL_PRESETS) {
                int idx = cmd - IDM_TPL0;
                int show = (idx == TPL_PRESETS);
                if (show != g_tplCustomShown) { g_tplCustomShown = show; layout(); InvalidateRect(h, NULL, TRUE); }
                SendMessageW(g_cbTemplate, CB_SETCURSEL, (WPARAM)idx, 0);
                PostMessageW(h, WM_APP_TPL, (WPARAM)idx, 0);
                if (idx < TPL_PRESETS) PostMessageW(h, WM_APP_TPL_FOCUS, 0, 0);
            }
            return 0;
        }
        case IDC_TEMPLATE:
            if (HIWORD(wp) == CBN_SELCHANGE) PostMessageW(h, WM_APP_TPL, (WPARAM)SendMessageW(g_cbTemplate, CB_GETCURSEL, 0, 0), 0);
            else if (HIWORD(wp) == CBN_EDITCHANGE) template_changed(0);
            else if (HIWORD(wp) == CBN_CLOSEUP) PostMessageW(h, WM_APP_TPL_FOCUS, 0, 0);
            return 0;
        }
        break;                                   /* IDOK/IDCANCEL from Enter/Esc are ignored */
    case WM_APP_TPL:
        if ((int)wp == TPL_PRESETS) {            /* "Personalizzato…": edit the custom pattern in place */
            SetWindowTextW(g_cbTemplate, g_templateCustom[0] ? g_templateCustom : L"{titolo} {data} {ora}");
            SetFocus(g_cbTemplate);
            SendMessageW(g_cbTemplate, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
            template_changed(0);
        } else if ((int)wp >= 0) {
            template_changed(1);
        }
        return 0;
    case WM_APP_TPL_FOCUS: {
        /* list closed after a pick: jump to the "Titolo" part of the name so it can be typed over */
        WCHAR cur[256];
        const WCHAR *t;
        LRESULT sel = SendMessageW(g_cbTemplate, CB_GETCURSEL, 0, 0);
        if (sel == TPL_PRESETS || !wcsstr(g_template, L"{titolo}")) return 0;
        GetWindowTextW(g_editName, cur, 256);
        t = wcsstr(cur, L"Titolo");
        if (t) { SetFocus(g_editName); SendMessageW(g_editName, EM_SETSEL, (WPARAM)(t - cur), (LPARAM)(t - cur + 6)); }
        return 0;
    }
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) { if (g_recording) stop_recording(1); else start_recording(g_pending); }
        else if (wp == HOTKEY_PAUSE && g_recording && !g_stopping) { pause_set(!g_pauseReq); update_controls(); update_title(); }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_UI) {
            /* level meter = current 10 ms RMS (the value the silence rule compares with SilenceDb),
               loudest block of the last 100 ms tick, no decay. The writer delivers every ~50 ms,
               so a tick without data keeps the last value briefly, then drops to the floor. */
            LONG r = InterlockedExchange(&g_rmsNow, 0);
            InterlockedExchange(&g_peakNow, 0);
            if (r > 0) { g_meterDb = (r - 20000) / 100.0; g_meterStale = 0; }
            else if (++g_meterStale > 3) g_meterDb = -120.0;
            if (g_recording) update_title();
            if (g_recording) src_button_update();          /* cheap: only redraws when the text changes */
            if (g_recording || g_convPending) InvalidateRect(h, NULL, FALSE);
            if (g_srcNotice && g_srcSwitches > 0) {     /* after a live switch: never modal mid-recording */
                WCHAR nm[128];
                InterlockedExchange(&g_srcNotice, 0);
                EnterCriticalSection(&g_statusLock); wcscpy(nm, g_curSrcName); LeaveCriticalSection(&g_statusLock);
                src_msg(1, 10000, L"Solo %ls non \u00E8 possibile su questo PC: registra tutto il sistema", nm);
                log_msg(L"Source switch: process loopback unavailable, whole system (shown in the window)");
            } else if (InterlockedExchange(&g_srcNotice, 0)) {
                WCHAR m[900], e[640];
                _snwprintf(m, 899, L"Non \u00E8 possibile registrare solo l'audio di %ls su questo PC "
                           L"(serve Windows 10 versione 2004 o successiva).\n\nLa registrazione continua con tutto il sistema.\n\n%ls",
                           g_curSrcName, hr_str(g_srcLastHr, e, 640));
                m[899] = 0;
                MessageBoxW(h, m, APP_TITLE L" - sorgente", MB_ICONWARNING);
            }
        } else if (wp == TIMER_DISK) {
            refresh_free();
            if (g_recording && !g_stopping && g_freeNow >= 0 && g_freeNow < LOW_DISK_STOP) {
                log_msg(L"Free disk space below 300 MB, stopping");
                stop_recording(5);
            }
            /* keep the suggested name's date/time current while idle and untouched */
            if (!g_recording && !g_nameEdited && GetFocus() != g_editName) refresh_name(0, 0);
            InvalidateRect(h, NULL, FALSE);
        } else if (wp == TIMER_RENAME) {
            KillTimer(h, TIMER_RENAME);
            if (g_recording && !g_stopping) {
                WCHAR nm[256];
                GetWindowTextW(g_editName, nm, 256);
                rename_request(nm);
            }
        } else if (wp == TIMER_TRAY) {
            KillTimer(h, TIMER_TRAY);
            if (!g_recording) tray_remove();
        }
        return 0;
    case WM_APP_STOPPED:
        on_stopped();
        return 0;
    case WM_APP_CONV:
        conv_check_idle();
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == NIN_BALLOONUSERCLICK) {
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
            if (LOWORD(lp) == NIN_BALLOONUSERCLICK && !g_recording) open_folder();
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        on_paint(h);
        return 0;
    case WM_CLOSE:
        if (g_recording) {
            int c;
            if (g_stopping) { g_closeAfterStop = 1; return 0; }
            c = ask_close(h);
            if (c == 1) { g_closeAfterStop = 1; g_stopping = 1; request_stop(6); update_controls(); }   /* suspend */
            else if (c == 2) { g_closeAfterStop = 1; stop_recording(1); }
            return 0;
        }
        if (g_convPending) {
            if (!g_exitWhenIdle) {
                g_exitWhenIdle = 1;      /* finish compressing/renaming, then exit */
                update_controls();
            } else if (MessageBoxW(h, L"Compressione in corso.\n\nInterrompere? I file WAV non ancora compressi verranno mantenuti.",
                                   APP_TITLE, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                InterlockedExchange(&g_convCancel, 1);
            }
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;                    /* shutdown may still be cancelled; act on WM_ENDSESSION */
    case WM_ENDSESSION:
        if (wp && g_recording) {
            request_stop(g_curLive ? 6 : 3);          /* live: suspend, resumable at next start */
            WaitForSingleObject(g_doneEvent, 4500);   /* writer finalizes the WAV */
            SetThreadExecutionState(ES_CONTINUOUS);
        }
        if (wp && g_sessEndMs && g_stopReason != 6) queue_finalize();   /* rename is quick; runs even when cancelling */
        if (wp && g_convPending) {
            /* no time to compress: abort (partial .m4a is deleted, WAVs are kept) */
            LONG64 until = now_ms() + 2000;
            InterlockedExchange(&g_convCancel, 1);
            while (g_convPending && now_ms() < until) Sleep(50);
        }
        if (wp) settings_save(h);
        return 0;
    case WM_DESTROY:
        settings_save(h);
        KillTimer(h, TIMER_UI); KillTimer(h, TIMER_DISK); KillTimer(h, TIMER_TRAY); KillTimer(h, TIMER_RENAME);
        if (g_hotkeyOk) UnregisterHotKey(h, HOTKEY_ID);
        if (g_hotkeyPOk) UnregisterHotKey(h, HOTKEY_PAUSE);
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* ================================================================== */
/* Headless test mode: --test-seconds N --out <folder>                 */
/* ================================================================== */
typedef struct {
    int pauseAt, resumeAt, suspendAfter, crashAfter, resumeSession, saveSession, recoverOnly;
    int nChg, chgDone;                 /* --silence-change-at S:on,db,min,pad (live settings change) */
    int nRen, renDone;                 /* --rename-at S:Name (simulates typing a new name) */
    int renAt[4]; WCHAR renName[4][128];
    int nSw, swDone;                   /* --source-switch-at S:EXE|S:system (live source switch) */
    int swAt[4]; WCHAR swExe[4][64];
    int chgAt[8], chgOn[8], chgDb[8], chgMin[8], chgPad[8];
} TestOpts;

/* MF duration of each live part of the current session (before the rename) */
static int live_verify(double *sumSec, LONG64 *sumBytes, LONG64 *maxBytes, int *nParts)
{
    int i, ok = 1;
    *sumSec = 0; *sumBytes = 0; *maxBytes = 0; *nParts = 0;
    EnterCriticalSection(&g_convLock);
    for (i = 0; i < g_partCount; i++) {
        WCHAR f[MAX_PATH * 2 + 8];
        double sec = 0;
        LONG64 b;
        HRESULT hr;
        if (g_parts[i].sess != (int)g_sessId) continue;
        _snwprintf(f, MAX_PATH * 2 + 7, L"%ls.m4a", g_parts[i].base); f[MAX_PATH * 2 + 7] = 0;
        b = file_size(f);
        hr = g_mfAvailable ? m4a_duration(f, &sec) : E_NOTIMPL;
        if (FAILED(hr)) { WCHAR e[640]; log_msg(L"VERIFY %ls: cannot read duration (%ls)", f, hr_str(hr, e, 640)); ok = 0; }
        else log_msg(L"VERIFY %ls: %.0f bytes, %.2f s", f, (double)b, sec);
        if (b > (LONG64)M4A_MAX_BYTES) { log_msg(L"VERIFY %ls exceeds %d MB", f, g_maxFileMB); ok = 0; }
        *sumSec += sec; *sumBytes += b > 0 ? b : 0;
        if (b > *maxBytes) *maxBytes = b;
        (*nParts)++;
    }
    LeaveCriticalSection(&g_convLock);
    return ok;
}

static LONG64 g_timerLast, g_timerBack; static LONG g_timerSamples;   /* test: big timer monotonic */
static int run_test(int seconds, const WCHAR *outArg, const WCHAR *nameArg, const TestOpts *t)
{
    WCHAR finalName[256] = L"";
    WCHAR folder[MAX_PATH], err[1024];
    LONG64 t0, end;
    int ok, resumed = 0, mfStarted = 0;
    double baseSec = 0;
    if (!make_folder(outArg, folder, err, 1024)) return 3;
    _snwprintf(g_logPath, MAX_PATH * 2 - 1, L"%ls\\test.log", folder);
    g_logPath[MAX_PATH * 2 - 1] = 0;
    _snwprintf(g_sessIni, MAX_PATH * 2 - 1, L"%ls\\test_session.ini", folder);   /* never touches the GUI session */
    g_sessIni[MAX_PATH * 2 - 1] = 0;
    if (nameArg) sanitize_name(nameArg, finalName, 256);
    log_msg(L"TEST start: %d s -> %ls (convert=%d live=%d kbps=%d mono=%d keep_wav=%d split_seconds=%d segment_seconds=%d "
            L"max_mb=%d name=\"%ls\" pause_at=%d resume_at=%d suspend_after=%d crash_after=%d resume_session=%d save_session=%d%ls%ls)",
            seconds, folder, g_compress, g_compressLive, g_aacKbps, g_mono, g_keepWav, g_testSplitSec, g_testSegSec, g_maxFileMB,
            finalName, t->pauseAt, t->resumeAt, t->suspendAfter, t->crashAfter, t->resumeSession, t->saveSession,
            g_fakeAacPath[0] ? L" fake_aac=" : L"", g_fakeAacPath);
    log_msg(L"TEST silence_cut=%d db=%d min=%d pad=%d synth=%.1f,%.1f", g_silCut, g_silDb, g_silMinSec, g_silPadMs,
            g_synthSound, g_synthQuiet);
    conv_init();
    if (g_mfAvailable) mfStarted = SUCCEEDED(mf.Startup(MF_VERSION, MFSTARTUP_LITE));   /* for verification */
    sess_load_pending();
    {   /* orphaned live parts (crash without a resumable session) */
        int files = 0; double sec = 0;
        int n = recover_folder(folder, g_pending ? g_pendBase : NULL, &files, &sec);
        if (n > 0 || t->recoverOnly)
            log_msg(L"RESULT_RECOVER %ls sessions=%d files=%d audio=%.2fs first=%ls", (n > 0 || t->recoverOnly) ? L"OK" : L"FAIL",
                    n, files, sec, g_recoverMsg);
        if (t->recoverOnly) { conv_shutdown(5000); return 0; }
    }
    if (t->saveSession) {
        if (!g_pending) { log_msg(L"RESULT_SAVE_SESSION FAIL no pending session"); conv_shutdown(5000); return 1; }
        {
            LONG64 ms = g_pendMs;
            double sec = 0;
            HRESULT hr;
            sess_save_now();
            hr = g_mfAvailable ? m4a_duration(g_finFirst, &sec) : E_NOTIMPL;
            log_msg(L"RESULT_SAVE_SESSION %ls files=%d bytes=%.0f expected=%.2fs first_duration=%.2fs%ls first=%ls",
                    g_finCount > 0 ? L"OK" : L"FAIL", g_finCount, (double)g_finBytes, ms / 1000.0, sec,
                    SUCCEEDED(hr) ? L"" : L" (duration not verified)", g_finFirst);
            conv_shutdown(5000);
            return g_finCount > 0 ? 0 : 1;
        }
    }
    if (t->resumeSession) {
        if (!g_pending) { log_msg(L"RESULT_RESUME FAIL no pending session in %ls", g_sessIni); conv_shutdown(5000); return 1; }
        baseSec = g_pendMs / 1000.0;
        if (!finalName[0]) sanitize_name(g_pendName, finalName, 256);
        resumed = 1;
        log_msg(L"TEST resuming session %ls (%.2f s recorded, %ls)", g_pendBase, baseSec, g_pendCrashed ? L"crashed" : L"suspended");
    }
    g_convEnabled = g_compress && g_mfAvailable && g_convThread;
    if (g_startSrcExe[0] && !g_startSrcName[0]) {   /* --source-exe: friendly name like the picker */
        DWORD pid = src_find_root(g_startSrcExe);
        if (pid) proc_friendly(pid, g_startSrcExe, g_startSrcName, 128);
        else wcsncpy(g_startSrcName, g_startSrcExe, 127);
        log_msg(L"TEST source %ls -> \"%ls\" root PID %lu%ls", g_startSrcExe, g_startSrcName, (unsigned long)pid,
                pid ? L"" : L" (not running)");
    }
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    if (!rec_start(folder, 0, resumed, err, 1024)) {
        log_msg(L"RESULT FAIL could not start: %ls", err);
        return 1;
    }
    if (finalName[0]) { live_save_name(finalName); rename_request(finalName); }
    t0 = now_ms();
    end = t0 + (LONG64)seconds * 1000;
    while (now_ms() < end && WaitForSingleObject(g_stopEvent, 100) != WAIT_OBJECT_0) {
        LONG64 fb = free_bytes(folder), el = now_ms() - t0;
        {   /* the big timer must never go backward */
            LONG64 f = file_ms();
            if (g_timerSamples && g_timerLast - f > g_timerBack) g_timerBack = g_timerLast - f;
            g_timerLast = f; g_timerSamples++;
        }
        if (fb >= 0 && fb < LOW_DISK_STOP) { log_msg(L"Free disk space below 300 MB, stopping"); request_stop(5); }
        if (t->pauseAt > 0 && !g_pauseReq && el >= t->pauseAt * 1000LL && (t->resumeAt <= 0 || el < t->resumeAt * 1000LL)) pause_set(1);
        if (t->resumeAt > 0 && g_pauseReq && el >= t->resumeAt * 1000LL) pause_set(0);
        {   /* level meter check: the value the UI bar shows (10 ms RMS, same as the silence rule) */
            static LONG64 lastM;
            LONG r = InterlockedExchange(&g_rmsNow, 0);
            if (el - lastM >= 500) {
                double mdb = r > 0 ? (r - 20000) / 100.0 : -120.0;
                lastM = el;
                log_msg(L"METER %.1f s: level %.1f dBFS, threshold %d -> %ls, silence run %.2f s, timer %.2f s", el / 1000.0, mdb, g_silDb,
                        r <= 0 ? L"no data" : mdb < g_silDb ? L"below" : L"above", (double)atomic_read64(&g_silRunF) / OUT_RATE,
                        file_ms() / 1000.0);
            }
        }
        if (t->crashAfter > 0 && el >= t->crashAfter * 1000LL) {
            log_msg(L"SIMULATED CRASH after %d s: terminating the process now (part %d, %lld frames written)",
                    t->crashAfter, g_wav.part, (long long)g_lv.totalFrames);
            TerminateProcess(GetCurrentProcess(), 99);
        }
        while (((TestOpts *)t)->chgDone < t->nChg && el >= t->chgAt[t->chgDone] * 1000LL) {
            int k = ((TestOpts *)t)->chgDone++;
            g_silCut = t->chgOn[k]; g_silDb = t->chgDb[k]; g_silMinSec = t->chgMin[k]; g_silPadMs = t->chgPad[k];
            log_msg(L"TEST live settings change at %.1f s: on=%d db=%d min=%d pad=%d", el / 1000.0,
                    g_silCut, g_silDb, g_silMinSec, g_silPadMs);
            gate_request_update();
        }
        while (((TestOpts *)t)->renDone < t->nRen && el >= t->renAt[t->renDone] * 1000LL) {
            int k = ((TestOpts *)t)->renDone++;
            sanitize_name(t->renName[k], finalName, 256);
            log_msg(L"TEST rename at %.1f s to \"%ls\"", el / 1000.0, finalName);
            live_save_name(finalName);
            rename_request(finalName);
        }
        while (((TestOpts *)t)->swDone < t->nSw && el >= t->swAt[t->swDone] * 1000LL) {
            int k = ((TestOpts *)t)->swDone++;
            WCHAR nm[128] = L"";
            const WCHAR *ex = _wcsicmp(t->swExe[k], L"system") ? t->swExe[k] : L"";
            DWORD pid = ex[0] ? src_find_root(ex) : 0;
            if (ex[0]) { if (pid) proc_friendly(pid, ex, nm, 128); else wcsncpy(nm, ex, 127); }
            log_msg(L"TEST source switch at %.1f s to %ls%ls (timer %.2f s)", el / 1000.0, ex[0] ? nm : L"whole system",
                    ex[0] && !pid ? L" (not running)" : L"", file_ms() / 1000.0);
            src_switch_request(ex, nm);
        }
        if (t->suspendAfter > 0 && el >= t->suspendAfter * 1000LL) { request_stop(6); break; }
    }
    if (t->nRen > 0 || finalName[0]) {   /* while still recording: is the open file already named? */
        WCHAR cur[MAX_PATH * 2];
        int okName;
        EnterCriticalSection(&g_statusLock);
        wcscpy(cur, g_curPath);
        LeaveCriticalSection(&g_statusLock);
        okName = finalName[0] && !_wcsnicmp(base_name(cur), finalName, wcslen(finalName)) && path_exists(cur);
        log_msg(L"RESULT_RENAME_LIVE %ls requested=\"%ls\" open_file=%ls", okName ? L"OK" : L"FAIL", finalName, cur);
    }
    if (g_stopReason != 6) { if (g_pauseReq) pause_set(0); request_stop(1); }
    if (WaitForSingleObject(g_doneEvent, 30000) != WAIT_OBJECT_0) {
        log_msg(L"RESULT FAIL finalize timeout");
        return 1;
    }
    SetThreadExecutionState(ES_CONTINUOUS);
    ok = !g_writeError && g_bytesWritten > 0;
    log_msg(L"RESULT %ls bytes=%.0f audio=%.2fs peak=%.1f dBFS (%.5f) synthesized=%.2fs dropped=%.2fs "
            L"discontinuities=%ld reconnects=%ld device=\"%ls\" format=\"%ls\" file=%ls",
            ok ? L"OK" : L"FAIL", (double)g_bytesWritten, (double)g_framesOut / OUT_RATE,
            peak_db(g_peakMax), g_peakMax / 1000000.0, (double)g_framesPadded / OUT_RATE,
            (double)g_framesDropped / OUT_RATE, (long)g_discont, (long)g_reconnects,
            g_devName, g_fmtDesc, g_curPath);
    {
        static const WCHAR *st[4] = { L"system", L"app", L"waiting", L"fallback-system" };
        WCHAR e[640];
        log_msg(L"RESULT_SOURCE %ls requested=\"%ls\" name=\"%ls\" state=%ls attached=%ld exits=%ld activation_failures=%ld "
                L"last_pid=%ld captured=%.2fs synthesized=%.2fs switches=%ld/%d last_error=%ls",
                (!g_curSrcExe[0] || g_srcState == 1 || g_srcState == 3 || g_srcState == 2) ? L"OK" : L"FAIL",
                g_curSrcExe, g_curSrcName, st[g_srcState & 3], (long)g_srcAttaches, (long)g_srcExits, (long)g_srcActFails,
                (long)g_srcPid, (double)(g_framesOut - g_framesPadded) / OUT_RATE, (double)g_framesPadded / OUT_RATE,
                (long)g_srcSwitches, t->nSw, g_srcLastHr == S_OK ? L"none" : hr_str(g_srcLastHr, e, 640));
    }
    {   /* the big timer shown in the UI must match what was actually written */
        double uiSec;
        if (g_timerSamples && g_timerLast - file_ms() > g_timerBack) g_timerBack = g_timerLast - file_ms();
        uiSec = file_ms() / 1000.0;
        double pcmSec = (g_resumeBaseMs - g_cutBaseMs) / 1000.0 + (double)g_fileFrames / OUT_RATE;
        double encSec = g_curLive ? (double)g_lv.totalFrames * AAC_FRAME / OUT_RATE : pcmSec;
        int dok = fabs(uiSec - encSec) <= 0.05 && g_timerBack <= 0;
        log_msg(L"RESULT_DURATION %ls ui_timer=%.3fs encoded=%.3fs pcm_written=%.3fs diff=%+.3fs timeline=%.3fs backward_max=%lldms samples=%ld "
                L"cut=%.2fs paused_session=%.2fs (this run %.2fs) silences_metered=%ld last_silence=%.1fs",
                dok ? L"OK" : L"FAIL", uiSec, encSec, pcmSec, uiSec - encSec, elapsed_ms() / 1000.0, (long long)g_timerBack, (long)g_timerSamples,
                cut_total_ms() / 1000.0, paused_total_ms() / 1000.0, g_pausedTotalMs / 1000.0, (long)g_silEvents,
                (double)g_silLastF / OUT_RATE);
    }
    if (g_stopReason == 6) {
        int pend = sess_load_pending();
        log_msg(L"RESULT_SUSPEND %ls session=%ls recorded=%.2fs part=%d ini=%ls", (pend && ok) ? L"OK" : L"FAIL",
                g_pendBase, g_pendMs / 1000.0, g_wav.part, g_sessIni);
        conv_shutdown(5000);
        return (pend && ok) ? 0 : 1;
    }
    {
        double runSec = (now_ms() - t0) / 1000.0;   /* informational */
        double pausedSec = g_pausedTotalMs / 1000.0;
        double audioSec = g_curLive ? (double)g_lv.totalFrames * AAC_FRAME / OUT_RATE
                        : g_gateUsed ? (double)g_bytesWritten / OUT_BPF / OUT_RATE    /* WAV data actually written */
                        : (double)g_framesOut / OUT_RATE;
        double cutSec = (double)g_cutFrames / OUT_RATE;
        double expect = baseSec + seconds - pausedSec - cutSec;
        (void)runSec;
        if (g_gateUsed) {
            double timeline = (double)g_framesOut / OUT_RATE;
            double written = audioSec - baseSec;
            int sok = fabs(timeline - cutSec - (audioSec - baseSec)) < 0.2;
            log_msg(L"RESULT_SILENCE %ls wall=%.2fs timeline=%.2fs cut=%.2fs cuts=%ld written=%.2fs (timeline-cut=%.2fs) "
                    L"db=%d min=%d pad=%d", sok ? L"OK" : L"FAIL", seconds - pausedSec, timeline, cutSec, (long)g_gateCuts,
                    written, timeline - cutSec, g_gateDb, g_gateMinSec, g_gatePadMs);
            if (!sok) ok = 0;
        }
        if (t->pauseAt > 0) {
            int pok = fabs(audioSec - expect) < 1.5;
            log_msg(L"RESULT_PAUSE %ls audio=%.2fs expected=%.2fs (test %d s - paused %.2f s)", pok ? L"OK" : L"FAIL",
                    audioSec - baseSec, expect - baseSec, seconds, pausedSec);
            if (!pok) ok = 0;
        }
        if (g_curLive) {
            double sum = 0; LONG64 bytes = 0, maxB = 0; int nParts = 0;
            int vok = live_verify(&sum, &bytes, &maxB, &nParts);
            int lok = nParts > 0 && maxB <= (LONG64)M4A_MAX_BYTES && fabs(audioSec - expect) < 2.0;
            double mbh = audioSec > 0 ? bytes / 1e6 * 3600.0 / audioSec : 0;
            if (vok && fabs(sum - audioSec) > 1.0) { log_msg(L"VERIFY duration mismatch: files %.2f s vs written %.2f s", sum, audioSec); lok = 0; }
            if (!vok && !g_fakeAacPath[0]) lok = 0;   /* dev fake-encoder runs under Wine cannot decode */
            log_msg(L"RESULT_LIVE %ls parts=%d m4a_bytes=%.0f max_part_bytes=%.0f limit=%.0f audio=%.2fs expected=%.2fs "
                    L"files_duration=%.2fs%ls kbps=%d channels=%d mb_per_hour=%.1f",
                    lok ? L"OK" : L"FAIL", nParts, (double)bytes, (double)maxB, M4A_MAX_BYTES, audioSec, expect, sum,
                    vok ? L"" : L" (not verified)", g_curKbps, g_curMono ? 1 : 2, mbh);
            if (!lok) ok = 0;
            if (resumed) {
                int rok = lok && (nParts == 1 || g_testSplitSec > 0 || maxB > (LONG64)(M4A_MAX_BYTES * 0.9));
                log_msg(L"RESULT_RESUME %ls parts=%d audio=%.2fs = before %.2fs + this run %.2fs (one continuous file: %ls)",
                        rok ? L"OK" : L"FAIL", nParts, audioSec, baseSec, audioSec - baseSec, nParts == 1 ? L"yes" : L"split by size");
                if (!rok) ok = 0;
            }
        }
    }
    if ((nameArg || resumed || t->nRen) && !finalize_request(finalName)) log_msg(L"Could not queue the final rename");
    {
        /* wait for AAC conversion of every part and the final rename (generous timeout) */
        LONG64 limit = now_ms() + 600000LL + (LONG64)seconds * 1000;
        while (g_convPending && now_ms() < limit) Sleep(100);
    }
    if (g_convEnabled) {
        int cok, cfail;
        EnterCriticalSection(&g_convLock);
        cok = g_sessConvOk; cfail = g_sessConvFail;
        log_msg(L"RESULT_M4A %ls parts_ok=%d parts_failed=%d m4a_bytes=%.0f m4a_duration=%.2fs last=%ls%ls%ls",
                (g_convPending == 0 && cok > 0 && cfail == 0) ? L"OK" : L"FAIL", cok, cfail,
                (double)g_sessM4aBytes, g_sessM4aSec, g_convLastOut,
                g_convLastErr[0] ? L" error=" : L"", g_convLastErr);
        LeaveCriticalSection(&g_convLock);
        if (g_convPending || cok == 0 || cfail) ok = 0;
    }
    if (nameArg || resumed || t->nRen) {
        int nameOk;
        EnterCriticalSection(&g_convLock);
        nameOk = g_finValid && g_finCount > 0 && (!finalName[0] ||
                 !_wcsnicmp(base_name(g_finFirst), finalName, wcslen(finalName))) && path_exists(g_finFirst);
        log_msg(L"RESULT_NAME %ls requested=\"%ls\" files=%d bytes=%.0f first=%ls",
                nameOk ? L"OK" : L"FAIL", finalName, g_finCount, (double)g_finBytes, g_finFirst);
        LeaveCriticalSection(&g_convLock);
        if (!nameOk) ok = 0;
    }
    if (mfStarted) mf.Shutdown();
    conv_shutdown(5000);
    return ok ? 0 : 1;
}

/* ================================================================== */
/* Entry point                                                         */
/* ================================================================== */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int nShow)
{
    int argc = 0, i, testSec = -1, rc = 0, optKbps = 0, optMono = 0, optKeep = 0, optNoConv = 0;
    WCHAR **argv;
    const WCHAR *outArg = NULL, *nameArg = NULL;
    int optMaxMb = 0, optAfter = 0;
    int optSil = -1, optSilDb = 1, optSilMin = 0, optSilPad = -1;
    TestOpts topt;
    HANDLE mutex = NULL;
    int optListSrc = 0;
    LARGE_INTEGER f;
    (void)hPrev; (void)cmdLine;

    g_hinst = hInst;
    memset(&topt, 0, sizeof(topt));
    InitializeCriticalSection(&g_sessLock);
    InitializeCriticalSection(&g_outLock);
    InitializeCriticalSection(&g_statusLock);
    InitializeCriticalSection(&g_gateReqLock);
    InitializeCriticalSection(&g_renLock);
    QueryPerformanceFrequency(&f);
    g_qpcFreq = f.QuadPart;
    g_stopEvent       = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_doneEvent       = CreateEventW(NULL, TRUE, TRUE, NULL);
    g_devChangedEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_srcSwitchEvent  = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_ring = (BYTE *)VirtualAlloc(NULL, RING_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_stopEvent || !g_doneEvent || !g_devChangedEvent || !g_ring) return 10;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--test-seconds") && i + 1 < argc) testSec = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--split-seconds") && i + 1 < argc) g_testSplitSec = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--kbps") && i + 1 < argc) optKbps = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--mono")) optMono = 1;
        else if (!wcscmp(argv[i], L"--keep-wav")) optKeep = 1;
        else if (!wcscmp(argv[i], L"--no-convert")) optNoConv = 1;
        else if (!wcscmp(argv[i], L"--name") && i + 1 < argc) nameArg = argv[++i];
        else if (!wcscmp(argv[i], L"--max-mb") && i + 1 < argc) optMaxMb = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--out") && i + 1 < argc) outArg = argv[++i];
        else if (!wcscmp(argv[i], L"--segment-seconds") && i + 1 < argc) g_testSegSec = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--simulate-crash") && i + 1 < argc) topt.crashAfter = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--pause-at") && i + 1 < argc) topt.pauseAt = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--resume-at") && i + 1 < argc) topt.resumeAt = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--suspend-after") && i + 1 < argc) topt.suspendAfter = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--resume-session")) topt.resumeSession = 1;
        else if (!wcscmp(argv[i], L"--save-session")) { topt.saveSession = 1; if (testSec < 0) testSec = 0; }
        else if (!wcscmp(argv[i], L"--recover-only")) { topt.recoverOnly = 1; if (testSec < 0) testSec = 0; }
        else if (!wcscmp(argv[i], L"--after")) optAfter = 1;
        else if (!wcscmp(argv[i], L"--silence-cut")) optSil = 1;
        else if (!wcscmp(argv[i], L"--rename-at") && i + 1 < argc) {
            const WCHAR *a = argv[++i], *c = wcschr(a, L':');
            if (c && topt.nRen < 4) {
                topt.renAt[topt.nRen] = _wtoi(a);
                wcsncpy(topt.renName[topt.nRen], c + 1, 127); topt.renName[topt.nRen][127] = 0;
                topt.nRen++;
            }
        }
        else if (!wcscmp(argv[i], L"--silence-change-at") && i + 1 < argc) {
            int k = topt.nChg;
            if (k < 8 && swscanf(argv[++i], L"%d:%d,%d,%d,%d", &topt.chgAt[k], &topt.chgOn[k], &topt.chgDb[k],
                                 &topt.chgMin[k], &topt.chgPad[k]) == 5 &&
                topt.chgDb[k] >= -70 && topt.chgDb[k] <= -20 && topt.chgMin[k] >= 1 && topt.chgMin[k] <= 60 &&
                topt.chgPad[k] >= 0 && topt.chgPad[k] <= 5000) topt.nChg++;
        }
        else if (!wcscmp(argv[i], L"--synth") && i + 1 < argc) {
            if (swscanf(argv[++i], L"%lf,%lf", &g_synthSound, &g_synthQuiet) != 2 || g_synthSound <= 0 || g_synthQuiet < 0)
                g_synthSound = g_synthQuiet = 0;
        }
        else if (!wcscmp(argv[i], L"--no-silence-cut")) optSil = 0;
        else if (!wcscmp(argv[i], L"--silence-db") && i + 1 < argc) optSilDb = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--silence-min") && i + 1 < argc) optSilMin = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--silence-pad") && i + 1 < argc) optSilPad = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--source-exe") && i + 1 < argc) { wcsncpy(g_startSrcExe, argv[++i], 63); g_startSrcExe[63] = 0; }
        else if (!wcscmp(argv[i], L"--source-switch-at") && i + 1 < argc) {
            const WCHAR *a = argv[++i], *c = wcschr(a, L':');
            if (c && c[1] && topt.nSw < 4) {
                topt.swAt[topt.nSw] = _wtoi(a);
                wcsncpy(topt.swExe[topt.nSw], c + 1, 63); topt.swExe[topt.nSw][63] = 0;
                topt.nSw++;
            }
        }
        else if (!wcscmp(argv[i], L"--list-sources")) optListSrc = 1;
        else if (!wcscmp(argv[i], L"--force-proc-fail")) g_forceProcFail = 1;
        else if (!wcscmp(argv[i], L"--open-source-menu")) g_openSrcMenu = 1;
        else if (!wcscmp(argv[i], L"--fake-aac") && i + 1 < argc) { wcsncpy(g_fakeAacPath, argv[++i], MAX_PATH - 1); g_fakeAacPath[MAX_PATH - 1] = 0; }
    }

    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 11;
    g_testMode = testSec >= 0 || optListSrc;
    settings_init();
    if (optListSrc) {            /* headless: what the source picker would list */
        WCHAR folder[MAX_PATH], err[512];
        int k;
        if (!make_folder(outArg ? outArg : L".", folder, err, 512)) { CoUninitialize(); return 3; }
        _snwprintf(g_logPath, MAX_PATH * 2 - 1, L"%ls\\sources.log", folder); g_logPath[MAX_PATH * 2 - 1] = 0;
        src_enum();
        log_msg(L"SOURCES %d app(s)", g_srcAppN);
        for (k = 0; k < g_srcAppN; k++)
            log_msg(L"SOURCE exe=%ls name=\"%ls\" root_pid=%lu audio_session=%d playing=%d", g_srcApps[k].exe, g_srcApps[k].name,
                    (unsigned long)g_srcApps[k].pid, g_srcApps[k].session, g_srcApps[k].active);
        if (argv) LocalFree(argv);
        CoUninitialize();
        return 0;
    }
    settings_load();
    if (testSec >= 0) {          /* test-mode overrides (not saved) */
        if (optKbps >= 32 && optKbps <= 320) g_aacKbps = optKbps;
        if (optMono) g_mono = 1;
        if (optKeep) g_keepWav = 1;
        if (optNoConv) g_compress = 0;
        g_compressLive = !optAfter;
        if (optMaxMb >= 1 && optMaxMb <= 4000) g_maxFileMB = optMaxMb;
        if (g_testSplitSec < 0) g_testSplitSec = 0;
        if (optSil >= 0) g_silCut = optSil;
        if (optSilDb >= -70 && optSilDb <= -20) g_silDb = optSilDb;
        if (optSilMin >= 1 && optSilMin <= 60) g_silMinSec = optSilMin;
        if (optSilPad >= 0 && optSilPad <= 5000) g_silPadMs = optSilPad;
    }

    if (testSec >= 0) {
        if (testSec < 1) testSec = 1;
        rc = run_test(testSec, outArg ? outArg : g_saveFolder, nameArg, &topt);
        if (argv) LocalFree(argv);
        CoUninitialize();
        return rc;
    }
    if (argv) LocalFree(argv);
#ifndef GUI_SYNTH_SHOT
    g_synthSound = g_synthQuiet = 0;   /* test-only signal never applies to the GUI */
#endif

    /* single instance: bring the existing window to front */
    mutex = CreateMutexW(NULL, TRUE, L"Local\\LoopbackRecGui.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(WND_CLASS, NULL);
        if (other) {
            if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
            ShowWindow(other, SW_SHOW);
            SetForegroundWindow(other);
        }
        CloseHandle(mutex);
        CoUninitialize();
        return 0;
    }

    settings_save(NULL);         /* writes any missing keys with their defaults */
    conv_init();
    g_convEnabled = g_compress && g_mfAvailable && g_convThread;
    {   /* resumable / crashed session first, then orphaned live parts in the save folder */
        int files = 0; double sec = 0;
        sess_load_pending();
        recover_folder(g_saveFolder, g_pending ? g_pendBase : NULL, &files, &sec);
        if (files) log_msg(L"Recovered %d interrupted file(s), %.1f s", files, sec);
    }
    dpi_init();
    {
        INITCOMMONCONTROLSEX icc;
        WNDCLASSEXW wc;
        HWND h;
        MSG msg;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
        g_icoBig   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        g_icoSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        if (!g_icoBig) g_icoBig = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
        if (!g_icoSmall) g_icoSmall = g_icoBig;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndproc;
        wc.hInstance = hInst;
        wc.hIcon = g_icoBig;
        wc.hIconSm = g_icoSmall;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = WND_CLASS;
        if (!RegisterClassExW(&wc)) { CoUninitialize(); return 12; }
        g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        g_msgTbButton = RegisterWindowMessageW(L"TaskbarButtonCreated");
        h = CreateWindowExW(0, WND_CLASS, APP_TITLE,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                            g_winX, g_winY, 440, 320, NULL, NULL, hInst, NULL);
        if (!h) { CoUninitialize(); return 13; }
        g_notifyHwnd = h;
        g_convNotify = h;
        ShowWindow(h, nShow);
        UpdateWindow(h);
        if (g_openSrcMenu) PostMessageW(h, WM_COMMAND, IDC_SRCBTN, 0);   /* screenshot helper */
        if (g_recoverMsg[0]) {
            tray_set_ex(1, APP_TITLE, L"Recuperata registrazione interrotta", base_name(g_recoverMsg), NIIF_INFO);
            SetTimer(h, TIMER_TRAY, 15000, NULL);
            wcscpy(g_lastFile, g_recoverMsg);
        }
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        rc = (int)msg.wParam;
    }
    /* safety net: never leave an unfinalized file */
    if (g_recording) { request_stop(1); WaitForSingleObject(g_doneEvent, 10000); }
    g_convNotify = NULL;
    if (g_convPending) {
        /* window closed with work left (should not happen: close waits) - finish it */
        LONG64 until = now_ms() + 600000;
        while (g_convPending && now_ms() < until) Sleep(100);
    }
    conv_shutdown(5000);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    CoUninitialize();
    return rc;
}
