/*
 * loopback_rec.c - record everything played on the default Windows output
 * device (WASAPI shared-mode loopback) to a 48 kHz / 16-bit / stereo WAV file.
 *
 * Designed for long (multi-hour) unattended sessions:
 *   - continuous timeline: silence is synthesized when WASAPI delivers no
 *     packets (nothing playing, device lost, reconnecting), and packet QPC
 *     timestamps are used to fill discontinuities, so the file stays in sync
 *     with wall-clock time;
 *   - a silent render stream on the same device keeps loopback packets flowing;
 *   - follows default-device changes and recovers from device invalidation /
 *     audio service restarts with retry + backoff;
 *   - any mix format (float/int, any channel count, any rate) is converted to
 *     the fixed output format;
 *   - capture runs in a "Pro Audio" MMCSS thread and hands data to a separate
 *     writer thread through a lock-free ring buffer (~170 s of slack);
 *   - WAV header is rewritten and the file flushed every 10 s, so a crash or
 *     power loss still leaves a playable file; rolls to a new part < 4 GB;
 *   - clean finalize on Q / Enter / Ctrl+C / console close / logoff / shutdown;
 *   - keeps the system awake while recording.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -static -o loopback_rec.exe \
 *       loopback_rec.c -lole32 -lavrt -lshell32 -luser32
 * Build (MSVC, Developer Command Prompt):
 *   cl /nologo /O2 /W3 loopback_rec.c ole32.lib avrt.lib shell32.lib user32.lib
 *
 * Usage:  loopback_rec.exe [output_folder]
 *   default folder: %USERPROFILE%\Documents\Recordings
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
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
#include <propidl.h>
#include <process.h>
#include <conio.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
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
static const GUID MY_FOLDERID_Documents       = {0xFDD39AD0,0x238F,0x46AF,{0xAD,0xB4,0x6C,0x85,0x48,0x03,0x69,0xC7}};
static const PROPERTYKEY MY_PKEY_Device_FriendlyName =
    {{0xA45C254E,0xDF1C,0x4EFD,{0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}}, 14};

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
static volatile LONG g_stopReason;   /* 0 none, 1 key, 2 ctrl-c, 3 close/logoff/shutdown, 4 error */

static BYTE *g_ring;
static volatile LONG64 g_ringW;   /* total bytes produced (capture thread) */
static volatile LONG64 g_ringR;   /* total bytes consumed (writer thread)  */

static LONG64 g_qpcFreq;
static LONG64 g_start100ns;       /* timeline origin, 100-ns QPC units */
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

static CRITICAL_SECTION g_outLock;
static int g_isConsole;
static int g_statusOnLine;        /* a \r status line is currently displayed */

/* ------------------------------------------------------------------ */
/* Output / logging                                                    */
/* ------------------------------------------------------------------ */
static void out_w(const WCHAR *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n = 0;
    if (h == NULL || h == INVALID_HANDLE_VALUE) return;
    if (g_isConsole) {
        WriteConsoleW(h, s, (DWORD)wcslen(s), &n, NULL);
    } else {
        char buf[4096];
        int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf), NULL, NULL);
        if (len > 1) WriteFile(h, buf, (DWORD)(len - 1), &n, NULL);
    }
}

