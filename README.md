<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/logo.png" width=256 height=256> <img src="https://github.com/Darkone83/XbDiag/blob/main/img/DC%20logo.png" width=256 height=256>
</div>

<a href="https://discord.gg/k2BQhSJ"><img src="https://github.com/Darkone83/ModXo-Basic/blob/main/Images/discord.svg"></a>

# XbDiag

**A hardware diagnostic suite for the original Xbox console.**

XbDiag is a native RXDK application that runs directly on an original Xbox hardware (revisions 1.0 through 1.6, and debug kits). It gives you a deep look at the internal state of the machine — RAM integrity, SMBus devices, temperatures, EEPROM contents, HDD identity, video encoder, and controller hardware — all presented in a clean full-screen UI with no dependencies outside the Xbox kernel.

---

## Modules

| # | Module | Description |
|---|--------|-------------|
| 01 | **System Info** | Full hardware snapshot — CPU, RAM config, video mode, storage, network IP, board revision |
| 02 | **Memory Test** | Quick chunk test (4 patterns, 2MB/chunk) and long-form moving-inversions soak (15 or 30 min) with per-bank visual grid |
| 03 | **SMBus Scan** | Full 0x00–0x7F address scan with live ACK/NAK grid, known-device decode, and per-register read panel |
| 04 | **Temp Monitor** | Live CPU and board temperatures via ADM1032 (rev 1.0–1.5) or PIC/Xcalibur (rev 1.6), with scrolling history graph |
| 05 | **EEPROM Viewer** | Full 256-byte EEPROM decode — serial number, region, HDD key, LAN MAC, confounder, checksum, and binary export |
| 06 | **Video Info** | Encoder type and chip ID (Conexant/Focus/Xcalibur), AV pack type, backbuffer resolution, refresh rate |
| 07 | **HDD Info** | ATA IDENTIFY — model, serial, firmware revision, capacity (LBA28/LBA48), UDMA mode, security lock state, export to TXT |
| 08 | **Controller Test** | Digital buttons, analog sticks, triggers, Black/White — live visualizer plus a dedicated rumble motor subcard |
| 09 | **About** | Version info, credits, fun Xbox hardware facts ticker |

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

---

## File Layout on the Xbox

Place the built `default.xbe` and the `tex\` folder together in the same directory on your Xbox HDD or disc image.

```
\
  default.xbe
  tex\
    xb.dds      (XbDiag logo, shown in top bar on every screen)
    tr.dds      (Team Resurgent credit logo)
    dc.dds      (Darkone Customs credit logo)
```

Diagnostic output files written by the tool

```
\eeprom.bin   (written by EEPROM Viewer export)
\hddinfo.txt  (written by HDD Info export)
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
Allocates the largest contiguous block available per bank (up to the full bank size) and runs memtest86-style moving inversions across the entire live allocation simultaneously. Phases:

| Phase | Operation |
|-------|-----------|
| 1/6 | WRITE forward — `0xAA55AA55` |
| 2/6 | READ+WRITE forward — verify `0xAA55`, write inverse `0x55AA` |
| 3/6 | READ+WRITE backward — verify `0x55AA`, write `0xAA55` backward |
| 4/6 | READ forward — final verify `0xAA55AA55` |
| 5/6 | WRITE — address XOR `0xDEADBEEF` |
| 6/6 | READ — verify address XOR pattern |

All adjacent cells are live simultaneously during soak — this catches coupling faults and marginal cells under sustained bus load that chunk tests cannot detect. Errors are bucketed back to the 2MB display grid for reporting.

---

## SMBus Address Reference

XbDiag uses software-shifted 8-bit addresses (hardware 7-bit address left-shifted by 1), matching the convention used by `HalReadSMBusValue` / `HalWriteSMBusValue`.

| Device | HW addr (7-bit) | SW addr (8-bit) |
|--------|-----------------|-----------------|
| PIC16L / SMC | 0x10 | 0x20 |
| Conexant video encoder | 0x45 | 0x8A |
| Focus FS454/455 video encoder | 0x6A | 0xD4 |
| Xcalibur video encoder (1.6) | 0x70 | 0xE0 |
| ADM1032 temp monitor (1.0–1.5) | 0x4C | 0x98 |
| EEPROM | 0x54 | 0xA8 |
| ICS clock generator | 0x69 | 0xD2 |

---

## Known Limitations

- **Rev 1.6 temperatures** are read via PIC registers (0x09/0x0A). Xcalibur readings are noisier than ADM1032 — values are averaged across 10 samples to compensate.
- **EEPROM export** writes `eeprom.bin`. If the title directory is read-only (e.g. running from a disc image without a writable D: mount) the export will silently fail — the status indicator on screen will show `FAIL`.
- **xemu compatibility**: The PIC SMBus device (0x20) may not respond in xemu. The Video Info and Temp Monitor screens handle this gracefully and show a note on screen. All other modules work normally.
- **LBA48 capacity** is displayed correctly for drives over 137GB. Drives over 2TB will display a `+` suffix indicating the upper 32 address bits are non-zero, but the displayed sector count is capped to the lower 32 bits.

---

## Credits

Built by **Team Resurgent** and **Darkone83**.

SMBus address conventions and HAL usage cross-referenced against [PrometheOS](https://github.com/Team-Resurgent/PrometheOS) by Team Resurgent, the authoritative reference for Xbox kernel HAL usage.
