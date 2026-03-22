<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/logo.png" width=256 height=256> <img src="https://github.com/Darkone83/XbDiag/blob/main/img/DC%20logo.png" width=256 height=256>
</div>

<a href="https://discord.gg/k2BQhSJ"><img src="https://github.com/Darkone83/ModXo-Basic/blob/main/Images/discord.svg"></a>

# XbDiag

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/main.png">
</div>

**A hardware diagnostic suite for the original Xbox console.**

XbDiag is a native RXDK application that runs directly on original Xbox hardware (revisions 1.0 through 1.6, and debug kits). It gives you a deep look at the internal state of the machine — RAM integrity, SMBus devices, temperatures, EEPROM contents, HDD identity, video encoder, controller hardware, and DVD drive — all presented in a clean full-screen UI with no dependencies outside the Xbox kernel.

---

## Active Notes

### PAL Display Support
PAL-I (576i 50Hz) and PAL-M (480i 60Hz, Brazil) consoles are fully supported. The backbuffer is sized correctly per region — 640×576 for PAL-I, 640×480 for PAL-M and PAL60. All layout is authored in 640×480 design units and scaled at runtime via `g_sx`/`g_sy`.

PAL-I users: modes other than 576i (480p, 720p, 1080i) require an HDTV-capable AV pack and are blocked on PAL-I consoles by region policy check in VideoInfo. PAL60 (480i 60Hz on a PAL console) is treated as NTSC timing.

### CPU Stress Test — Thermal Tuning In Progress
The CPU stress kernel in `StressMath.cpp` has been updated with increased SSE pass count (`SM_SSE_OUTER = 256`) and adjusted memory flood timing (`MEM_BURST_CYCLES = 16`, `MEM_BURST_MS = 30`) to improve sustained thermal soak on upgraded systems. Real hardware validation of the new thermal delta is pending.

---

## Modules

| # | Module | Description |
|---|--------|-------------|
| 01 | **System Info** | Full hardware snapshot — CPU IC, board revision, RAM config, video mode, storage, network IP, modchip, BIOS dump |
| 02 | **Memory Test** | Quick chunk test (4 patterns, 2MB/chunk) and long-form moving-inversions soak (15 or 30 min) with per-bank visual grid |
| 03 | **SMBus Scan** | Full 0x00–0x7F address scan with live ACK/NAK grid, known-device decode, per-register read panel, and user-extensible device database |
| 04 | **Temp Monitor** | Live CPU and board temperatures via ADM1032 (rev 1.0–1.5) or PIC/Xcalibur (rev 1.6), with fan speed readback and scrolling history graph |
| 05 | **EEPROM Viewer** | Full 256-byte EEPROM decode with hex view, field editor, checksum repair, and backup restore |
| 06 | **Video Info** | Encoder type and chip ID, AV pack type, NV2A GPU/memory/pixel clocks, VRAM size, HD mod detection, NTSC/PAL color bar patterns, and live video mode switching test |
| 07 | **HDD Info** | ATA IDENTIFY — model, serial, firmware, capacity, RPM/SSD, UDMA mode, partition sizes, SMART, HDD benchmark, and DVD drive detection |
| 08 | **Controller Test** | Port connection status strip (all 4 ports), digital buttons, analog sticks, triggers, Black/White — live visualizer with stick sub-tests (dead-zone, circularity, drift, trigger dead-zone card) and rumble motor subcard |
| 09 | **Stress Test** | CPU and RAM stress tests with live temperature monitoring, fan speed readback, and configurable thermal auto-abort |
| 10 | **File Explorer** | Full file manager with FTP server, file copy/move/delete, multi-select, new folder creation, file viewer, XBE launcher, and MU Utilities (format, Skeleton Key, ENDGAME creation) |
| 11 | **Update** | GitHub OTA updater — checks latest release tag, downloads, and overwrites `D:\XbDiag.xbe` in place |
| 12 | **About** | Version info, credits, and rotating Xbox hardware facts ticker |

---

## System Info

<div align="center">
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Sysinfo.png">
</div>

Displays a full hardware snapshot captured on entry. The left column covers the CPU, memory, and chipset; the right column covers video, thermal, storage, and network.

