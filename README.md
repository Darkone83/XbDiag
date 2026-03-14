<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/logo.png" width=256 height=256> <img src="https://github.com/Darkone83/XbDiag/blob/main/img/DC%20logo.png" width=256 height=256>
</div>

<a href="https://discord.gg/k2BQhSJ"><img src="https://github.com/Darkone83/ModXo-Basic/blob/main/Images/discord.svg"></a>

# XbDiag

**A hardware diagnostic suite for the original Xbox console.**

XbDiag is a native RXDK application that runs directly on original Xbox hardware (revisions 1.0 through 1.6, and debug kits). It gives you a deep look at the internal state of the machine — RAM integrity, SMBus devices, temperatures, EEPROM contents, HDD identity, video encoder, controller hardware, and DVD drive — all presented in a clean full-screen UI with no dependencies outside the Xbox kernel.

---

## Modules

| # | Module | Description |
|---|--------|-------------|
| 01 | **System Info** | Full hardware snapshot — CPU IC, board revision, RAM config, video mode, storage, network IP, BIOS dump |
| 02 | **Memory Test** | Quick chunk test (4 patterns, 2MB/chunk) and long-form moving-inversions soak (15 or 30 min) with per-bank visual grid |
| 03 | **SMBus Scan** | Full 0x00–0x7F address scan with live ACK/NAK grid, known-device decode, and per-register read panel |
| 04 | **Temp Monitor** | Live CPU and board temperatures via ADM1032 (rev 1.0–1.5) or PIC/Xcalibur (rev 1.6), with scrolling history graph |
| 05 | **EEPROM Viewer** | Full 256-byte EEPROM decode — serial number, region, HDD key, LAN MAC, confounder, checksum, and binary export |
| 06 | **Video Info** | Encoder type and chip ID, AV pack type, refresh rate, NV2A GPU/memory clocks, NTSC/PAL color bar patterns, and live video mode switching test |
| 07 | **HDD Info** | ATA IDENTIFY — model, serial, firmware, capacity (LBA28/LBA48), UDMA mode, SMART, HDD benchmark, DVD drive detection and read speed test |
| 08 | **Controller Test** | Port connection status strip (all 4 ports), digital buttons, analog sticks, triggers, Black/White — live visualizer plus rumble motor subcard |
| 09 | **Stress Test** | CPU and RAM stress tests with live temperature monitoring and configurable thermal auto-abort |
| 10 | **File Explorer** | Full file manager with FTP server, file copy/move/delete, multi-select, and XBE launcher |
| 11 | **About** | Version info, credits, and rotating Xbox hardware facts ticker |

---

## HDD Info

### Drive Info View
Displays ATA IDENTIFY data for the primary HDD: model, serial number, firmware revision, capacity (LBA28 and LBA48), UDMA mode, buffer size, ATA version, and SMART support status. Also shows EEPROM security data (HDD key, region, online key) sourced via the kernel.

If a DVD drive is detected on the secondary ATA channel, its model string is shown at the bottom of the info page and the DVD read speed test becomes available.

### SMART View `[Right]`
Live SMART attribute table — attribute ID, name, value, worst, threshold, and raw data for all reported attributes. Export to `D:\smart.txt` with `[A]`.

