# PortVector

**An e-reader firmware for the Xteink X4, built on top of CrossPoint Reader.**

PortVector is a personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — open-source firmware for the **Xteink X4** e-paper reader, built with **PlatformIO** on **ESP32-C3**.

## Hardware

| Spec | Detail |
|------|--------|
| MCU | ESP32-C3 RISC-V @ 160MHz |
| RAM | ~380KB (no PSRAM) |
| Flash | 16MB |
| Display | 800x480 E-Ink (SSD1677) |
| Storage | SD Card |
| Wireless | WiFi 802.11 b/g/n, BLE 5.0 |

---

## Features

### Reading Experience

- **EPUB 2 & 3** with image support and CSS styling
- **XTC** native pre-rendered format (supports files >2GB)
- **TXT / Markdown** with auto-wrapping and chapter detection
- **3 font families** — Bookerly, Lexend, Bokerlam
- **4 font sizes** — Small, Medium, Large, Extra Large
- **Anti-aliased grayscale** text rendering with 3 darkness levels
- **Multi-language hyphenation** support
- **4 screen orientations** with remappable buttons
- **Auto page turn** — on/off toggle + speed 1–20 pages/min
- **Reading statistics & streaks** — per-session, daily, all-time tracking
- **KOReader Sync** for cross-device reading progress
- **Bookmarks** via long-press Confirm

### Home Screen

- **Continue Reading card** — cover art + title + author + progress bar
- **Recent books grid** — 3-column cover thumbnails with progress bars
- **Bottom navigation** — Apps | Recents | Library | Settings
- **Header widgets** — clock + pet status
- **Focus mode** — hides recent books for a cleaner look

### Sleep Screens

9 modes available, configurable from the **Sleep Image** app:

| Mode | Description |
|------|-------------|
| **Dark** | Black screen (default, lowest battery) |
| **Light** | White screen |
| **Custom** | User image from `/sleep/` folder on SD card |
| **Cover** | Current book's cover art |
| **None** | Keep last screen content |
| **Cover + Custom** | Book cover with image overlay |
| **Clock** | 7-segment digital clock with calendar |
| **Reading Stats** | Today's reading time, all-time total, current book progress |
| **Page Overlay** | PNG/BMP image composited on top of current book page |

> **Battery warning:** Clock and Reading Stats modes require **Keep Clock Alive** to update during sleep. This drains ~3–4mA continuously. Only enable if you use these sleep screen modes.

### Virtual Chicken Companion

Your chicken grows with every page you read.

**Evolution:** Egg → Hatchling → Youngster → Companion → Elder

**Reading feeds your pet:** Every 20 pages read earns a meal (+25 hunger). Reading streaks reduce the cost:

| Streak | Pages per meal |
|--------|---------------|
| 0–6 days | 20 pages |
| 7–13 days | 16 pages |
| 14–29 days | 13 pages |
| 30+ days | 10 pages |

### Tools & Apps

| App | Description |
|-----|-------------|
| **File Transfer** | WiFi book upload from computer |
| **Clock** | Digital clock with calendar |
| **Pomodoro** | Work/break timer with pet happiness bonus |
| **Virtual Pet** | Pet care, feeding, evolution tracking |
| **Reading Stats** | Today/total/sessions summary with streak info |
| **Sleep Image** | Sleep mode selector + image picker + overlay slideshow |
| **OPDS Browser** | Browse and download from OPDS catalog servers |
| **Chess** | Full chess game with AI (Easy/Medium/Hard) |
| **Caro** | Gomoku (5-in-a-row) with AI |
| **Sudoku** | 9x9 puzzle generator |
| **Minesweeper** | Classic minesweeper |
| **2048** | Tile-merging puzzle game |

### Connectivity

- **WiFi book upload** — transfer books from your computer's browser
- **Calibre / OPDS** — browse and download from your Calibre library or any OPDS server
- **KOReader Sync** — sync reading progress across devices
- **WiFi OTA updates** — update firmware over-the-air

> **Note:** WiFi uses 80–200mA. Charge your device before OTA updates or long sync sessions.

### Button Configuration

**Power button** supports single/double/triple click, each independently configurable.

**Side buttons** and **front pad** layouts are remappable. Long-press side button skips chapters.

### Status Bar

Fully customizable — each element can be shown or hidden independently:

- Chapter page count
- Book progress percentage
- Progress bar (book-level, chapter-level, or hidden)
- Title (book title, chapter title, or hidden)
- Battery percentage
- Clock

---

## Important Notes

### Battery Life

- **Keep Clock Alive** drains ~3–4mA continuously. Only enable for Clock or Reading Stats sleep screens.
- **Sleep Refresh Interval** requires Keep Clock Alive to be ON.
- **WiFi** uses significant power (80–200mA). Charge before OTA updates.

### Reading & Files

- **Auto page turn speed** is global, not per-book.
- **Deleting `.crosspoint/` folder** on SD card clears ALL reading progress and cached data.
- **Moving or renaming a book file** resets its reading progress (cache is tied to file path).

### Fonts

- **Bokerlam** is a Vietnamese-optimized serif font and may lack some Unicode characters. Missing glyphs are silently hidden.
- **Lexend** is a sans-serif font designed for readability.

---

## Development

### Prerequisites

- **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
- Python 3.8+
- USB-C cable
- Xteink X4

### Checking out the code

```sh
git clone --recursive https://github.com/jslopez9854-cmyk/PortVector
```

### Build verification (before flashing)

```sh
pio run
```

### Flashing your device

```sh
pio run --target upload
```

### Debugging

```python
python3 -m pip install pyserial colorama matplotlib
```

```sh
python3 scripts/debugging_monitor.py
```

---

## Internals

The ESP32-C3 only has ~380KB of usable RAM. Caching to SD card is used aggressively to minimize RAM usage.

### Data caching

```
.crosspoint/
├── epub_12471232/
│   ├── progress.bin
│   ├── cover.bmp
│   ├── book.bin
│   └── sections/
├── reading_stats.bin
└── ...
```

Deleting `.crosspoint/` clears the entire cache. Moving a book file resets its reading progress.

---

Based on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).
Inspired by [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader) by atomic14.