**CPU** — IC identification (Coppermine-128), speed read directly from the MCPX CPUMPLL register (PCI Bus 0, Dev 3, Fun 0, Offset 0x6C) combined with the CPU ratio from MSR 0x2A. This gives an exact, instantaneous reading with no timing window — correct on all stock and Tualatin upgrade variants from 733 MHz through 1400 MHz. Raw CPUID leaf 1 EAX value also shown. An `[xemu]` flag is shown when the hypervisor present bit is detected.

**Memory** — total RAM size and bank configuration.

**Chipset & Board Revision** — NV2A PCI device/revision ID and the resolved board revision string. Detection follows the PrometheOS algorithm: raw PIC register 0x01 produces a 3-character string (P01/P05/P11/P2L), the video encoder SMBus probe splits P11 boards into 1.2/1.3 vs. 1.4/1.5, and NV2A EMRS strap bits further split P2L boards into 1.6 vs. 1.6b.

**Modchip** — detected via direct LPC port I/O reads at known signature addresses. Reported values include Modxo, Aladdin 1MB (Lattice and Xilinx variants), Aladdin 2MB, or None/TSOP for softmod and unmodified boards.

**Expansion Hardware** — X-RTC expansion module presence (SMBus 0x68) and I2C display detection (probed at 0x27, 0x3C, and 0x3D).

**BIOS** — version string where detectable.

**Video** — encoder type, AV pack, and current video mode.

**Thermal** — CPU die and board ambient temperatures, sourced from ADM1032 (rev 1.0–1.5) or PIC registers 0x09/0x0A (rev 1.6).

**Storage** — HDD model and capacity, UDMA mode.

**Network** — local IP address (resolved via UDP connect trick, non-blocking) and LAN MAC.

**GPU Speed** — NV2A core clock decoded from PRAMDAC NVPLL at MMIO 0xFD680500, using the correct 16.666667 MHz crystal reference.

| Button | Action |
|--------|--------|
| `[A]` | Export snapshot to `sysinfo.txt` |
| `[X]` | Open flash chip info popup |
| `[Y]` | Dump BIOS to `bios.bin` (256KB or 1MB) |
| `[B]` | Back to menu |

### Flash Chip Info Popup

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Sysinfo_flash.png">
</div>

Opened with `[X]` from the main view. Uses JEDEC autoselect to identify the TSOP flash chip — manufacturer ID, device ID, chip name, and whether the write-enable line is bridged. Two guards prevent accidental probing: the popup is suppressed when a modchip is active (LPC bus intercepted) and on rev 1.6/1.6b hardware (no TSOP present). Press `[B]` to close.

---

## Memory Test

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Ramtest.png">
</div>

### Quick Test `[A]`

Each 2MB chunk is allocated, tested with four patterns, and freed before moving to the next chunk. One chunk per frame — the display stays live throughout.

Patterns applied per chunk:
1. `0xAA55AA55` — alternating bit pairs
2. `0x55AA55AA` — inverse of pattern 1
3. Walking 1s — `1 << (i & 31)` per dword, tests each bit position independently
4. Address XOR — `i ^ 0xDEADBEEF`, detects address decoder aliasing

Each pass follows a strict write → `wbinvd` → read/verify sequence to prevent L2 cache masking real DRAM faults.

### Stress Soak `[X]` 15 min / `[Y]` 30 min

Allocates the largest contiguous block available per bank and runs memtest86-style moving inversions across the entire live allocation simultaneously.

| Phase | Operation |
|-------|-----------|
| 1/6 | WRITE forward — `0xAA55AA55` |
| 2/6 | READ+WRITE forward — verify `0xAA55`, write inverse `0x55AA` |
| 3/6 | READ+WRITE backward — verify `0x55AA`, write `0xAA55` backward |
| 4/6 | READ forward — final verify `0xAA55AA55` |
| 5/6 | WRITE — address XOR `0xDEADBEEF` |
| 6/6 | READ — verify address XOR pattern |

All adjacent cells are live simultaneously during soak — this catches coupling faults and marginal cells under sustained bus load that chunk tests cannot detect.

### Chip Help Card `[WHITE]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Ramtest_help.png">
</div>

Available at any time, including during a running test. Displays bank-to-physical-chip mapping for 64MB and 128MB configurations and a diagnosis guide for interpreting per-bank results. Press `[WHITE]` or `[B]` to return to the main view.

