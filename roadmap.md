# FlashVerify — Roadmap

## Phase 5 — Thai Language Support

**Goal:** UI and PDF report support Thai language

### Tasks

1. **i18n layer (C)**
   - Create `src/i18n.h` / `src/i18n.c`
   - Simple `tr(key)` function returning string based on selected locale
   - No gettext dependency — plain struct array
   - Supported locales: `LANG_EN`, `LANG_TH`

2. **Language selector in UI**
   - Dropdown (EN / TH) in header area
   - Stored in `AppData`, rebuilds all labels on change

3. **Thai font in GTK4 (Windows)**
   - Check system fonts for Thai support (Noto Sans Thai, Leelawadee)
   - Bundle `NotoSansThai-Regular.ttf` alongside the executable if needed
   - Apply via GTK CSS provider: `* { font-family: "Noto Sans Thai", sans-serif; }`

4. **PDF report (Thai)**
   - `generate_report.py` already has Thai font support
   - Add `"language": "th"` field to JSON session output
   - Switch PDF labels based on that field

**Risk:** Thai font rendering on GTK4/Windows requires real-device testing

---

## Phase 6 — macOS Support

**Goal:** Run FlashVerify on macOS — both CLI and GUI

### Tasks

1. **rawio.c — macOS backend**
   - `rawio_open_rw` / `rawio_open_ro` use `open()` like Linux
   - Unmount volumes via `DADiskUnmount()` (DiskArbitration framework)
     - Alternative: `diskutil unmount /dev/diskNsM` via `system()` to avoid framework dependency
   - No `O_DIRECT` on macOS — use `fcntl(F_NOCACHE)` + `fcntl(F_FULLFSYNC)` instead
   - `rawio_now_ms()` already works via `CLOCK_MONOTONIC`

2. **device.c — macOS backend**
   - `list_removable_devices()`: parse `diskutil list -plist` via `popen()`
     - Alternative: IOKit `IOServiceMatching("IOUSBMassStorageClass")`
   - `detect_device()`: `ioctl(DKIOCGETBLOCKCOUNT)` + `ioctl(DKIOCGETBLOCKSIZE)`
   - `is_system_drive()`: check `/` mount point via `getmntinfo()`

3. **UI — GTK4 via Homebrew**
   - Install: `brew install gtk4`
   - `meson.build` requires minimal changes — pkg-config handles it
   - Known risks: HiDPI scaling, dark mode, window decorations

4. **Build system**
   - Add macOS condition in `meson.build`:
     - Link `-framework DiskArbitration -framework CoreFoundation` (if using DA)
     - Or `-framework IOKit` (if using IOKit)
   - Create `.app` bundle via `gtk-mac-bundler` or a packaging script

5. **PDF report**
   - Python + ReportLab works on macOS without changes
   - `generate_pdf_report()` in `report.c` uses `python3` (already in macOS PATH)
   - No code changes needed

**Risks:**
- `diskutil unmount` may require SIP disabled in some cases
- GTK4 on macOS has known issues with input methods and Thai text rendering
- Must test on both Apple Silicon (arm64) and Intel (x86_64)

---

## Execution Order

```
Phase 5: Thai UI
    └─> Phase 6: macOS rawio + device
            └─> Phase 6: macOS GUI + build + packaging
```

- Phase 5 can be done entirely on Windows
- Phase 6 requires a physical Mac for testing
