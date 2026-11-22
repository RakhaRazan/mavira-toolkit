# MAVIRA - Native C++20 Batch Comic Converter, Extractor, Merger & Offline OCR Suite (v1.0.0)

**MAVIRA** is a 100% portable, high-performance Windows x64 C++20 utility suite for batch comic conversion, smart page extraction, archive creation/merging, and offline Windows OCR, complete with a built-in hardware-accelerated quick preview reader and SQLite WAL + FTS5 library index.

---

## Key Features

- **Batch Comic Converter:** Converts `.cbz`, `.cbr`, `.cb7`, `.cbt`, `.pdf`, `.epub`, and `.zip` containers with 100% lossless raw byte pass-through when keeping the original image format, plus optional batch image processing (scaling, levels, brightness, sharpening, grayscale manga tuning, and `.jpg`/`.png`/`.webp` re-encoding).
- **Smart Page Extractor:** Extracts pages into `<ComicName>/001.ext` structured subfolders with zero-byte filtering, natural sort ordering, and direct PDF embedded image extraction.
- **Comic Book Creator & Merger:** Packages image folders into `.cbz`, `.cb7`, `.cbt`, `.epub`, or `.pdf`, and merges multiple volumes via a 1-page streaming pipeline (< 40 MB RAM) with automatic chapter bookmarks.
- **Offline Native Windows OCR:** Zero-bloat WinRT `Windows::Media::Ocr::OcrEngine` integration with manga screentone symbol cleanup and ALL-CAPS to Sentence case normalization.
- **Built-In Quick Preview Reader:** Hardware-accelerated `QOpenGLWidget` viewport with Hold-to-Zoom Loupe Magnifier, Single Page / Dual-Page Smart Spread / Vertical Webtoon tiling, RTL/LTR reading modes, Auto-Crop, Auto-Scroll, and GLSL Manga Enhancer.
- **High-Scale Library & Portable Layout:** SQLite3 WAL + FTS5 full-text search, Dual-Mode Cover System (64 MB RAM LRU + single-file `covers.pack`), Unplugged Drive Guard, 5 Gradient Themes, 5 Interface Languages, and zero registry writes (`./data/`).

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

## Quick Start (Portable Release)

1. Download or extract the portable release folder containing `MAVIRA.exe` and `runtime/`.
2. Launch `MAVIRA.exe`.
3. All library data, settings, and cover caches are automatically created in the local `./data/` directory.

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

### Deploy Portable Release
```powershell
powershell -ExecutionPolicy Bypass -File packaging/deploy_windows.ps1
```
This builds `MAVIRA.exe` + `runtime/` into the repository root and verifies the deployed footprint.

---

## Open-Source Acknowledgments & License

MAVIRA is licensed under the **MIT License** - see [LICENSE](LICENSE).

Third-party open-source components (Qt 6 Widgets, PDFium, libzip, libarchive, libjpeg-turbo, libpng, libwebp, SQLite3, spdlog, GoogleTest) are acknowledged in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