| Button | Action |
|--------|--------|
| `[A]` | Start quick test |
| `[X]` | Start 15-minute stress soak |
| `[Y]` | Start 30-minute stress soak |
| `[BLACK]` | Export results to `ramresult.csv` (available after test completes) |
| `[WHITE]` | Toggle chip help card |
| `[B]` | Abort running test / back to menu |

---

## SMBus Scan

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Smbusscan.png">
</div>

Performs a full scan of all 128 addresses (0x00–0x7F, software-shifted 8-bit convention matching `HalReadSMBusValue`). Results are displayed as a live ACK/NAK grid. The cursor can be moved freely across the grid; the info panel on the right updates to show device details for the selected address.

On rev 1.6 hardware a warning is shown before scanning because the Xcalibur ASIC occupies several addresses previously used by discrete components. Proceeding requires `[A]` confirmation; `[B]` cancels without scanning.

### Info Panel

The info panel shows one of four states depending on the selected address:

- **Known device** — device name, description, notes, and an `[A] Read registers` prompt if the device ACKed.
- **Unknown ACK** — confirms the address responded and offers `[A]` to probe registers 0x00–0x02.
- **Reserved address** — flags the address as reserved per the SMBus spec with a `[Y] One-shot probe` option.
- **No device** — address did not ACK or has not been scanned yet.

An ID source badge in the bottom-right of the panel indicates whether the device entry came from the built-in table or a user-loaded `smbid.id` file.

### Register Read Panel `[A]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Smbusscan_reg.png">
</div>

Opens a live register read for the selected ACK address, displaying values for registers 0x00 through the supported range. Updates every frame. Press `[B]` to close and return to the grid.

### User Device Database (`smbid.id`)

XbDiag loads `smbid.id` on entry to supplement the built-in known-device table with up to 32 user-defined entries. If the file is not present it is created automatically with a commented template. Entries follow the format:

```
0xAD | NAME | description | notes
```

Lines beginning with `#` are treated as comments. A badge in the info panel shows how many user entries are loaded.

| Button | Action |
|--------|--------|
| `[A]` | Open register read panel for selected address |
| `[B]` | Close register panel / back to menu |
| `[X]` | Re-scan all addresses |
| `[Y]` | One-shot probe of selected reserved address |
| `[DPad]` | Move cursor |

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
| X-RTC expansion module | 0x68 | 0xD0 |

---

## Temp Monitor

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Tempmonitor.png">
</div>

Displays live CPU die and board ambient temperatures. Sensor path is auto-detected on entry: ADM1032 at SMBus 0x98 for rev 1.0–1.5 boards, or PIC registers 0x09/0x0A at SMBus 0x20 for rev 1.6.

Two gauge panels show the current reading for each sensor with a color-coded indicator. Below the gauges, a scrolling line graph plots both temperatures over the last ~64 seconds (128 samples at ~500ms per sample).

**Fan Speed** — on boards where the PIC fan register (0x10) responds, the current fan duty cycle is shown as a percentage in the CPU gauge panel.

Rev 1.6 temperature readings are averaged across 10 samples to compensate for the noisier Xcalibur/PIC readout path.

| Button | Action |
|--------|--------|
| `[B]` | Back to menu |

---

## EEPROM Viewer

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Eepromview_dec.png">
</div>

### Hex View

Displays the full 256-byte EEPROM as a 16-column hex grid with field highlighting. Moving the cursor over a row highlights the corresponding named field. Press `[Right]` to switch to Decoded View.

| Button | Action |
|--------|--------|
| `[A]` | Save raw 256-byte dump to `eeprom.bin` |
| `[Right]` | Switch to Decoded View |
| `[B]` | Back to menu |

### Decoded View

Presents all parsed EEPROM fields in a scrollable list: serial number, region, HDD key, LAN MAC, confounder, video standard, video flags, audio flags, DVD region, game region, parental rating, time zone, last boot time, and checksum validity for both the factory and user sections.

| Button | Action |
|--------|--------|
| `[A]` | Save `eeprom.bin` |
| `[Left]` | Switch to Hex View |
| `[Y]` | Enter Edit View |
| `[X]` | Enter Repair View |
| `[WHITE]` | Enter Restore View (only shown in hint if `eeprom.bin` exists) |
| `[B]` | Back to menu |

### Edit View `[Y]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Eepromview_edit.png">
</div>