static void log_msg(const WCHAR *fmt, ...)
{
    WCHAR msg[1024], line[1200];
    SYSTEMTIME st;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 1023, fmt, ap);
    va_end(ap);
    msg[1023] = 0;
    GetLocalTime(&st);
    EnterCriticalSection(&g_outLock);
    _snwprintf(line, 1199, L"%ls[%02u:%02u:%02u] %ls\r\n",
               g_statusOnLine ? L"\r                                                                                                                      \r" : L"",
               st.wHour, st.wMinute, st.wSecond, msg);
    line[1199] = 0;
    out_w(line);
    g_statusOnLine = 0;
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
} WavOut;

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
    for (attempt = 0; attempt < 100; attempt++) {
        WCHAR sfx[32] = L"";
        if (o->part > 1)  _snwprintf(sfx, 31, L"_part%d", o->part);
        if (attempt > 0)  _snwprintf(sfx + wcslen(sfx), 31 - wcslen(sfx), L"_%d", attempt);
        sfx[31] = 0;
        _snwprintf(path, MAX_PATH * 2 - 1, L"%ls\\%ls%ls.wav", o->folder, o->base, sfx);
        path[MAX_PATH * 2 - 1] = 0;
        o->h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (o->h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (o->h == INVALID_HANDLE_VALUE) {
        LOG_HR(L"Creating output file", HRESULT_FROM_WIN32(GetLastError()));
        return FALSE;
    }
    o->dataBytes = 0;
    if (!wav_header(o)) {
        LOG_HR(L"Writing WAV header", HRESULT_FROM_WIN32(GetLastError()));
        CloseHandle(o->h); o->h = INVALID_HANDLE_VALUE;
        return FALSE;
    }
    EnterCriticalSection(&g_statusLock);
    wcsncpy(g_curPath, path, MAX_PATH * 2 - 1);
    LeaveCriticalSection(&g_statusLock);
    log_msg(L"Writing %ls", path);
    return TRUE;
}

static void wav_close(WavOut *o)
{
    if (o->h == INVALID_HANDLE_VALUE) return;
    if (!wav_header(o)) LOG_HR(L"Finalizing WAV header", HRESULT_FROM_WIN32(GetLastError()));
    FlushFileBuffers(o->h);
    CloseHandle(o->h);
    o->h = INVALID_HANDLE_VALUE;
}

/* write n bytes to the current part, rolling to a new part near 4 GiB */
static BOOL wav_write(WavOut *o, const BYTE *p, size_t n)
{
    while (n) {
        ULONG64 room = MAX_DATA_BYTES - o->dataBytes;
        size_t chunk = n;
        if (room < OUT_BPF) {
            wav_close(o);
            o->part++;
            log_msg(L"Approaching 4 GB WAV limit, starting part %d", o->part);
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

static unsigned __stdcall writer_thread(void *arg)
{
    WavOut *o = (WavOut *)arg;
    LONG64 lastHdr = now_ms();
    BOOL ok = TRUE;
    for (;;) {
        /* sample "capture finished" BEFORE draining so nothing is left behind */
        BOOL finished = (WaitForSingleObject(g_captureThread, 0) == WAIT_OBJECT_0);
        LONG64 w = atomic_read64(&g_ringW), r = g_ringR;
        while (ok && w > r) {
            size_t off = (size_t)(r % RING_BYTES);
            size_t n = (size_t)(w - r);
            if (n > RING_BYTES - off) n = RING_BYTES - off;
            if (n > 1024 * 1024) n = 1024 * 1024;
            ok = wav_write(o, g_ring + off, n);
            r += (LONG64)n;
            InterlockedExchange64(&g_ringR, r);
        }
        if (!ok) {
            /* disk full / I/O error: keep consuming so capture never blocks, stop recording */
            InterlockedExchange(&g_writeError, 1);
            request_stop(4);
            InterlockedExchange64(&g_ringR, w);
        }
        if (finished) break;
        if (ok && now_ms() - lastHdr >= HEADER_EVERY_MS) {
            lastHdr = now_ms();
            if (!wav_header(o)) LOG_HR(L"Updating WAV header", HRESULT_FROM_WIN32(GetLastError()));
            FlushFileBuffers(o->h);
        }
        WaitForSingleObject(g_captureThread, 50);
    }
    wav_close(o);
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
        silentFlag = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        peak = to_stereo(c, data, frames, silentFlag);
        hr = IAudioCaptureClient_ReleaseBuffer(d->capc, frames);   /* release ASAP */
        if (FAILED(hr)) return hr;
        if (frames == 0) continue;
        *gotPacket = 1;

        quiet = silentFlag || peak < QUIET_PEAK;
        flagged = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        if (flagged) InterlockedIncrement(&g_discont);

        /* keep the file aligned with the QPC timeline */
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

static unsigned __stdcall capture_thread(void *arg)
{
    Conv conv;
    DWORD taskIdx = 0;
    HANDLE mmcss;
    HRESULT hr;
    DWORD backoff = BACKOFF_MIN_MS;
    int first = 1;
    (void)arg;

    memset(&conv, 0, sizeof(conv));
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LOG_HR(L"CoInitializeEx(capture thread)", hr); request_stop(4); return 1; }
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    if (!mmcss) log_msg(L"Warning: MMCSS 'Pro Audio' not available (error %lu), using normal priority",
                        (unsigned long)GetLastError());
    if (!mmcss) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        Dev d;
        LONG64 lastPacket, lastCheck;
        HANDLE waits[2];

        ResetEvent(g_devChangedEvent);
        hr = dev_open(&d, &conv);
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
                pad_to_now(&conv);
                if (WaitForSingleObject(g_stopEvent, 100) == WAIT_OBJECT_0) break;
                if (WaitForSingleObject(g_devChangedEvent, 0) == WAIT_OBJECT_0) break;
            }
            backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
            continue;
        }
        backoff = BACKOFF_MIN_MS;
        InterlockedExchange(&g_haveDevice, 1);
        if (!first) InterlockedIncrement(&g_reconnects);
        first = 0;
        lastPacket = lastCheck = now_ms();
        waits[0] = g_stopEvent; waits[1] = g_devChangedEvent;

        for (;;) {
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, POLL_MS);
            int got = 0;
            LONG64 t;
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_OBJECT_0 + 1) { log_msg(L"Default output device changed, switching"); break; }
            hr = keepalive(&d);
            if (FAILED(hr)) { LOG_HR(L"Keep-alive render", hr); break; }
            hr = drain(&d, &conv, &got);
            if (FAILED(hr)) { LOG_HR(L"Capture", hr); break; }
            t = now_ms();
            if (got) lastPacket = t;
            else if (t - lastPacket >= IDLE_PAD_MS) pad_to_now(&conv);
            /* watchdog only when the keep-alive should guarantee packets */
            if (d.renc && t - lastPacket >= WATCHDOG_MS) { log_msg(L"No audio packets for %d s, reinitializing", WATCHDOG_MS / 1000); break; }
            if (t - lastCheck >= DEVCHECK_MS) {
                lastCheck = t;
                if (!default_still_same(&d)) { log_msg(L"Default output device changed (poll), switching"); break; }
            }
        }
        dev_close(&d);
        InterlockedExchange(&g_haveDevice, 0);
        conv.pl = conv.pr = 0.0f; conv.pos = 0.0;
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    free(conv.tmp); free(conv.out);
    CoUninitialize();
    return 0;
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

/* ------------------------------------------------------------------ */
/* Shutdown handling                                                   */
/* ------------------------------------------------------------------ */
static BOOL WINAPI ctrl_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        request_stop(2);
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        request_stop(3);
        /* the process is terminated when we return: wait for finalize */
        WaitForSingleObject(g_doneEvent, 4500);
        return TRUE;
    }
    return FALSE;
}

