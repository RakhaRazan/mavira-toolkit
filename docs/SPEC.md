# MAVIRA (`MAVIRA.exe`) v1.0.0 - Technical Architecture & Specification

## 1. Overview

**MAVIRA v1.0.0** is a high-performance, 100% portable Windows x64 desktop comic, manga, and PDF library manager and GPU-accelerated reader designed to scale effortlessly across massive collections (30,000+ volumes, 1.3 TB+) and multi-gigabyte archive/PDF files (up to 20+ GB) while maintaining a minimal memory and disk footprint.

---

## 2. Tech Stack (100% Open-Source & Commercial-Safe)

| Layer / Component | Technology | License / Compliance Notes |
| :--- | :--- | :--- |
| **Language & Toolchain** | C++20 (MSVC 2022 x64, `WIN32` GUI subsystem) | Standard C++20 |
| **UI Framework** | Qt 6 Widgets (`qtbase`, dynamically linked) | LGPLv3 compliant via dynamic linking (`*.dll`) |
| **GPU Rendering** | `QOpenGLWidget` + custom GLSL shaders | OpenGL / GLSL |
| **Build & Dependencies** | CMake + Ninja + `vcpkg` | MIT / BSD tooling |
| **Archive Handling** | `libzip` (`.cbz`/`.zip`), `libarchive` (`.cbr`/`.rar`/`.cb7`/`.7z`/`.cbt`/`.tar`) | BSD-3-Clause / BSD-2-Clause |
| **PDF Engine** | `PDFium` | BSD-3-Clause |
| **Image Codecs** | `libjpeg-turbo`, `libpng`, `libwebp` | IJG / BSD / libpng |
| **Database & Search** | `SQLite3` (WAL mode + FTS5 full-text search) | Public Domain |
| **Logging** | `spdlog` | MIT |
| **Testing** | `GoogleTest` (`gtest`) | BSD-3-Clause |
| **Installer** | `Inno Setup` | Inno Setup License (commercial-safe) |

---

## 3. Engineering Targets

| Metric | Target |
| :--- | :--- |
| **Installer Size** | `< 100 MB` (~38-48 MB) |
| **Installed Footprint** | `< 100 MB` |
| **Idle RAM Usage** | `< 100 MB` (~35-55 MB) |
| **Cold Startup Time** | `< 300 ms` |
| **Frame Rate** | Smooth `60-120 FPS` during pan, zoom, and page transitions |
| **Collection Scale** | `30,000+` volumes (`1.3 TB+` total library size) |
| **Large File Support** | Multi-GB archives & PDFs (`20+ GB` per file) via streaming / indexed random access |

---

## 4. Architecture

MAVIRA enforces a strict **4-layer modular separation** with unidirectional dependencies:

```text
+-------------------------------------------------------------------+
| 1. UI Layer (src/ui/)                                             |
|    Qt 6 Widgets, Library Grid, GPU Viewport, Toolbars, Dialogs    |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
| 2. Application Layer (src/app/)                                   |
|    Use-case coordinators, State management, ViewModels, Commands  |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
| 3. Pure C++20 Core Engine (src/core/)                             |
|    No Qt UI dependencies; pure C++20 + codec/archive/GL primitives|
|    - threading/ : Worker pools, priority queues, cancellation     |
|    - archive/   : Streaming CBZ/CBR/CB7/PDF readers & ComicMerger |
|    - imaging/   : Fast decode, resize, crop detection, color ops  |
|    - rendering/ : GLSL shaders, texture tiling, page layout math  |
|    - metadata/  : ComicInfo.xml, filename parsing, natural sort   |
|    - cache/     : Dual-mode cover cache & page decode LRU         |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
| 4. Data & Win32 OS Layer (src/data/)                              |
|    - fs/        : Win32 volume/drive monitoring, cloud attributes |
|    - db/        : SQLite3 (WAL + FTS5) persistence & queries      |
|    - config/    : Portable ./data/ settings & state serialization |
+-------------------------------------------------------------------+
```

### Directory Layout

- `assets/shaders/` - GLSL vertex and fragment shaders (e.g., Manga Enhancer, tone/contrast, sharpening)
- `packaging/` - Inno Setup scripts, deployment manifests, and LGPLv3 notice/DLL bundling configs
- `docs/` - Architecture specs and design documentation
- `src/app/` - Application orchestration and state controllers
- `src/core/` - Pure C++20 engine (`threading/`, `archive/`, `imaging/`, `rendering/`, `metadata/`, `cache/`)
- `src/data/` - Persistence & OS integration (`fs/`, `db/`, `config/`)
- `src/ui/` - Qt 6 Widgets views, delegates, and `QOpenGLWidget` canvas
- `tests/` - GoogleTest unit and integration test suites

---

## 5. Core Features

### 5.1 100% Portable `./data/` Layout
- All runtime state-SQLite database (`*.db`, `*.db-wal`, `*.db-shm`), configuration, logs (`logs/`), and optional cover packs (`covers.pack`)-resides in a self-contained `./data/` directory next to `MAVIRA.exe`.
- Zero registry writes or mandatory `%APPDATA%` pollution; moving the folder moves the entire library state.

### 5.2 Resilient Storage & Cloud Awareness (`src/data/fs/`)
- **Unplugged Drive Guard:** Tracks volume GUIDs / drive letters and marks disconnected external drives as offline without purging library metadata, reading progress, or bookmarks.
- **Cloud Sync Detection + Offline Pinning:** Inspects Win32 file attributes (e.g., OneDrive/cloud placeholder attributes) to distinguish cloud-only files from locally hydrated files, avoiding accidental mass downloads during background scans and supporting explicit offline pinning.

### 5.3 Dual-Mode Cover System (`src/core/cache/`)
- **Mode 1 - 64 MB RAM LRU:** Zero-disk-bloat in-memory LRU cache capped at 64 MB for instant visible-grid cover rendering with on-demand background decoding.
- **Mode 2 - Single-File `covers.pack`:** Optional consolidated binary pack file inside `./data/` storing pre-decoded thumbnails for instant cold-boot grid scrolling across 30,000+ volumes without thousands of loose files.

### 5.4 GPU-Accelerated Reader (`src/core/rendering/`, `src/ui/`, `assets/shaders/`)
- **Reading Modes:**
  - `Single Page` & `Dual Page` spread modes
  - `RTL Manga` & `LTR` reading directions
  - `Smart Wide-Spread` detection (automatically displays landscape/double-width pages across full width)
  - `Vertical Webtoon` continuous scroll with **GPU texture tiling** to support ultra-tall strips without exceeding `GL_MAX_TEXTURE_SIZE`
- **Visual & Navigation Tools:**
  - `Auto-Crop` (automatic whitespace/border trimming)
  - `Auto-Scroll` (smooth variable-speed continuous scrolling)
  - `Hold-to-Zoom Loupe Magnifier` (high-resolution localized inspection lens)
  - `GLSL Manga Enhancer` (real-time GPU shader pass for crisp linework, black-level/contrast tuning, and de-halftoning/sharpening)
  - `Named Bookmarks` (per-volume named page bookmarks persisted in SQLite)

### 5.5 `ComicMerger` & Archive Tooling (`src/core/archive/`)
- **1-Page Streaming Merger:** Merges multiple `.cbr`, `.cbz`, and `.pdf` inputs into a unified output by streaming **one page at a time**-keeping peak RAM flat even when merging multi-GB volumes.
- **Extract Pages:** Selective or full page extraction from supported archive and PDF formats.
- **Create CBZ:** Packages image folders or page streams directly into compliant `.cbz` archives.