Four tabbed cards navigated with `[Left]` / `[Right]`:

- **VIDEO** — video standard (NTSC-M, NTSC-J, PAL), widescreen, 480p, 720p, 1080i, letterbox, PAL60
- **AUDIO** — audio mode, Dolby Digital passthrough, DTS passthrough
- **REGION** — game region, DVD region, parental rating
- **TIME** — time zone picker

Within each card, `[DPad Up/Down]` moves between fields and `[DPad Left/Right]` changes the selected value. Pressing `[A]` shows a confirmation overlay before writing; `[B]` discards changes and returns to Decoded View.

### Repair View `[X]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Eepromview_repair.png">
</div>

Scans the EEPROM for fields with corrupt or out-of-range values and categorizes each as **Repairable** or **Detect-Only**. Repairable fields can be corrected to known-good defaults; detect-only fields are flagged for information only. Pressing `[A]` shows a confirmation overlay before writing; after repair, `[A]` re-reads the live EEPROM to confirm the changes took effect. Press `[B]` to return to Decoded View without writing.

### Restore View `[WHITE]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Eepromview_restore.png">
</div>

Available when `eeprom.bin` is present. Reads the file, validates both the factory and user section checksums, and displays the result. Pressing `[A]` confirms the restore and writes the file contents back to the EEPROM; after restore, `[A]` re-reads the live EEPROM to confirm. Press `[B]` to cancel without writing.

---

## Video Info

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Videoinfo.png">
</div>

### Info View

Displays encoder type (Conexant CX25871, Focus FS454/455, or Xcalibur), chip ID, AV pack type, encoder output standard (NTSC / PAL-B/G / PAL-M / PAL-N), backbuffer resolution, real refresh rate (read from D3D display mode), and color depth.

**NV2A GPU section** reads live from MMIO:

| Field | Source | Notes |
|-------|--------|-------|
| `GPU CLK` | NVPLL @ 0xFD680500 | ~233 MHz on retail hardware |
| `MEM CLK` | MPLL @ 0xFD680504 | ~200 MHz DDR on retail hardware |
| `PIX CLK` | VPLL1 @ 0xFD680508 | ~13 MHz (480i), ~27 MHz (480p), ~74 MHz (720p) |
| `FB BASE` | PCRTC_START @ 0xFD600810 | VRAM scanout offset, shown in hex |
| `VRAM` | PFB_BOOT_0 @ 0xFD100200 | 64 MB retail / 128 MB debug kit |

Raw NVPLL and MPLL register values are displayed beneath each clock field for hardware diagnosis. A `*` suffix on a clock value indicates the MMIO read returned 0 and a hardware default was substituted.

**HD Mod / HDMI Adapter** — if a Chimeric-compatible HD mod or HDMI adapter is detected at SMBus 0x44 or 0x43, its firmware version string is shown in the right column.

| Button | Action |
|--------|--------|
| `[X]` | NTSC SMPTE RP 219 75% color bars |
| `[Y]` | PAL EBU Tech 3373 8-bar pattern |
| `[WHITE]` | Enter mode switch test |
| `[B]` | Back to menu |

### Color Bar Test Patterns

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Videoinfo_colorbars.png">
</div>

Full-screen color bar patterns for display calibration. Press `[B]` to return to the info view from either pattern.

### Mode Switch Test `[WHITE]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Videoinfo_test.png">
</div>

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

## HDD Info

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Hddinfo.png">
</div>

### Drive Info View

Displays ATA IDENTIFY data for the primary HDD: model, serial number, firmware revision, capacity (LBA28 and LBA48), UDMA mode, buffer size, ATA version, SMART support status, and rotation rate (shown as `SSD` when word 217 reports a non-rotating medium).

**Partition Sizes** — C, E, F, and G partition free/total sizes are shown once drive letters are mounted.

**Security / EEPROM** — HDD key, region, and online key sourced via the kernel, shown in the right column.

| Button | Action |
|--------|--------|
| `[A]` | Export info to `hddinfo.txt` |
| `[Right]` | Switch to SMART view |
| `[RT]` | Switch to Benchmark view |
| `[B]` | Back to menu |

### SMART View `[Right]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Hddinfo_smart.png">
</div>

Live SMART attribute table read directly from the drive via ATA command `0xB0 / 0xD0` (SMART READ DATA). Up to 30 attributes are displayed with the following columns:

| Column | Description |
|--------|-------------|
| `ID` | Attribute number in hex |
| `ATTRIBUTE` | Human-readable name, or `Attr 0xNN` for vendor-specific IDs |
| `CUR` | Current normalized value (drive-reported, 1–253) |
| `WST` | Worst recorded value since the drive was manufactured |
| `THR` | Failure threshold set by the drive manufacturer |
| `RAW VALUE` | 6-byte vendor raw counter, most-significant byte first |

#### Color Coding

Each row is colored to indicate health at a glance:

**Attribute name (ATTRIBUTE column)**
- **White** — attribute is healthy or non-critical
- **Red** — attribute is one of the three critical failure indicators (`05` Reallocated Sectors, `C5` Pending Sectors, `C6` Uncorrectable Sectors) *and* its raw value is non-zero. A non-zero raw count on any of these three attributes is a direct warning of physical media damage or imminent failure.

**Current value (CUR column)**
- **Cyan** — current value is above the manufacturer's failure threshold; attribute is passing
- **Red** — current value has reached or fallen below the threshold (`CUR ≤ THR` where `THR > 0`). This is the same condition a drive's internal SMART status reports as a predicted failure.

**Worst value (WST column)**
- Always shown in **gray** — informational only, indicates the lowest the current value has ever been.

**Threshold (THR column)**
- Always shown in **dim** — the fixed reference value set by the drive firmware.

**Raw value (RAW VALUE column)**
- **Yellow** — at least one byte of the 6-byte raw counter is non-zero. For most attributes this means the event has occurred at least once.
- **Dim** — all six raw bytes are zero; the event has never been recorded.

#### Critical Attributes to Watch

| ID | Attribute | What a non-zero raw value means |
|----|-----------|--------------------------------|
| `05` | Reallocated Sectors | Bad sectors have been remapped to spare area. Any non-zero count indicates physical platter damage. |
| `C5` | Pending Sectors | Sectors that have been read with errors and are awaiting reallocation. Drive is actively detecting new bad areas. |
| `C6` | Uncorrectable Sectors | Sectors that could not be corrected by ECC or remapped. Data loss has occurred or is occurring. |

If any of these three appear in red, the drive should be considered suspect regardless of whether the overall SMART status is still "passing."

| Button | Action |
|--------|--------|
| `[A]` | Export to `smart.txt` |
| `[Left]` | Return to Drive Info view |
| `[B]` | Back to menu |

### HDD Benchmark `[RT]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Hddinfo_bench.png">
</div>

Sequential write, sequential read, buffer read, and random seek test against a temporary file on `E:\` (falls back to ``). On drives identified as SSDs, the sequential seek test is replaced with a 4K random read test reporting both MB/s and IOPS. Results are displayed as MB/s bars with numeric readouts.

| Button | Action |
|--------|--------|
| `[A]` | Confirm and start benchmark / export results to `hddbench.txt` when done |
| `[B]` | Cancel in-progress benchmark / return to Drive Info |
| `[Left]` | Return to Drive Info view |

---

## Controller Test

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Controllertest.png">
</div>

 
### Port Status Strip

A live strip at the top of the screen shows connection status for all four controller ports. Connected ports are highlighted in green; disconnected ports are dimmed. Updates every frame — hotplug is reflected immediately.

### Button / Stick Visualizer

Displays all digital buttons (A, B, X, Y, Black, White, Start, Back, D-pad, thumbstick clicks), both analog sticks with live position dot, and trigger pressure bars. Analog button pressure is shown as a numeric value below each button.

| Button | Action |
|--------|--------|
| `[START+B]` | Back to menu |
| `[START+DPad Up]` | Enter Stick Test |
| `[START+A]` | Enter Rumble subcard |

### Stick Test `[START+DPad Up]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Controllertest_dead.png">
</div>

Four sub-tests selectable with `[DPad Left]` / `[DPad Right]`:

- **Dead-zone** — raw XY scatter plot with configurable dead-zone ring
- **Circularity** — traces the stick gate path vs an ideal circle. `[X]` clears the trace.
- **Drift** — samples at-rest position over ~3 seconds and reports average XY offset for both sticks. `[X]` resets the sample buffer.
- **Triggers** — dedicated trigger dead-zone card (see below)

Press `[B]` to exit back to the main visualizer.