/* Console apps that load user32 do not get CTRL_LOGOFF/SHUTDOWN events,
   so a hidden top-level window catches WM_QUERYENDSESSION/WM_ENDSESSION. */
static LRESULT CALLBACK hidden_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_QUERYENDSESSION) return TRUE;   /* shutdown may still be cancelled */
    if (m == WM_ENDSESSION) {
        if (wp) { request_stop(3); WaitForSingleObject(g_doneEvent, 4500); }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static unsigned __stdcall window_thread(void *arg)
{
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    (void)arg;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = hidden_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"LoopbackRecHiddenWnd";
    if (!RegisterClassW(&wc)) return 1;
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"loopback_rec", WS_OVERLAPPED,
                           0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static int launched_by_double_click(void)
{
    DWORD pids[4];
    return GetConsoleProcessList(pids, 4) == 1;
}

static void pause_if_double_clicked(int dbl)
{
    if (!dbl || !g_isConsole) return;
    out_w(L"\r\nPress Enter to close this window...");
    while (1) { int ch = _getwch(); if (ch == L'\r' || ch == L'\n' || ch == 27) break; }
}

int wmain(int argc, wchar_t **argv)
{
    static WavOut wav;
    HRESULT hr;
    IMMDeviceEnumerator *notifEn = NULL;
    int notifRegistered = 0, dbl, stdinConsole;
    HANDLE writer;
    LARGE_INTEGER f;
    LONG64 tStart, lastStatus = 0;
    SYSTEMTIME st;
    DWORD mode;

    InitializeCriticalSection(&g_outLock);
    InitializeCriticalSection(&g_statusLock);
    g_isConsole = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) ? 1 : 0;
    stdinConsole = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) ? 1 : 0;
    dbl = launched_by_double_click();

    QueryPerformanceFrequency(&f);
    g_qpcFreq = f.QuadPart;

    g_stopEvent       = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_doneEvent       = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_devChangedEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_ring = (BYTE *)VirtualAlloc(NULL, RING_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_stopEvent || !g_doneEvent || !g_devChangedEvent || !g_ring) {
        log_msg(L"Fatal: out of resources");
        pause_if_double_clicked(dbl);
        return 1;
    }
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    SetConsoleTitleW(L"loopback_rec - starting");

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LOG_HR(L"CoInitializeEx", hr); pause_if_double_clicked(dbl); return 1; }

    /* output folder */
    if (argc > 1 && argv[1][0]) {
        DWORD n = GetFullPathNameW(argv[1], MAX_PATH, wav.folder, NULL);
        if (n == 0 || n >= MAX_PATH) { log_msg(L"Invalid output folder: %ls", argv[1]); goto fatal; }
    } else {
        PWSTR docs = NULL;
        hr = SHGetKnownFolderPath(&MY_FOLDERID_Documents, 0, NULL, &docs);
        if (FAILED(hr)) { LOG_HR(L"SHGetKnownFolderPath(Documents)", hr); goto fatal; }
        _snwprintf(wav.folder, MAX_PATH - 1, L"%ls\\Recordings", docs);
        wav.folder[MAX_PATH - 1] = 0;
        CoTaskMemFree(docs);
    }
    {
        size_t L = wcslen(wav.folder);
        while (L > 3 && (wav.folder[L - 1] == L'\\' || wav.folder[L - 1] == L'/')) wav.folder[--L] = 0;
    }
    {
        int r = SHCreateDirectoryExW(NULL, wav.folder, NULL);
        if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) {
            LOG_HR(L"Creating output folder", HRESULT_FROM_WIN32((DWORD)r)); goto fatal;
        }
    }
    GetLocalTime(&st);
    _snwprintf(wav.base, 63, L"loopback_%04u-%02u-%02u_%02u-%02u-%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    wav.base[63] = 0;
    wav.part = 1;
    wav.h = INVALID_HANDLE_VALUE;
    if (!wav_open(&wav)) goto fatal;

    /* device-change notifications (capture thread also polls as a backstop) */
    hr = CoCreateInstance(&MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &MY_IID_IMMDeviceEnumerator, (void **)&notifEn);
    if (SUCCEEDED(hr)) {
        hr = IMMDeviceEnumerator_RegisterEndpointNotificationCallback(notifEn, &g_nc);
        if (SUCCEEDED(hr)) notifRegistered = 1;
        else LOG_HR(L"RegisterEndpointNotificationCallback (polling instead)", hr);
    } else LOG_HR(L"CoCreateInstance(notification enumerator)", hr);

    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    { HANDLE wt = (HANDLE)_beginthreadex(NULL, 0, window_thread, NULL, 0, NULL); if (wt) CloseHandle(wt); }

    g_start100ns = now_100ns();
    tStart = now_ms();
    g_captureThread = (HANDLE)_beginthreadex(NULL, 0, capture_thread, NULL, 0, NULL);
    if (!g_captureThread) { log_msg(L"Fatal: cannot create capture thread"); wav_close(&wav); goto fatal; }
    writer = (HANDLE)_beginthreadex(NULL, 0, writer_thread, &wav, 0, NULL);
    if (!writer) {
        log_msg(L"Fatal: cannot create writer thread");
        request_stop(4);
        WaitForSingleObject(g_captureThread, INFINITE);
        wav_close(&wav);
        goto fatal;
    }

    log_msg(L"Recording system audio. Press Q or Enter (or Ctrl+C) to stop.");

    /* UI loop */
    while (WaitForSingleObject(g_stopEvent, 200) != WAIT_OBJECT_0) {
        LONG64 t = now_ms();
        if (stdinConsole) {
            while (_kbhit()) {
                int ch = _getwch();
                if (ch == L'q' || ch == L'Q' || ch == L'\r' || ch == L'\n' || ch == 27) request_stop(1);
            }
        }
        if (t - lastStatus >= (g_isConsole ? 1000 : 10000)) {
            WCHAR line[600], title[256], dev[256], fmt[128];
            LONG64 el = (t - tStart) / 1000;
            double audioSec = (double)atomic_read64(&g_framesOut) / OUT_RATE;
            double mb = (double)atomic_read64(&g_bytesWritten) / (1024.0 * 1024.0);
            lastStatus = t;
            EnterCriticalSection(&g_statusLock);
            wcscpy(dev, g_devName); wcscpy(fmt, g_fmtDesc);
            LeaveCriticalSection(&g_statusLock);
            _snwprintf(line, 599, L"%ls%02d:%02d:%02d | %.1f MB | %ls%ls%ls | drift %+.2fs | reconn %ld%ls",
                       g_isConsole ? L"\r" : L"",
                       (int)(el / 3600), (int)(el / 60 % 60), (int)(el % 60), mb,
                       dev, fmt[0] ? L" | " : L"", fmt,
                       audioSec - (t - tStart) / 1000.0, (long)g_reconnects,
                       g_isConsole ? L"   " : L"\r\n");
            line[599] = 0;
            if (g_isConsole && wcslen(line) > 118) { line[115] = L'.'; line[116] = L'.'; line[117] = L' '; line[118] = 0; }
            EnterCriticalSection(&g_outLock);
            out_w(line);
            g_statusOnLine = g_isConsole;
            LeaveCriticalSection(&g_outLock);
            _snwprintf(title, 255, L"REC %02d:%02d:%02d - %.0f MB - loopback_rec",
                       (int)(el / 3600), (int)(el / 60 % 60), (int)(el % 60), mb);
            title[255] = 0;
            SetConsoleTitleW(title);
        }
    }

    log_msg(L"Stopping, finalizing file...");
    WaitForSingleObject(g_captureThread, 10000);
    WaitForSingleObject(writer, 15000);
    CloseHandle(writer);
    SetEvent(g_doneEvent);

    if (notifRegistered) IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(notifEn, &g_nc);
    SAFE_RELEASE(notifEn, IMMDeviceEnumerator);
    SetThreadExecutionState(ES_CONTINUOUS);
    {
        double sec = (double)g_framesOut / OUT_RATE;
        log_msg(L"Done. %.1f MB, %d:%02d:%02d of audio (%.1f s synthesized silence, %.1f s dropped, %ld discontinuities, %ld reconnects).",
                (double)g_bytesWritten / (1024.0 * 1024.0), (int)(sec / 3600), (int)sec / 60 % 60, (int)sec % 60,
                (double)g_framesPadded / OUT_RATE, (double)g_framesDropped / OUT_RATE,
                (long)g_discont, (long)g_reconnects);
        EnterCriticalSection(&g_statusLock);
        log_msg(L"Last file: %ls", g_curPath);
        LeaveCriticalSection(&g_statusLock);
        if (g_writeError) log_msg(L"Recording stopped because of a write error (disk full?).");
    }
    CoUninitialize();
    if (g_stopReason != 3) pause_if_double_clicked(dbl);
    return g_writeError ? 2 : 0;

fatal:
    SetEvent(g_doneEvent);
    if (notifRegistered) IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(notifEn, &g_nc);
    SAFE_RELEASE(notifEn, IMMDeviceEnumerator);
    CoUninitialize();
    pause_if_double_clicked(dbl);
    return 1;
}
