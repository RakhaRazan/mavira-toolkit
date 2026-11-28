# MAVIRA - Native C++20 Comic Reader, Batch Converter, Extractor, Merger & Offline OCR Suite

**MAVIRA** is a 100% portable, high-performance Windows x64 C++20 comic reader and batch utility suite built for **30,000+ volume collections** across Manga (JP RTL), Manhwa (KR Vertical Webtoon), and Manhua (CN Full-Color), backed by SQLite WAL + FTS5 indexing and hardware-accelerated OpenGL rendering.

---

## Key Features:

### Hardware-Accelerated Daily Reader (30,000+ Volumes)
- **GPU-Accelerated OpenGL Viewport (`QOpenGLWidget`):** Smooth rendering with Single Page, Dual-Page Smart Spread (`Smart Wide-Spread` auto-solo detection), `Auto-Crop` whitespace/black-border trimming, and `F11` global fullscreen.
- **Seamless Multi-Page Vertical Webtoon & Auto-Scroll:** Per-page `[N-1, N, N+1]` GPU tile cache with incremental `glDeleteTextures` eviction for zero-stutter continuous scrolling across 120-slice Manhwa chapters, plus 60fps hands-free Auto-Scroll (`Shift+Space`).
- **Per-Comic Reading Profiles & Auto-Webtoon Detection:** Automatically saves Layout, Direction (`RTL`/`LTR`), and Fit Mode per comic in SQLite without mutating global Preferences; unset comics with `height / width > 2.2` auto-activate `VerticalWebtoon` + `LTR` + `FitWidth`.
- **True HiDPI Loupe Magnifier:** Full `devicePixelRatioF()` scaling across `resizeGL()`, `paintGL()`, and the circular GLSL Loupe Magnifier (`2.0x-4.0x`, `180-320 px`) for razor-sharp Japanese furigana/kanji on 125%-200% Windows DPI.
- **30,000-Title Discovery & Storage:** SQLite WAL + FTS5 instant search, indexed `Sort By` (`Title A-Z`, `Recently Added`, `Last Read`, `File Size`) and status filters (`All`, `Unread`, `In Progress`, `Finished`), Unicode CJK series buckets (`A-Z`, `JP`, `KR`, `CJK`, `#`), Dual-Mode Cover Cache (`covers.pack` persistent Win32 `HANDLE` + `OVERLAPPED` offset reads vs 64 MB Zero-Disk RAM LRU), `[N-5..N+2]` sequential RAR5/7z backward retention, and **Unplugged Drive Guard** for external HDDs.

### Batch Comic Toolkit & Offline OCR
- **100% Lossless Raw Byte Pass-Through:** Converts `.cbz`, `.cbr` (RAR5), `.cb7`, `.cbt`, `.pdf`, `.epub`, and `.zip` containers without re-encoding images when keeping the original format, plus optional batch image processing (scaling, levels, brightness, sharpening, grayscale manga tuning, and `.jpg`/`.png`/`.webp` encoding).
- **PDF & Streaming Merger Optimizations:** `/DeviceGray` monochrome PDF optimization, direct embedded PDF image extraction, explicit >4 GB PDF Win64 LLP64 guard, and a 1-page streaming Comic Merger with automatic chapter bookmarks.
- **Offline Native Windows OCR (`Tategaki` + CJK):** Zero-bloat WinRT `Windows::Media::Ocr::OcrEngine` with Japanese/Chinese vertical (`Tategaki`) right-to-left column sorting, CJK inter-character space normalization, halftone screentone cleanup, and ALL-CAPS normalization.
- **5 Gradient Themes & 5 Languages:** `Obsidian Cyber`, `Midnight Velvet`, `Warm Parchment`, `Nordic Glacier`, and `Sakura Mist` with live switching across `en`, `id`, `zh`, `ja`, and `ko` (zero raw i18n keys).

---

## Verified v1.1.0 Benchmarks

| Metric | Measured Result |
| :--- | :--- |
| **Portable Deployed Footprint (`MAVIRA.exe` + `runtime/`)** | **44.60 MB** |
| **Cold Spawn (`MAVIRA.exe` -> `MAVIRA_Core.exe`)** | **69 ms** |
| **Idle WorkingSet RAM (Lazy Workspaces)** | **59.46 MB** (`20.64 MB` Private) |
| **Automated Unit Test Suite (`ctest`)** | **69 / 69 Passed (100%)** |

---

## Architecture

MAVIRA enforces a strict 4-layer modular C++20 architecture:

| Layer | Directory | Technology |
| :--- | :--- | :--- |
| **1. UI Layer** | `src/ui/` | Qt 6 Widgets, `QOpenGLWidget`, custom GLSL shaders (`assets/shaders/`) |
| **2. Application Layer** | `src/app/` | Qt 6 Core coordinators (`LibraryManager`, `ReaderController`, `ApplicationCore`) |
| **3. Core Engine** | `src/core/` | Pure C++20 (zero Qt dependencies): `PDFium`, `libzip`, `libarchive`, `libjpeg-turbo`, `libpng`, `libwebp`, WinRT OCR |
| **4. Data & OS Layer** | `src/data/` | Pure C++20: `SQLite3` (WAL + FTS5), Win32 volume/file watchers, portable `./data/` config |

---

## Quick Start (Portable & Installer)

### Portable ZIP
1. Download `MAVIRA-v1.1.0-Windows-x64-Portable.zip` from [GitHub Releases](https://github.com/RakhaRazan/mavira-toolkit/releases/latest).
2. Extract the folder containing `MAVIRA.exe` and `runtime/`.
3. Launch `MAVIRA.exe`. All library data, settings, and cover caches are stored cleanly inside `./data/`.

### Windows x64 Installer
1. Download and run `MAVIRA-Setup-x64-v1.1.0.exe` from [GitHub Releases](https://github.com/RakhaRazan/mavira-toolkit/releases/latest).
2. Launch MAVIRA from the Start Menu or Desktop shortcut.

---

## Build from Source (Windows x64)

### Prerequisites
- Windows 10 / 11 (x64)
- Visual Studio 2022 (MSVC v143 C++20 toolchain)
- CMake 3.25+ & Ninja
- `vcpkg` (`VCPKG_ROOT` configured)

### Configure, Build & Test
```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel
ctest --test-dir build/x64-release --output-on-failure
```

### Deploy & Package Release
```powershell
powershell -ExecutionPolicy Bypass -File packaging/deploy_windows.ps1
powershell -ExecutionPolicy Bypass -File packaging/package_release.ps1
```

---

## Open-Source Acknowledgments & License

MAVIRA is licensed under the **MIT License** - see [LICENSE](LICENSE).

Third-party open-source components (Qt 6 Widgets, PDFium, libzip, libarchive, libjpeg-turbo, libpng, libwebp, SQLite3, spdlog, GoogleTest) are acknowledged in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