### Trigger Dead-Zone Card

Two large vertical bars display live LT and RT pressure (0–255). The bottom portion of each bar is highlighted in red to show the dead-zone band (threshold: 30/255). Squeeze each trigger from rest to record min/max values for the session.

| Indicator | Meaning |
|-----------|---------|
| **PASSES DZ** (green) | Recorded minimum was inside dead-zone; maximum broke through cleanly |
| **IN DZ** (red) | Maximum never exceeded the threshold — trigger does not actuate fully |

`[X]` resets the min/max stats. Stats also reset on re-entry to the Stick Test card.

### Rumble Subcard `[START+A]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Controllertest_rumble.png">
</div>

Dedicated subcard for testing the left (low-frequency) and right (high-frequency) rumble motors independently.

| Button | Action |
|--------|--------|
| `[LT]` | Set left motor intensity |
| `[RT]` | Set right motor intensity |
| `[B]` | Exit subcard |

---

## Stress Test

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Stress_CPU.png">
</div>

**Retro Fighter: Hunter Notes** -- This controller has a firmware-level issue that, when holding down Back+b, the controller will reset. This is not the fault of the app but a FW-related issue on the dongle that Retro Fighters will need to correct.

Two test cards — CPU and RAM — navigated with `[DPad Left]` / `[DPad Right]`. Each card shares the same thermal threshold and abort controls.

### Starting a Test

Before the test begins, a threshold picker allows adjusting the thermal abort limit in 5°C steps using `[LT]` / `[RT]`. Press `[A]` to proceed to a final confirmation overlay, then hold `[LT+RT]` simultaneously to start. Press `[B]` at any point during setup to cancel.

### CPU Stress `[A]`

Sustained FPU burn using a dual-chain SSE1 `MULPS`/`ADDPS` kernel targeting both FP execution ports simultaneously. A 16KB working set fits entirely in L1 cache. A secondary x87 FFT pass and integer chain run alongside to exercise all three execution domains. Live CPU and board temperatures are shown throughout with a scrolling history graph, min/max statistics, and fan speed readback (where PIC register 0x10 responds). A CPU load bar shows core utilization.

### RAM Stress `[Right]`

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Stress_RAM.png">
</div>

Allocates the largest available contiguous block per bank and runs 11-phase moving-inversions and pattern passes across the entire live allocation simultaneously. The current phase label is shown above the chunk grid. Phases include moving inversions, checkerboard (0xAAAAAAAA/0x55555555) forward and inverted, and stride-31 address XOR patterns. Temperatures and fan speed are monitored throughout.

| Phase | Operation |
|-------|-----------|
| 1/11 | WRITE fwd — `0xAA55AA55` |
| 2/11 | RD+WR fwd — verify / write inverse |
| 3/11 | RD+WR bwd — verify / write forward |
| 4/11 | READ fwd — final verify `0xAA55AA55` |
| 5/11 | WRITE fwd — addr XOR `0xDEADBEEF` |
| 6/11 | READ fwd — verify addr XOR |
| 7/11 | WRITE fwd — checkerboard `0xAA/0x55` |
| 8/11 | RD+WR bwd — verify checker / write inverted |
| 9/11 | READ fwd — verify inverted checker |
| 10/11 | WRITE stride-31 — addr XOR `0xBAADF00D` |
| 11/11 | READ stride-31 — verify |

### Thermal Auto-Abort

Adjustable in 5°C steps before starting. If the hottest sensor (CPU die or board) reaches the threshold, the test halts immediately and the result is flagged as a thermal abort.

### Aborting a Running Test

Hold `[Back+A]` simultaneously for 5 seconds to abort while a test is in progress.

---

## File Explorer

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Fileexplorer.png">
</div>

A full single-pane file manager for navigating and managing files on your Xbox HDD partitions.

### Navigation

| Button | Action |
|--------|--------|
| `[DPad Up/Down]` | Move cursor |
| `[LT / RT]` | Page up / page down |
| `[A]` | Enter directory / open drive |
| `[B]` | Go up one level (at drive list returns to menu) |
| `[X]` | Launch selected XBE via XLaunchNewImage |
| `[R3]` | Create new folder (opens virtual keyboard to enter name) |
| `[L3]` | Open selected `.txt` or `.csv` file in the file viewer |
| `[Start]` | Toggle FTP server on / off |

