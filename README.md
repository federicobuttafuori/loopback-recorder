# loopback_rec

Records everything Windows plays on the **default output device** (for example browser tab audio from Edge or Chrome during long video calls) to a WAV file. It uses WASAPI loopback capture: plain Win32 and COM, no third-party libraries.

Since v1.4.0 the GUI can also record **just one app** (for example only the browser, or only Teams) instead of the whole system: see [v1.4.0](#v140--choose-the-audio-source-whole-system-or-one-app).

There are two front-ends built on the same capture engine:

| File | What it is |
|---|---|
| `loopback_rec_gui.exe` / `loopback_rec_gui.c` | **Window app** (pure Win32/GDI, Italian UI, no console window) |
| `loopback_rec.exe` / `loopback_rec.c` | Console version (status line in a terminal) |

## GUI app (`loopback_rec_gui.exe`)
- **Main button** toggles **"● Avvia registrazione"** and **"■ Ferma e salva"**. **Ctrl+Alt+R** is a global hotkey that starts and stops recording even when the window isn't focused; the bottom line shows whether the hotkey is available.
- **"Pausa" / "Riprendi"** (next to it, or global hotkey **Ctrl+Alt+P**). While paused, nothing is written and no silence is synthesized. The timer freezes, the dot turns steady grey, the status reads **PAUSA**, and the title reads "PAUSA 01:23:45 - Loopback Rec". Resuming continues the same file seamlessly: the capture timeline origin is moved forward by the paused time, so the pause is simply not in the recording. Stopping while paused works normally.
- A red dot blinks next to a large **HH:MM:SS** timer (recorded time, pauses excluded, including time recorded before a resume). While recording, the window title (and the taskbar entry) reads **"● REC 01:23:45 - Loopback Rec"**.
- A **level meter** shows the peak of the last ~100 ms in dBFS (green, then yellow above -12 dB, red above -3 dB), so you can see the browser audio is actually being captured.
- The **status line** while recording with live compression shows **"Dimensione: ~X MB (compressi) · ~Y MB/h · parte N · liberi: …"**. X is the real size of the .m4a parts on disk, including the sample tables still to be written. Y is the measured rate of this run; for the first 5 s it shows the nominal rate. In "dopo" mode it shows the device, the WAV MB and free space. It turns **orange below 2 GB free**. **Below 300 MB free** it stops and saves cleanly, then shows a message.
- The **info line** shows the device, reconnect count and drift against the wall clock, or the pause hint. After a stop it shows the last file.
- **File name:**
  - The **"Nome file"** box shows the name the recording will get. The extension (`.m4a`, or `.wav` with compression off) is added automatically.
  - Before you start, the name is rendered from the **"Modello nome"** template and kept up to date until you type in it. **During the recording you can edit it freely.**
  - When the recording stops, every output file is renamed to whatever is in the box: `Nome.m4a`, `Nome_part2.m4a`, … This happens after compression, in the background.
  - **Sanitizing:** invalid characters (`<>:"/\|?*`) become `-`; spaces and dots are trimmed; a typed `.m4a`/`.wav` is dropped; the name is limited to 120 characters; reserved names like `CON` get a `_` prefix.
  - **No overwriting:** if any target exists, the whole set gets ` (2)`, ` (3)`, … (for example `Nome (2).m4a`, `Nome (2)_part2.m4a`). If the box is empty, the internal `loopback_YYYY-MM-DD_HH-MM-SS` name is kept.
- **"Modello nome"** (editable combo): `{data} {ora}` (default), `Chiamata {data} {ora}`, `Riunione - {titolo} - {data}`, `{titolo} {data}`, `Lezione {data}`, and **"Personalizzato…"**, which puts your last custom pattern in the box to edit. You can also just type any pattern.
  - Tokens: `{data}` = YYYY-MM-DD, `{ora}` = HH-MM, `{giorno}` = weekday in Italian (lunedì…), `{titolo}` = the placeholder **"Titolo"**, `{n}` = a counter that restarts at 1 every day and advances when a recording starts with a `{n}` template.
  - Picking a `{titolo}` preset from the list selects "Titolo" in the name box, so you just type the title.
  - The template combo is disabled while recording; the name box is not.
- **"Impostazioni…"** opens a small dialog. **Changes always apply from the next recording**; the dialog says so explicitly while recording.
  - **Comprimi in AAC (.m4a)** on/off.
  - **Comprimi: "durante la registrazione"** (default, live AAC, see below) or **"dopo la registrazione"** (WAV while recording, converted after stop; the v1.2 behaviour).
  - **Salvataggio sicuro ogni** 1 / 2 / 5 / 10 / 15 min (`SegmentMinutes`, default 5). This is how often the live files are forced to the physical disk (`FlushFileBuffers`), and it bounds what a power cut or OS crash can lose. An app crash loses nothing that was already encoded (see below).
  - **Qualità**: only rates the Windows encoder accepts (96 / 128 / 160 / 192 kbps), each shown with ~MB/h and the maximum hours per file at the current size limit, e.g. "96 kbps · ~43 MB/h · max 4,2 h per file".
  - **Canali**: Stereo / Mono.
  - **Tieni anche il file WAV**: only applies to "dopo"; live mode never writes a WAV.
  - **Dimensione massima per file (MB)** (default 190, 20–4000): recordings are auto-split so no file exceeds it (the .m4a with compression on, the WAV with it off).
  - **Dividi file ogni**: No / 30 / 60 / 90 / 120 min.
- **Save folder** is shown with path ellipsis. **"Cartella…"** opens the modern folder picker; it is disabled while recording. **"Apri cartella"** opens Explorer with the last file selected.
- **Live AAC compression (default, "durante").**
  - **Design.** The writer thread feeds the 48 kHz int16 stream (downmixed for mono) into Windows' own **AAC encoder MFT** (`CLSID_AACMFTEncoder`, the same encoder the Sink Writer uses). A small built-in MP4 muxer writes each part as `ftyp | mdat | raw AAC frames…`, appended as they come. Next to it, a tiny index `<part>.m4a.lrx` holds 2 bytes per frame and is appended after every batch, data first. When a part ends, the muxer appends the `moov` box (sample tables) and patches the `mdat` size. Capture is never blocked: it still only fills the 32 MB ring buffer.
  - **Why not segments or fragmented MP4?** Re-joining finalized MF segments losslessly would need our own MP4 parser and muxer anyway, and every segment boundary would add encoder priming silence. Fragmented MP4 (`MFTranscodeContainerType_FMPEG4`) is not reliably available across Windows 10/11 editions and decoders, and an unfinished fragment is still lost. The index approach is simpler and stronger. It is one continuous encoder stream, so parts split at exact frame boundaries with no gaps. After a stop, the only work is writing the moov (milliseconds; no re-encode, no copy). After a crash, **everything already handed to `WriteFile` is recoverable**: typically all but the last ~0.1 s, or the last `SegmentMinutes` after a power cut.
  - **Splitting uses real bytes.** Before each frame is added, the exact size of the finished file is computed (header + data + moov for that frame count). If the next frame would exceed **MaxFileMB**, the part is finished and `_part2.m4a` starts. A Wine test with `--max-mb 1` produced a first part of 999,926 bytes. "Dividi file ogni" still applies (as a frame count).
  - **Fallback.** If the encoder can't be created at start (e.g. Windows "N" without the Media Feature Pack), that recording runs as WAV in "dopo" mode. If it fails mid-recording, the current .m4a part is finished and the rest continues as a WAV part, which is converted after stop.
- **Crash recovery and resumable sessions.**
  - While recording live, `%APPDATA%\LoopbackRec\session.ini` describes the session: folder, internal base name, current part, bitrate/channels, size limit, frames in finished parts, the name box (updated as you type), template and state.
  - **Closing while recording** (live mode) asks: **"Chiudi e riprendi dopo"** (default), **"Ferma e salva"** or **"Annulla"**. "Chiudi e riprendi dopo" drains the encoder, flushes the open part and its index (no moov, so it can be extended) and marks the session `suspended`. **Windows logoff/shutdown suspends the same way.** In "dopo" (WAV) mode the old "Fermare e salvare?" prompt is used.
  - **On the next launch** a suspended or crashed session (`State=active` without a running app) is shown in the main window: **"Sessione in sospeso: <nome> (01:23:45)"** (or "Registrazione interrotta, in sospeso: …"), with the buttons **"Riprendi sessione"**, **"Salva ora"** and **"Scarta…"** (with confirmation, default No). Until it is resolved, Avvia/Ctrl+Alt+R resumes it.
    - **Riprendi**: reopens the same part. Anything not covered by the index (a torn last write, a stale moov) is truncated, and new frames are appended, so the result is **one continuous file**. The timer continues from the recorded time, and the earlier parts stay in the session. A new encoder instance adds only its normal priming (~40 ms) at the splice.
    - **Salva ora**: writes the moov for the open part and renames all parts to the name box content (same rules as a normal stop).
    - **Scarta**: deletes the session's `.m4a`, `.lrx` and `.lrn` files and `session.ini`; nothing else.
  - **Orphaned live parts** (an `.lrx` with no session.ini, e.g. after deleting it) are rebuilt automatically at startup into `"<name> (recuperata).m4a"`, using the last name typed. A green **"Recuperata registrazione interrotta: …"** line and a tray notification report it.
- **AAC compression after recording ("dopo").** Each finished WAV part (including the final one) is converted in a background thread with Windows' own **Media Foundation** AAC encoder. Nothing extra is installed, and MF is loaded at runtime, so on Windows "N" editions without the Media Feature Pack the app still runs and just keeps WAVs.
  - The UI shows **"Compressione… NN%"** and then **"Salvato: name.m4a (X MB)"**, or in red "Compressione fallita, WAV mantenuto: …".
  - The encoder writes to a temporary `*.encodingN.m4a`, which is renamed only after a successful `Finalize`. The result is then **verified**: the file exists, is non-empty, and its duration (read back with `MFCreateSourceReaderFromURL`) is within max(1 s, 1 %) of the WAV. Only then is the WAV deleted (unless `KeepWav=1`). On any failure the WAV is kept and the error is logged.
  - **Bitrate:** chosen in Impostazioni (96/128/160/192 kbps, the only rates the Windows AAC encoder accepts; 64 was confirmed rejected on Windows). If a rate is rejected anyway, the app falls back to the next higher one. At 96 kbps that's ≈ 43 MB/h (mono or stereo is the same size); at 64 kbps, if accepted, ≈ 29 MB/h.
  - **Size guarantee (< MaxFileMB per .m4a, default 190 MB):** while compression is on, the recording is split into parts no longer than `MaxFileMB / (bitrate × 1.03)`. The bitrate used here is the fallback rate, so the limit holds whichever rate the encoder accepts. At 96 kbps that's about **4 h 16 min per part**, so a 5 h call gives `name.m4a` + `name_part2.m4a`. A shorter "Dividi file" interval still applies.
  - Parts are compressed **while the recording continues**.
  - **Closing while compressing** waits for it to finish ("Chiusura al termine…"). Closing again offers to abort, which keeps the remaining WAVs. On Windows **logoff/shutdown**, the WAV is finalized first, then compression is aborted and the WAV kept.
- **Tray icon** appears while recording; clicking it brings the window to the front. When the session's files are ready, a **"Registrazione salvata"** notification shows the **.m4a** name and size (or "Compressione non riuscita" with the kept WAV); clicking it opens the folder.
- **Closing while recording**: see "Crash recovery and resumable sessions" above. In "dopo" mode it asks *"Fermare e salvare?"*, and logoff/shutdown finalizes the WAV (`WM_ENDSESSION`).
- **Single instance:** launching it a second time brings the existing window to the front.
- **High-DPI:** per-monitor v2 (manifest plus `SetProcessDpiAwarenessContext`, falling back to `SetProcessDPIAware`), Segoe UI, themed controls (Common Controls v6). The exe embeds an icon and a version resource.
- **Settings** are stored in `%APPDATA%\LoopbackRec\settings.ini` (UTF-16, so non-ASCII paths work):
  ```
  [Settings]
  SaveFolder=C:\Users\<you>\Documents\Recordings
  SplitMinutes=0        ; 0 = off, any value 1..1440 is accepted
  Compress=1            ; 1 = AAC .m4a
  CompressLive=1        ; 1 = "durante" (live), 0 = "dopo" (WAV, converted after stop)
  SegmentMinutes=5      ; live mode: flush-to-disk checkpoint interval (1..60)
  AacKbps=96            ; 96/128/160/192 (other values are snapped up)
  MaxFileMB=190         ; size limit per output file, used for auto-split
  NameTemplate={data} {ora}
  NameTemplateCustom=   ; last custom pattern ("Personalizzato…")
  Mono=0                ; 1 = mono AAC (downmix L+R)
  KeepWav=0             ; 1 = keep the WAV next to the .m4a
  [Counter]
  Date=2026-10-20  N=3  ; state of the {n} daily counter
  [Window]
  X=...  Y=...          ; last window position
  ```
- **Session state** (live mode only) is in `%APPDATA%\LoopbackRec\session.ini` (deleted when a recording is saved or discarded).
- **Log** goes to `%APPDATA%\LoopbackRec\log.txt` (device changes, errors, session summaries). It rotates to `log.old.txt` above 1 MB.

### Test mode (headless, no window)
```
loopback_rec_gui.exe --test-seconds N --out <folder>
```
Test mode uses the saved settings (live compression by default) and its own session file `<folder>\test_session.ini`, so it never touches a GUI session. Flags (test only, not saved):
- `--after`: use "dopo" mode (WAV, converted after stop).
- `--segment-seconds S`: checkpoint (flush) interval in seconds instead of SegmentMinutes.
- `--pause-at S --resume-at S`: pause and resume at those seconds; logs `RESULT_PAUSE OK|FAIL audio=… expected=…` (expected = test seconds − paused time).
- `--suspend-after S`: "Chiudi e riprendi dopo" after S seconds; logs `RESULT_SUSPEND OK …`, exit 0.
- `--resume-session --test-seconds N`: resumes the suspended or crashed session in `<folder>` for N more seconds, then stops and saves with the session's name; logs `RESULT_RESUME OK parts=1 audio=… = before … + this run …`.
- `--simulate-crash S`: kills the process abruptly (`TerminateProcess`) after S seconds. Follow it with `--resume-session` or `--save-session` (logs `RESULT_SAVE_SESSION OK …`).
- `--recover-only`: rebuild orphaned `.lrx` parts in `<folder>` and exit; logs `RESULT_RECOVER …`.
- Live runs log `LIVE t=…s size=… part=… est=… MB/h` every 5 s, a `LIVE PART file=… bytes=… frames=…` per part, and `RESULT_LIVE OK|FAIL parts=… m4a_bytes=… max_part_bytes=… limit=… audio=…s expected=…s files_duration=…s kbps=… mb_per_hour=…`. Each part's duration is read back with Media Foundation and must match the frames written.
- `--fake-aac <file.adts>` is a developer hook for Wine (which has no AAC encoder): it replays pre-encoded frames instead of encoding. Don't use it on Windows.

Older flags: `--name "<text>"` (sanitized and applied exactly like the name box at stop; logs a `RESULT_NAME OK|FAIL requested="…" files=N bytes=… first=<final path>` line and fails the test if the rename didn't happen), `--max-mb M` (size limit; values below 20 are allowed here to test splitting), `--split-seconds S` (forces parts of S seconds, to test splitting and per-part conversion quickly), `--kbps K`, `--mono`, `--keep-wav`, `--no-convert`.

This records N seconds into `<folder>`, stops cleanly, waits for the AAC conversion of every part, and exits. It doesn't use the single-instance lock, so it also works while the window is open. It appends to `<folder>\test.log`, ending with a line like:
```
RESULT OK bytes=2879808 audio=15.00s peak=-14.2 dBFS (0.19498) synthesized=0.03s dropped=0.00s discontinuities=1 reconnects=0 device="Speakers (Realtek(R) Audio)" format="48000 Hz, 2 ch, 32-bit float" file=C:\temp\lt\loopback_....wav
```
With compression on, each part also logs a `CONVERT OK file=… bytes=… duration=…s wav_duration=…s kbps=… channels=… wav_deleted=…` line (or `CONVERT FAIL …`), followed by a final:
```
RESULT_M4A OK parts_ok=1 parts_failed=0 m4a_bytes=… m4a_duration=…s last=…\loopback_….m4a
```
Exit code: **0** means a file was written with no write errors and (if compression is on) every part converted and verified, **1** means it failed (see `test.log`), **3** means the folder is invalid. The exe is a GUI-subsystem program, so wait for it explicitly in PowerShell:
```
$p = Start-Process .\loopback_rec_gui.exe -ArgumentList '--test-seconds','15','--out','C:\temp\lt' -Wait -PassThru; $p.ExitCode
```
Play something in the browser during the test. A `peak` around -120 dBFS means only silence was captured.

## Capture engine (both versions)
- Captures the default render device (eConsole) in shared-mode loopback.
- Plays a **silent render stream** on the same device, so loopback keeps sending packets even when nothing else is playing.
- **Gap-free timeline.** When no packets arrive (nothing playing, device missing, reconnecting), it writes silence based on QPC wall-clock time. Packet QPC timestamps (`DATA_DISCONTINUITY`, glitches) are used to fill holes. Small clock drift is corrected only inside quiet passages, so the file stays in sync with real time over hours.
- **Device changes.** It follows default-device switches (IMMNotificationClient, plus a 2 s poll as a backstop). It recovers from `AUDCLNT_E_DEVICE_INVALIDATED` and Windows Audio service restarts with retry and backoff (250 ms up to 5 s), filling the gap with silence.
- **Fixed output format: 48 kHz, stereo, 16-bit PCM WAV.** It converts any mix format (float32/64, int16/24/32, u8; mono, stereo, 5.1 or 7.1 downmixed; any sample rate, using linear resampling).
- **Crash-safe file.** The WAV header is rewritten and the file flushed to disk every 10 s. Before the 4 GB WAV limit it rolls over to `..._part2.wav` (about 5.8 h per part).
- Capture runs in an MMCSS "Pro Audio" thread with a 500 ms WASAPI buffer. A separate writer thread drains a 32 MB lock-free ring buffer (about 170 s), so a slow disk never blocks capture.
- Keeps the PC from going to sleep while recording (`SetThreadExecutionState`).

## Build
MinGW-w64 GUI (icon, manifest and version info come from `loopback_rec_gui.rc`, `loopback_rec.ico` and `loopback_rec_gui.manifest`):
```
x86_64-w64-mingw32-windres loopback_rec_gui.rc -O coff -o loopback_rec_gui_res.o
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -mwindows -static -s -o loopback_rec_gui.exe loopback_rec_gui.c loopback_rec_gui_res.o -lole32 -lavrt -lshell32 -luser32 -lgdi32 -lcomctl32 -luuid -lmfuuid
```
MSVC GUI (untested):
```
rc /nologo loopback_rec_gui.rc
cl /nologo /O2 /W3 loopback_rec_gui.c loopback_rec_gui.res /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup ole32.lib avrt.lib shell32.lib user32.lib gdi32.lib comctl32.lib mfuuid.lib
```
`mfplat.dll` and `mfreadwrite.dll` are loaded with `LoadLibrary`, so they are not link-time dependencies.
MinGW-w64 console version (no extra DLL dependencies):
```
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -static -s -o loopback_rec.exe loopback_rec.c -lole32 -lavrt -lshell32 -luser32
```
MSVC (x64 Native Tools / Developer Command Prompt):
```
cl /nologo /O2 /W3 loopback_rec.c ole32.lib avrt.lib shell32.lib user32.lib
```

## Run (console version)
- Double-click `loopback_rec.exe`, or run `loopback_rec.exe [output_folder]`.
- The status line refreshes every second: elapsed time, MB written, device, mix format, drift against the wall clock, and reconnect count. The window title shows `REC hh:mm:ss`.
- **Stop:** press **Q**, **Enter**, **Esc** or **Ctrl+C**. Closing the console window, logging off or shutting down also finalizes the file (Windows allows about 5 s for this).

## Output
- Folder: `%USERPROFILE%\Documents\Recordings\` by default (created if missing), or the folder you pass as an argument.
- File name: `loopback_YYYY-MM-DD_HH-MM-SS.wav`, plus `_part2`, `_part3` and so on for very long sessions.
- Size: WAV is 192 KB/s, about **691 MB per hour** (the GUI compresses it to .m4a, ≈ 43 MB/h at 96 kbps). A 3 h call is about 2.1 GB and a 5 h call about 3.5 GB.

## Convert to Opus manually (console version or `Compress=0`; optional, about 29 MB/h at 64 kbps)
```
ffmpeg -i loopback_2026-10-20_09-00-00.wav -c:a libopus -b:a 64k -application audio loopback_2026-10-20_09-00-00.opus
```
For speech only, `-b:a 32k -application voip` is plenty. To join parts first: `ffmpeg -i "concat:a.wav|a_part2.wav" ...`, or use the concat demuxer.

## Known limitations
- Records **system output only**. Your **microphone is not captured**, so in a call you get the other people, not yourself (unless an app plays your mic back).
- Records only the **default** output device. Audio an app sends to a different, non-default device is not recorded. ("Tutto il sistema" mode; one-app mode captures that app on whatever device it plays to.)
- One-app mode records a whole application (all its processes), never a **single browser tab**: Windows has no API that separates tabs, because all tabs play through the same browser audio process. Use a separate browser (or profile/window of another browser) for the call if you need isolation.
- One-app mode needs Windows 10 version 2004 (build 19041) or later; on older systems it falls back to the whole system with a warning.
- DRM-protected audio (some streaming services) may come through as silence.
- If an app takes the device in **exclusive mode**, loopback stops. The recorder writes silence and reconnects when the device is released.
- Linear resampling is only used when the device mix rate isn't 48 kHz. It's fine for speech but not audiophile quality. Channels beyond 7.1 are ignored (only the first 8 are mixed).
- Drift correction inserts or drops a few ms only during quiet passages (below -60 dBFS). After a real outage the gap is filled with silence, so the timeline matches wall-clock time, not "audio only".
- Live mode: an app crash loses only audio not yet encoded and written (well under a second); a power loss or OS crash can lose up to the last `SegmentMinutes`. "Dopo" mode: a power loss can lose up to the last ~10 s of the WAV.
- The live MP4 muxer, crash recovery and resume were verified under Wine with replayed AAC frames (files checked with ffprobe/ffmpeg: valid, gap-free, decodable). The real Windows AAC encoder MFT path, and the Media Foundation read-back of live files, still need the PC test below.
- Has not been tested with MSVC. The included exes were cross-compiled with MinGW-w64. The GUI was smoke-tested under Wine (layout, start/stop, close prompt, single instance, settings, and the no-device silence path), but real loopback capture has only been verified on Windows with the console exe.

## Screenshots (Wine, v1.3.0, using the `--fake-aac` hook)

- `screenshots/gui_idle.png`: idle window ("AAC 96 kbps stereo (durante)").
- `screenshots/gui_recording.png`: live recording with "Dimensione: ~0.1 MB (compressi) · ~43 MB/h · parte 1".
- `screenshots/gui_paused.png`: paused (grey dot, PAUSA, frozen timer, "Riprendi" button).
- `screenshots/gui_settings.png`: Impostazioni with "Comprimi: durante/dopo" and "Salvataggio sicuro ogni".
- `screenshots/gui_close_dialog.png`: closing while recording ("Chiudi e riprendi dopo" / "Ferma e salva" / Cancel; Wine draws the task dialog without a frame).
- `screenshots/gui_pending.png`: next launch, "Sessione in sospeso: Riunione Budget Q4 (00:00:35)" with Riprendi sessione / Salva ora / Scarta.
- `screenshots/gui_resumed.png`: resumed; the timer continues from 00:00:35.
- `screenshots/gui_saved.png`: after stop: one continuous "Riunione Budget Q4.m4a" (41.6 s = 35.3 s + 6.3 s).
- `screenshots/gui_conv_fail_wine.png`: Wine has no AAC encoder, so the WAV is kept (expected only under Wine).

## v1.3.4 – Taglia silenzi (automatic silence cut)
- Impostazioni → "Taglia silenzi": checkbox (default OFF), "Silenzio minimo (s)" (default 3, 1–60),
  "Margine prima/dopo (ms)" (default 500, 0–5000), "Soglia silenzio (dB)" (default -45, -70..-20).
  settings.ini keys: SilenceCut, SilenceMinSec, SilencePadMs, SilenceDb. Applies from the next recording.
- Live, on the writer thread before the encoder (live AAC and WAV modes): 10 ms RMS blocks; a silent run longer
  than the minimum is removed except the margin after the last sound and the margin before the sound resumes (lookahead hold ring).
  The capture timeline (g_framesOut, drift, padding) is untouched; timer = real time; session.ini CutMs keeps the cut total across suspend/crash.
- Main window info line: "Tagliati m:ss · …" / "Silenzio: taglio in corso (tagliati m:ss) · …". log.txt gets per-cut lines and a total at stop.
- Test flags: --silence-cut / --no-silence-cut, --silence-db, --silence-min, --silence-pad, --synth S,Q (tone/silence test signal).
- Screenshot: screenshots/settings_silence.png

## v1.3.5 – silence cut settings apply live
- OK in Impostazioni during a recording applies Taglia silenzi (on/off, min, margin, threshold) to the running recording;
  other settings still apply from the next recording (dialog note: "Taglia silenzi: vale subito. Il resto: dalla prossima registrazione.").
- UI thread queues the request (g_gateReqLock + g_gateReqVer); the writer thread applies it in gate_write(). Hold ring is
  allocated at the max (60 s + 5 s, ~12.5 MB) so min/margin can change mid-run. Turning off flushes the partial block and the held
  audio (mid-cut: keeps the lead-in margin); turning on starts a fresh silence run. CutMs persists whenever the cut was used.
- Test flag: --silence-change-at S:on,db,min,pad (up to 8).

## v1.3.6 — live rename while recording + stronger silence feedback

- The file takes the chosen name **while recording**: about 1.7 s after the last edit in "Nome file", or straight away when a template is picked or at start. Open files are renamed by handle (SetFileInformationByHandle). Closed split parts and .lrn/.lrx files are moved too.
- Crash safety: session.ini records `PrevBase`+`Base` as a journal, and an interrupted rename is completed at the next load. Recovery, resume and orphan recovery all find the renamed files. Sessions started by older versions resume fine.
- If a rename fails (name taken → " (k)"; invalid, locked or longer than 55 chars), the current name is kept, the failure is logged, and the file is renamed at stop as before. Finalize never renames twice.
- WAV mode (convert after): only the open part and later parts are renamed live. Closed WAV parts get the new name at stop.
- Silence UI:
  - A blue "SILENZIO m:ss" state shows the current silence length, with a light-blue band reading "Silenzio · m:ss — non salvato · Tagliati … · Durata file ~…".
  - The window title reads "SILENZIO m:ss · REC hh:mm:ss".
  - A gray taskbar overlay appears during a cut.
  - "Tagliati · Durata file" is always shown when the cut is on, and the timer is labelled "tempo reale".
- Test option: `--rename-at S:Name` (up to 4).
- Screenshot: screenshots/silence_state.png

## v1.3.8 — pause and silence durations always visible

- Silence metering is always on while recording, also with "Taglia silenzi" off; nothing is cut unless the cut is on. It uses the SilenceDb threshold in 10 ms blocks.
  - After 1 s below the threshold, a neutral gray band shows "Silenzio · m:ss · Durata file ~…", and the window title gets "· silenzio m:ss".
  - With the cut on, the blue "non salvato" state appears once the cut actually starts.
  - When a silence ends, "Ultimo silenzio m:ss" shows for 6 s.
- Manual pause:
  - The label reads "PAUSA m:ss" and the title "PAUSA m:ss · REC …".
  - The info line reads "In pausa da m:ss · Pause totali m:ss · Durata file ~…".
  - Total pause time is saved in session.ini as `PausedMs` (on resume, at checkpoints, at part change and on suspend), so it survives suspend/resume and crashes. Sessions from ≤1.3.7 start at 0.
- The info line always shows "Durata file ~h:mm:ss" (= timer − silence cut), plus "Tagliati m:ss" and "Pause m:ss" when > 0. The big timer is labelled "timer: tempo reale" and excludes pauses, as before.
- The timer now stops when capture stops, not after the file has been finalized.
- Test mode logs `RESULT_DURATION` (UI "Durata file" vs encoded frames vs PCM written).
- Screenshots: screenshots/paused_state.png, screenshots/silence_cutoff_state.png

## v1.3.9 — level meter shows the current level + silence threshold mark

- The "Livello" bar now shows the **current** level with no slow fall-off: the loudest 10 ms RMS block of each 100 ms UI tick, in dBFS — the same value the silence rule compares with SilenceDb, so bar and threshold are directly comparable. The numeric label shows the same value ("< -70 dB" below the scale, "–" when idle/paused).
- Scale is now -70..0 dBFS (the whole SilenceDb range). A dark vertical mark with a small label (e.g. "-30") sits at the current SilenceDb; it follows the setting live and is shown whether "Taglia silenzi" is on or off. The zone left of the mark is shaded slightly darker.
- Bar colour: gray-blue when below the threshold (counts as silence), green above, yellow above -12 dB, red above -3 dB.
- RMS reads ~3 dB lower than the old peak meter for a sine (more for speech); that's expected.
- Test mode logs `METER t s: level x dBFS, threshold y -> below/above` every 0.5 s.
- `-DGUI_SYNTH_SHOT` build flag (screenshots only, never shipped) lets the GUI use `--synth`.
- Screenshots: screenshots/meter_threshold.png (tone, above), screenshots/meter_threshold_silence.png (silence, below)

## v1.3.10 — the big timer is the file length

- The big timer now shows the **length of the audio in the file**: pauses and cut silences are excluded. The "Durata file ~…" line and the "timer: tempo reale" caption are gone (no duplicate figure). Window title, tray tooltip and the pending-session line use the same value. "Tagliati m:ss" / "Pause m:ss" remain as small secondary info, plus the current silence/pause counters.
- Source: `file_ms()` = audio already in the file at resume + PCM frames handed to the encoder this run (writer thread). Silence held in the cut's lookahead is not counted until it is known to be kept, so the timer **never goes backward**: during a silence it stops after the trailing margin; if the silence ends shorter than the minimum (kept) or the sound resumes (lead-in margin kept) it catches up forward. Cut off → silence counts normally. Paused → frozen. Resumed session → continues from the file's length.
- The log keeps the timeline (real time minus pauses) for drift/MB/h; test mode logs `RESULT_DURATION … ui_timer= encoded= … backward_max=` and the timer in each `METER` line.
- Screenshots: screenshots/timer_filelen.png (silence being cut, timer stopped), timer_filelen_sound.png, timer_filelen_paused.png, timer_filelen_saved.png

## v1.3.11 — silence counter appears immediately, with tenths

- The current-silence counter appears as soon as the 10 ms level drops below SilenceDb (no 1 s delay), gray band with the cut off, blue SILENZIO when a cut is in progress.
- Under 5 s it shows tenths ("Silenzio · 0.4 s" … "4.9 s", also "SILENZIO 3.2 s" and "Ultimo silenzio 1.9 s"); from 5 s on it switches to m:ss as before.
- Anti-strobe: when a below-threshold run ends, the gray band holds its last value for 150 ms; a new run within that time restarts the counter from 0.0 (it follows the detector). "Ultimo silenzio" (6 s) is shown for runs ≥ 0.3 s.
- Window title unchanged (still adds "· silenzio m:ss" after 1 s). Timer/file-length/cut logic unchanged.
- Screenshot: screenshots/silence_tenths.png (gray 4.7 s, blue cut 2.4 s, Ultimo silenzio 1.9 s)

## v1.4.0 — choose the audio source: whole system or one app

- New **"Sorgente: … ▾"** row under the big buttons. Choices: **Tutto il sistema** (default, unchanged behaviour: loopback of the default output device) or **one app**.
- The menu lists the apps that currently have an audio session (by friendly name, e.g. "Google Chrome", "Microsoft Edge", "Microsoft Teams"; "♪ in riproduzione" marks the ones playing now), then **"Altre app aperte"** (other apps with a window, in case an app has not opened its audio yet), **"Aggiorna elenco"**, and a reminder that a single browser tab cannot be separated. One entry per app: browsers' many processes are grouped under their root process.
- Capture uses Windows **process loopback** (`ActivateAudioInterfaceAsync` on `VAD\Process_Loopback`, `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`), so the app's child processes (e.g. the browser's audio service process) are included and nothing else is: other apps, system sounds and the microphone are not recorded. Format: 48 kHz stereo 16-bit with `AUTOCONVERTPCM`; silence is synthesized when the app is quiet, so the file stays aligned with real time.
- The choice is saved in `settings.ini` (`SourceExe`, `SourceName`) by exe name and resolved to the current root process at every start. Since 1.4.1 it can also be changed while recording (see below). It is also stored in the session (`session.ini`), so **"Riprendi sessione"** continues with the same app (sessions from ≤ 1.3.x resume as whole system).
- **App not running at start:** a prompt offers *Sì* = record the whole system this time, *No* = start anyway and wait for the app (silence until it starts), *Annulla*.
- **App closed while recording:** recording continues with silence, an orange line says **"App chiusa: in attesa di X… (registra silenzio)"**, and the app is re-attached automatically when it starts again (checked every 2 s).
- **Process loopback unavailable** (older Windows): a warning is shown and the recording continues with the whole system. After the app was attached once, the recorder never silently switches to the whole system (privacy): it keeps waiting for the app.
- The window title / taskbar / tray tooltip show the source ("● REC 00:12:34 · Microsoft Edge - Loopback Rec"); the idle hint and info line too.
- Everything else is unchanged: live m4a, silence cut with live settings, pause, crash-safe resume, live rename, taskbar overlay, meter, timer = file length.
- Test flags: `--source-exe NAME` (e.g. `msedge.exe`), `--list-sources --out <folder>` (writes `sources.log` with what the picker would offer), `--force-proc-fail` (simulate missing process loopback), `--open-source-menu` (GUI: open the picker at start, for screenshots). Test mode logs `RESULT_SOURCE … state=app|waiting|fallback-system attached= exits= captured= synthesized=`. Test mode no longer rotates the GUI's `log.txt` or creates `settings.ini`.
- Tested: Wine regression of all system-mode flags (identical results to 1.3.11), waiting path, fallback path; Windows 11 25H2: 5 s whole system and 5 s Edge-only capture (timer = file length), and an app exit/re-attach test.
- Screenshot: screenshots/source_picker.png

## v1.4.1 — change the source while recording

- The **"Sorgente ▾"** picker stays enabled while recording, while paused, and on a pending (suspended/crashed) session.
- **While recording / paused:** picking another source (whole system ↔ an app, or app → another app) switches live into the **same file**. The UI only posts a request; the capture thread drains the old capture client, closes it, fills the switch gap with synthesized silence and opens the new one (system loopback or process loopback). The output format to the encoder is unchanged (the pipeline converts/resamples as usual), so timer = file length, silence cut (incl. live settings), meter, pause, live rename and live m4a keep working. A switch is not counted as a reconnect.
- The new source is written to `session.ini` immediately (a crash or "Chiudi e riprendi dopo" resumes with it) and to `settings.ini` (next recordings). Each switch is logged: `SOURCE SWITCH #n at T s of timeline (F s in file): A -> B`.
- A green line shows **"Sorgente cambiata: Google Chrome"** for 5 s (also in the paused line). If the app is not running: orange "Sorgente cambiata: X non è in esecuzione, in attesa… (registra silenzio)" and the existing wait/re-attach logic takes over. If process loopback is unavailable after a live switch, an orange line (no dialog mid-recording) says the whole system is recorded.
- **Pending session:** the choice is stored in its `session.ini`, so "Riprendi sessione" continues with the new source.
- Test flag: `--source-switch-at S:EXE` or `S:system` (up to 4). `RESULT_SOURCE` reports `switches=applied/requested`.
- Tested: Wine regression of all system-mode flags (identical to 1.4.0), switch system→app→system (fallback and `--force-proc-fail`), switch to a missing app and back, app→system, same-source (ignored), switch while paused, with silence cut, suspend+resume (resumes with the new source), crash (session.ini has the new source). Windows 11 (T480): 12 s run, system → Microsoft Edge at 4 s → system at 8 s: timer 12.055 s, encoded 12.075 s, MF-read file 12.07 s, 0 reconnects.