### HDD Benchmark `[RT]`
Sequential write, sequential read, and random seek test against a temporary 64MB file on `E:\` (falls back to `D:\`). Results displayed as MB/s bars with numeric readout. Export to `D:\hddbench.txt` with `[A]` after completion.

| Button | Action |
|--------|--------|
| `[A]` | Confirm and start benchmark / export results when done |
| `[B]` | Cancel in-progress benchmark |
| `[Left]` | Return to info view |

### DVD Read Speed Test `[LT]`
Available only when a DVD drive is detected on the secondary ATA channel via ATAPI IDENTIFY. Performs a timed 16MB sequential read from a disc in the drive.

| Button | Action |
|--------|--------|
| `[A]` | Check disc readiness and start test |
| `[B]` | Cancel in-progress test / return to info |

Result is shown in MB/s with a reference note (1x DVD = 1.39 MB/s; stock Xbox drives typically read 5–11 MB/s depending on disc radius).

---

## Video Info

### Info View
Displays encoder type (Conexant CX25871, Focus FS454/455, or Xcalibur), chip ID, AV pack type, encoder output standard (NTSC / PAL-B/G / PAL-M / PAL-N), backbuffer resolution, real refresh rate (read from D3D display mode), and color depth.

**NV2A GPU section** reads live from MMIO:

| Field | Source | Notes |
|-------|--------|-------|
| `GPU CLK` | NVPLL @ 0xFD680500 | ~233 MHz on retail hardware |
| `MEM CLK` | MPLL @ 0xFD680504 | ~200 MHz DDR on retail hardware |
| `PIX CLK` | VPLL1 @ 0xFD680508 | ~13 MHz (480i), ~27 MHz (480p), ~74 MHz (720p) |
| `FB BASE` | PCRTC_START @ 0xFD600810 | VRAM scanout offset, shown in hex |

Raw NVPLL and MPLL register values are displayed beneath each clock field for hardware diagnosis. A `*` suffix on a clock value indicates the MMIO read returned 0 and a hardware default was substituted.

### Color Bar Test Patterns
| Button | Pattern |
|--------|---------|
| `[X]` | NTSC SMPTE RP 219 75% color bars |
| `[Y]` | PAL EBU Tech 3373 8-bar pattern |
| `[B]` | Return to info view |

### Mode Switch Test `[WHITE]`
Cycles through the video modes supported by the connected AV pack, applying each via D3D `Reset()` with the corresponding presentation flags. Full-screen color bars are displayed in the active mode with a live hardware verification readout showing the actual backbuffer dimensions as reported by `GetBackBuffer()`. This confirms whether the GPU framebuffer committed to the requested resolution.

| Button | Action |
|--------|--------|
| `[WHITE]` | Next mode |
| `[BLACK]` | Previous mode |
| `[B]` | Restore original mode and return to info view |

Modes are filtered by AV pack type at entry:

| AV Pack | Available Modes |
|---------|----------------|
| Composite / S-Video / SCART | 480i, 576i PAL |
| VGA | 480i, 480p |
| HDTV Component | 480i, 480p, 576i PAL, 720p, 1080i |

The hardware verify readout (`HW: WxH  OK / MISMATCH`) is deferred by two frames after each switch to allow the NV2A scanout registers to settle before sampling. A `MISMATCH` result indicates the D3D Reset succeeded but the GPU framebuffer did not commit to the requested resolution — typically caused by an AV pack that cannot carry the signal.

---

## Controller Test

### Port Status Strip
A live strip at the top of the screen shows connection status for all four controller ports. Connected ports are highlighted in green; disconnected ports are dimmed. Updates every frame — hotplug is reflected immediately.

### Button / Stick Visualizer
Displays all digital buttons (A, B, X, Y, Black, White, Start, Back, D-pad, thumbstick clicks), both analog sticks with live position dot, and trigger pressure bars. Analog button pressure is shown as a numeric value below each button.

### Stick Test `[Start+DPad Up]`
Three sub-tests selectable with `[Left]` / `[Right]`:
- **Dead-zone** — raw XY scatter plot with configurable dead-zone ring
- **Circularity** — traces the stick gate path vs an ideal circle
- **Drift** — samples at-rest position over ~3 seconds and reports average XY offset

### Rumble Subcard `[A]`
Dedicated subcard for testing the left (low-frequency) and right (high-frequency) rumble motors independently, with adjustable intensity via triggers.

---

## Stress Test

### CPU Stress `[A]`
Sustained FPU/integer burn using a Prime95-derived eight-real FFT kernel. Live CPU and board temperatures shown throughout with a scrolling graph. A configurable thermal threshold (adjustable before starting) will auto-abort if the CPU exceeds the limit.

### RAM Stress `[X]`
Allocates the largest available contiguous block per bank and runs sustained moving-inversions read/write passes. Temperatures monitored throughout.

### Thermal Auto-Abort
Adjustable in 5°C steps before starting the test. If the hottest sensor (CPU die or board) reaches the threshold the test halts immediately and the result is flagged as a thermal abort.

---

## File Explorer

The File Explorer is a full single-pane file manager for navigating and managing files on your Xbox HDD partitions.

### Navigation

| Button | Action |
|--------|--------|
| `[DPad Up/Down]` | Move cursor |
| `[LT / RT]` | Page up / page down |
| `[A]` | Enter directory / open drive |
| `[B]` | Go up one level (at drive list returns to menu) |
| `[X]` | Launch selected XBE via XLaunchNewImage |
| `[Start]` | Toggle FTP server on / off |

### File Operations

| Button | Action |
|--------|--------|
| `[Y]` | Mark / unmark item (multi-select, cursor auto-advances) |
| `[Black]` | If items marked: copy to clipboard. If clipboard loaded: paste here |
| `[White]` | If items marked: cut to clipboard. If clipboard loaded: move here |
| `[B]` | If items marked: confirm delete prompt. If nothing marked: go up |
| `[Back]` | Cancel delete prompt / clear marks and clipboard |

Marked items are shown in **green**. Marks persist while you navigate to your destination — mark your files, navigate to the target folder, then paste.

Copy and move operations run as a tick-driven background task so large transfers show a live progress widget and keep the UI responsive throughout.

### FTP Server

- **Credentials:** xbox / xbox
- **Port:** 21
- **Mode:** Passive (PASV) only
- **Supported commands:** USER, PASS, SYST, TYPE, FEAT, PWD, CWD, CDUP, LIST, NLST, RETR, STOR, DELE, MKD, RMD, RNFR, RNTO, SIZE, OPTS, NOOP, QUIT
- Fully non-blocking — polled every frame, never stalls the UI
- Compatible with FlashFXP, FileZilla, and standard FTP clients

---

## Requirements

- Original Xbox console, revision 1.0 through 1.6, or a debug kit
- Modchip, softmod, or other mechanism that allows running unsigned code
- RXDK / Xbox XDK build environment (to compile from source)

---

## Building

XbDiag is built with the Xbox XDK (RXDK). Open the project in the XDK build environment and compile normally. No third-party libraries are required beyond the standard XDK headers and import libraries.

**Key build constraints:**
- Fixed-function D3D8 only — no vertex or pixel shaders
- No `__ftol2_sse` — float-to-int uses an inline x87 `Ftoi()` helper
- No CRT string functions in the hot path — all string work uses the internal helpers in `DiagCommon.cpp`
- `MmAllocateContiguousMemory` / `MmFreeContiguousMemory` used directly for RAM test
- NV2A MMIO access uses direct pointer reads, guarded by PCI vendor ID check before touching the MMIO window

---

## File Layout on the Xbox

Place the built `default.xbe` and the `tex\` folder together in the same directory on your Xbox HDD or disc image.

```
\
  default.xbe
  tex\
    xb.dds      (XbDiag logo, shown in top bar on every screen)
    dc.dds      (Darkone Customs credit logo)
    tr.dds      (Team Resurgent credit logo)