### File Operations

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Fileexplorer_ops.png">
</div>

| Button | Action |
|--------|--------|
| `[Y]` | Mark / unmark item (multi-select, cursor auto-advances) |
| `[Black]` | If items marked: copy to clipboard. If clipboard loaded: paste here |
| `[White]` | If items marked: cut to clipboard. If clipboard loaded: move here |
| `[B]` | If items marked: confirm delete prompt. If nothing marked: go up |
| `[Back]` | Cancel delete prompt / clear marks and clipboard |

Marked items are shown in **green**. Marks persist while you navigate to your destination — mark your files, navigate to the target folder, then paste.

Copy and move operations run as a tick-driven background task so large transfers show a live progress widget and keep the UI responsive throughout.

### File Viewer `[L3]`

Opens `.txt` and `.csv` files directly in the explorer. The viewer reads up to 512KB, wraps long lines automatically, and displays a scroll bar and line counter. A truncation warning is shown if the file exceeds the 512KB limit.

| Button | Action |
|--------|--------|
| `[DPad Up/Down]` | Scroll one line |
| `[LT / RT]` | Page up / page down |
| `[B]` | Close and return to explorer |

### Memory Unit Support

Plugged-in Memory Units appear in the drive list as lettered drives (A–H, mapped by controller port and slot). Insertion and removal are detected every frame — the drive list reloads automatically on hotplug. Memory Units also appear as selectable destinations in the copy/move destination picker alongside HDD partitions.

Navigating to a Memory Unit entry in the drive list and pressing `[Back+Black]` opens the **MU Utilities** card — a three-item modal menu:

| Item | Action |
|------|--------|
| **Format** | Dismount, format as FATX, and remount the MU. A confirmation overlay appears before any data is erased. |
| **Create Skeleton Key** | Write a Skeleton Key image to the MU. If `D:\resources\SK.xba` is not present, offers to download it from the update server first. |
| **Create ENDGAME** | Write an ENDGAME image to the MU. If `D:\resources\ENDGAME.xba` is not present, offers to download it from the update server first. |

Navigate the card with `[DPad Up/Down]`, confirm with `[A]`, or cancel with `[B]`.

#### Skeleton Key / ENDGAME Flow

Both operations follow the same sequence:

1. **Asset check** — if the `.xba` archive is not present on `D:\resources\`, a download prompt is shown. Pressing `[A]` downloads the file from `darkone83.myddns.me:8008` with a live KB progress bar. Press `[B]` to cancel.
2. **Confirm** — once the asset is present, a confirmation overlay shows the target MU label and warns that all data will be erased. Press `[A]` to proceed, `[B]` to cancel.
3. **Create** — the MU is formatted, the `.xba` archive is extracted to a temporary directory on `D:\resources\tmp\`, all files are copied to the MU root, and the temp directory is cleaned up. A file-count progress bar is shown throughout.
4. **Result** — success or a two-line error message (error type / detail). Press `[B]` to close.

The `.xba` assets are retained in `D:\resources\` after creation so subsequent writes to additional MUs do not require a re-download.

### FTP Server

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/Fileexplorer_FTP.png">
</div>

- **Credentials:** xbox / xbox
- **Port:** 21
- **Mode:** Passive (PASV) only
- **Supported commands:** USER, PASS, SYST, TYPE, FEAT, PWD, CWD, CDUP, LIST, NLST, RETR, STOR, DELE, MKD, RMD, RNFR, RNTO, SIZE, OPTS, NOOP, QUIT
- Fully non-blocking — polled every frame, never stalls the UI
- Compatible with FlashFXP, FileZilla, and standard FTP clients

---

## Update

XbDiag includes a built-in OTA updater that fetches the latest release from a self-hosted update server over raw TCP HTTP/1.0 and writes it directly to the directory XbDiag was launched from.

1. Connects to `darkone83.myddns.me:8008` and reads `/xbdiag/XbDiag.ver` to check the latest version tag
2. If a newer version is available, displays the tag, fetches the remote changelog, and prompts for confirmation
3. Downloads `default.xbe` first, then overwrites the local copy; `XbDiag.ver` is written last to confirm a complete update
4. Reports success or failure with a status message; a `[X] Download anyway` option bypasses the version check
5. On successful update XbDiag relaunches automatically

A background boot check runs silently at startup — if a newer version is detected the app jumps directly to the Update screen instead of the main menu.

Requires a network connection and DHCP. Reports `NO LINK` if no IP address is available at entry.

| Button | Action |
|--------|--------|
| `[A]` | Confirm update and begin download |
| `[B]` | Cancel and return to menu |

---

## About

<div align=center>
<img src="https://github.com/Darkone83/XbDiag/blob/main/img/About.png">
</div>

Displays version information and two credit logo cards. A cracktro-style rainbow wave scroller runs below the modules list, crediting contributors with a per-character sine wave and cycling rainbow colour effect. A rotating Xbox hardware trivia ticker is shown above the bottom bar, fading between facts automatically. Press `[X]` or `[Y]` to cycle through facts manually.

| Button | Action |
|--------|--------|
| `[X] / [Y]` | Cycle facts |
| `[B]` | Back to menu |

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
eeprom.bin     (written by EEPROM Viewer export / restore)
hddinfo.txt    (written by HDD Info export)
sysinfo.txt    (written by System Info export)
smart.txt      (written by HDD Info SMART export)
hddbench.txt   (written by HDD Info benchmark export)
bios.bin       (written by System Info BIOS dump)
ramresult.csv  (written by Memory Test export)
smbid.id       (created automatically if not present; user-editable SMBus device database)
```

MU Utilities asset cache (downloaded on first use, retained for subsequent writes):

```
D:\resources\SK.xba       (Skeleton Key archive — downloaded from update server on demand)
D:\resources\ENDGAME.xba  (ENDGAME archive — downloaded from update server on demand)
D:\resources\tmp\         (temporary extraction directory — created and cleaned up per operation)
```

---

## Physical LCD Display (Optional)

XbDiag supports an optional US2066-compatible 20×4 character OLED connected to the Xbox SMBus at address 0x3C (8-bit 0x78). Detection is automatic — if no display is found at startup the module has no effect on operation.

When present, the display shows live system data cycling through four pages every 5 seconds:

| Page | Content |
|------|---------|
| Thermal | CPU temp, board temp, fan % |
| Clocks | CPU MHz, GPU MHz, board revision, modchip |
| Storage | HDD model, size, UDMA mode |
| Network | IP address, MAC address |

A splash screen showing the XbDiag version and Team Resurgent / Darkone83 credits is shown at startup until SysInfo data is loaded.

When the FTP server is active, the display locks to an FTP status page mirroring the on-screen overlay: connection state, IP:21, current filename, and a live progress bar (determinate for GET, animated for PUT).

**Key combos** (hold all three simultaneously):

| Combo | Action |
|-------|--------|
| START + A + WHITE | Enable display, re-initialise hardware |
| START + A + BLACK | Disable display (sends HD44780 display-off command) |

---

## Known Limitations

- **Rev 1.6 temperatures** are read via PIC registers (0x09/0x0A). Xcalibur readings are noisier than ADM1032 — values are averaged across 10 samples to compensate.
- **EEPROM export** writes to the title directory. If the directory is read-only the export will silently fail and the status indicator on screen will show `FAIL`.
- **Flash chip detection** is suppressed when a modchip is active (LPC bus intercepted) and on rev 1.6/1.6b hardware (no TSOP present).

- **LBA48 capacity** is displayed correctly for drives over 137GB. Drives over 2TB will display a `+` suffix indicating the upper 32 address bits are non-zero.
- **Video mode switching** (`[WHITE]` in Video Info) switches the D3D device via `Reset()`. Modes unsupported by the connected AV pack will be rejected silently by the NV2A encoder — the hardware verify readout will show `MISMATCH` in that case. The original mode is always restored cleanly on exit.
- **FTP passive mode only** — active mode (PORT) is not supported. Configure your FTP client to use passive mode.
- **OTA Update** connects over plain HTTP to `darkone83.myddns.me`. No TLS certificate verification is performed. Do not use on untrusted networks.

---

## Credits

Built by **Darkone83** / **Darkone Customs**.

Additional credits:

Team Resurgent x Equinox [PrometheOS]  
Rocky5 [XBMC4Gamers] — referenced for compatibility  
Ernegien [XboxEepromEditor] — referenced for EEPROM repair utility and validation

These sources were referenced for various functions throughout XbDiag to ensure hardware compatibility.