```

Diagnostic output files written by the tool:

```
D:\eeprom.bin     (written by EEPROM Viewer export)
D:\hddinfo.txt    (written by HDD Info export)
D:\sysinfo.txt    (written by System Info export)
D:\smart.txt      (written by HDD Info SMART export)
D:\hddbench.txt   (written by HDD Info benchmark export)
D:\bios.bin       (written by System Info BIOS dump)
D:\ramresult.csv  (written by RAM Test export)
```

---

## Memory Test Details

### Quick Test `[A]`
Each 2MB chunk is allocated, tested with four patterns, and freed before moving to the next chunk. One chunk per frame — the display stays live throughout.

Patterns applied per chunk:
1. `0xAA55AA55` — alternating bit pairs
2. `0x55AA55AA` — inverse of pattern 1
3. Walking 1s — `1 << (i & 31)` per dword, tests each bit position independently
4. Address XOR — `i ^ 0xDEADBEEF`, detects address decoder aliasing

Each pass follows a strict write → `wbinvd` → read/verify sequence to prevent L2 cache masking real DRAM faults.

### Stress Soak `[X]` 15 min / `[Y]` 30 min
Allocates the largest contiguous block available per bank and runs memtest86-style moving inversions across the entire live allocation simultaneously. Phases:

| Phase | Operation |
|-------|-----------|
| 1/6 | WRITE forward — `0xAA55AA55` |
| 2/6 | READ+WRITE forward — verify `0xAA55`, write inverse `0x55AA` |
| 3/6 | READ+WRITE backward — verify `0x55AA`, write `0xAA55` backward |
| 4/6 | READ forward — final verify `0xAA55AA55` |
| 5/6 | WRITE — address XOR `0xDEADBEEF` |
| 6/6 | READ — verify address XOR pattern |

All adjacent cells are live simultaneously during soak — this catches coupling faults and marginal cells under sustained bus load that chunk tests cannot detect.

---

## SMBus Address Reference

XbDiag uses software-shifted 8-bit addresses (hardware 7-bit address left-shifted by 1), matching the convention used by `HalReadSMBusValue` / `HalWriteSMBusValue`.

| Device | HW addr (7-bit) | SW addr (8-bit) |
|--------|-----------------|-----------------|
| PIC16L / SMC | 0x10 | 0x20 |
| Conexant CX25871 video encoder | 0x45 | 0x8A |
| Focus FS454/455 video encoder | 0x6A | 0xD4 |
| Xcalibur video encoder (1.6) | 0x70 | 0xE0 |
| ADM1032 temp monitor (1.0–1.5) | 0x4C | 0x98 |
| EEPROM | 0x54 | 0xA8 |
| ICS clock generator | 0x69 | 0xD2 |

---

## Known Limitations

- **Rev 1.6 temperatures** are read via PIC registers (0x09/0x0A). Xcalibur readings are noisier than ADM1032 — values are averaged across 10 samples to compensate.
- **EEPROM export** writes to the title directory (`D:\`). If the directory is read-only the export will silently fail and the status indicator on screen will show `FAIL`.
- **xemu compatibility**: The PIC SMBus device (0x20) may not respond in xemu. Video Info and Temp Monitor handle this gracefully. NV2A MMIO reads in Video Info are guarded by a PCI vendor ID check and will show `N/A` if the guard fails. CPU detection uses the hypervisor present bit (CPUID leaf 1, ECX bit 31) to distinguish xemu from real hardware. All other modules work normally.
- **LBA48 capacity** is displayed correctly for drives over 137GB. Drives over 2TB will display a `+` suffix indicating the upper 32 address bits are non-zero.
- **DVD read speed test** requires a disc to be inserted before starting. Discs with no files at the root of the filesystem will report an error.
- **Video mode switching** (`[WHITE]` in Video Info) switches the D3D device via `Reset()`. Modes unsupported by the connected AV pack will be rejected silently by the NV2A encoder — the hardware verify readout will show `MISMATCH` in that case. The original mode is always restored cleanly on exit.
- **FTP passive mode only** — active mode (PORT) is not supported. Configure your FTP client to use passive mode.

---

## Credits

Built by **Darkone83** / **Darkone Customs**.

Additional credits:

Team Resurgent x Equinox [PrometheOS] — virtual path architecture, board revision detection, and FTP server reference  
Rocky5 [XBMC4Gamers] — referenced for compatibility

These sources were referenced for various functions throughout XbDiag to ensure hardware compatibility.