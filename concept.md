# FlashVerify — Implementation Spec

> ตรวจสอบ Flash Drive / SSD แท้เก๊ ด้วยการเขียน-อ่าน-ตรวจสอบ hash เต็มความจุ  
> Stack: **C + GTK4 + OpenSSL** | PDF report: **Python + ReportLab**

---

## สารบัญ

1. [Architecture Overview](#1-architecture-overview)
2. [Project Structure](#2-project-structure)
3. [Data Structures](#3-data-structures)
4. [Core Algorithm](#4-core-algorithm)
5. [Module Breakdown](#5-module-breakdown)
6. [GTK4 UI Spec](#6-gtk4-ui-spec)
7. [PDF Report Generator](#7-pdf-report-generator)
8. [Build System](#8-build-system)
9. [Platform Notes](#9-platform-notes)
10. [Implementation Checklist](#10-implementation-checklist)

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                    GTK4 Main Thread                  │
│  GtkWindow → UI widgets → start/cancel buttons      │
│  g_idle_add() ← progress updates ←──────────────────┤
└────────────────────────┬────────────────────────────┘
                         │ GThread
          ┌──────────────▼──────────────┐
          │      Test Worker Thread      │
          │  write_pass() → verify_pass()│
          │  fills TestSession struct    │
          └──────────────┬──────────────┘
                         │ JSON via pipe / stdout
          ┌──────────────▼──────────────┐
          │   Python PDF Generator       │
          │   generate_report.py         │
          │   → flashverify_report.pdf   │
          └─────────────────────────────┘
```

**หลักการสำคัญ**
- UI thread และ test thread แยกกันเสมอ — ห้าม block GTK main loop
- อ่าน/เขียน device ผ่าน raw fd พร้อม `O_DIRECT | O_SYNC` (bypass page cache)
- hash ทุก chunk เก็บใน RAM ไม่เขียนลง drive
- เสร็จแล้ว dump `TestSession` เป็น JSON ส่งให้ Python สร้าง PDF

---

## 2. Project Structure

```
flashverify/
├── src/
│   ├── main.c          # GTK app entry, g_application_run()
│   ├── ui.c / ui.h     # GtkWindow layout, widgets, callbacks
│   ├── device.c / .h   # detect_device(), list_removable()
│   ├── test.c / .h     # write_pass(), verify_pass(), verdict()
│   ├── hash.c / .h     # sha256_chunk() wrapper → OpenSSL EVP
│   ├── random.c / .h   # fill_random_buffer() → getrandom() / CryptGenRandom
│   └── report.c / .h   # dump_session_json(), spawn PDF generator
├── python/
│   └── generate_report.py   # ReportLab PDF generator (รับ JSON stdin)
├── meson.build
├── meson_options.txt
└── README.md
```

---

## 3. Data Structures

### 3.1 VerifyStatus

```c
typedef enum {
    CHUNK_PENDING    = 0,
    CHUNK_WRITTEN    = 1,
    CHUNK_OK         = 2,   // read back + hash match
    CHUNK_MISMATCH   = 3,   // hash ไม่ตรง (เก๊)
    CHUNK_UNREADABLE = 4,   // อ่านกลับไม่ได้
} VerifyStatus;
```

### 3.2 ChunkRecord

```c
typedef struct {
    uint64_t     offset;        // byte offset บน device
    uint64_t     length;        // ขนาด chunk (bytes)
    uint8_t      sha256[32];    // hash ของ random data ที่เขียนลงไป
    VerifyStatus status;
    double       write_ms;      // เวลาเขียน chunk นี้ (ms)
    double       read_ms;       // เวลาอ่านกลับ (ms)
} ChunkRecord;
```

### 3.3 TestState

```c
typedef enum {
    STATE_IDLE      = 0,
    STATE_WRITING   = 1,
    STATE_VERIFYING = 2,
    STATE_DONE      = 3,
    STATE_CANCELLED = 4,
    STATE_ERROR     = 5,
} TestState;
```

### 3.4 TestSession (โครงสร้างหลัก)

```c
typedef struct {
    // --- device info ---
    char        device_path[512];     // "/dev/sdb" หรือ "\\.\PhysicalDrive1"
    char        device_name[256];     // จาก udev / WMI
    char        device_serial[64];
    char        vendor_id[32];
    char        product_id[32];

    // --- capacity ---
    uint64_t    claimed_bytes;        // ที่ผู้ใช้กรอก (ตามกล่อง)
    uint64_t    actual_bytes;         // จาก ioctl(BLKGETSIZE64)
    uint64_t    chunk_size;           // ปกติ 64 MB = 67108864

    // --- test data ---
    ChunkRecord *chunks;              // malloc'd array
    size_t       chunk_count;
    size_t       ok_count;
    size_t       fail_count;

    // --- state ---
    TestState    state;
    char         error_msg[256];

    // --- performance ---
    double       avg_write_mbps;
    double       avg_read_mbps;

    // --- order info (กรอกใน UI) ---
    char        order_id[64];
    char        shop_name[128];
    char        platform[32];        // "Shopee" / "Lazada"
    char        purchase_price[32];

    // --- callback (thread → UI) ---
    void      (*on_progress)(struct TestSession *s, void *user_data);
    void       *callback_data;
} TestSession;
```

---

## 4. Core Algorithm

### 4.1 write_pass()

```
WRITE PASS
──────────
input:  TestSession *s, int fd (O_RDWR | O_DIRECT | O_SYNC)

for each chunk i in [0 .. chunk_count):
    1. fill buffer ด้วย random bytes    → fill_random_buffer(buf, len)
    2. คำนวณ SHA-256 ของ buffer          → sha256_chunk(buf, len, s->chunks[i].sha256)
    3. lseek(fd, s->chunks[i].offset, SEEK_SET)
    4. write(fd, buf, chunk_size)       ← ถ้า fail → mark CHUNK_UNREADABLE
    5. s->chunks[i].status = CHUNK_WRITTEN
    6. g_idle_add(update_write_progress, s)   ← UI update

หมายเหตุ:
- chunk สุดท้ายอาจมีขนาด < chunk_size  (actual_bytes % chunk_size)
- ถ้า write ได้น้อยกว่า chunk_size → retry 1 ครั้ง แล้ว mark UNREADABLE
```

### 4.2 verify_pass()

```
VERIFY PASS
───────────
input:  TestSession *s, int fd (O_RDONLY | O_DIRECT)

for each chunk i in [0 .. chunk_count):
    1. lseek(fd, s->chunks[i].offset, SEEK_SET)
    2. read(fd, buf, s->chunks[i].length)
    3. sha256_chunk(buf, len, read_hash)
    4. memcmp(read_hash, s->chunks[i].sha256, 32)
       → ตรง  → CHUNK_OK
       → ไม่ตรง → CHUNK_MISMATCH
    5. g_idle_add(update_verify_progress, s)

ตรวจ loopback:
    ถ้า chunk i และ chunk j (j < i) มี sha256 เหมือนกันแต่ offset ต่างกัน
    → firmware กำลัง loopback → mark ทั้งคู่เป็น CHUNK_MISMATCH + log warning
```

### 4.3 verdict()

```c
typedef enum {
    VERDICT_GENUINE      = 0,   // actual >= claimed AND fail_count == 0
    VERDICT_COUNTERFEIT  = 1,   // actual < claimed OR fail_count > 0
    VERDICT_WARNING      = 2,   // actual >= claimed AND 0 < fail < 5%
} Verdict;

Verdict verdict(TestSession *s) {
    if (s->actual_bytes < s->claimed_bytes)
        return VERDICT_COUNTERFEIT;
    double fail_ratio = (double)s->fail_count / s->chunk_count;
    if (fail_ratio == 0.0)
        return VERDICT_GENUINE;
    if (fail_ratio < 0.05)
        return VERDICT_WARNING;
    return VERDICT_COUNTERFEIT;
}
```

---

## 5. Module Breakdown

### 5.1 device.c

```c
// อ่านขนาดจริงจาก OS
int     detect_device(const char *path, uint64_t *out_bytes);

// แสดงรายชื่อ removable drive ให้เลือก
// Linux: อ่านจาก /sys/block/*/removable + /proc/partitions
// Windows: WMI query Win32_DiskDrive WHERE MediaType='Removable Media'
char  **list_removable_devices(int *count);

void    free_device_list(char **list, int count);
```

**Linux — ioctl:**
```c
#include <linux/fs.h>
int fd = open(path, O_RDONLY);
uint64_t size = 0;
ioctl(fd, BLKGETSIZE64, &size);   // ได้ bytes โดยตรง
```

**Windows — DeviceIoControl:**
```c
HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
DISK_GEOMETRY_EX geo;
DWORD bytes;
DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                &geo, sizeof(geo), &bytes, NULL);
uint64_t size = geo.DiskSize.QuadPart;
```

---

### 5.2 hash.c

```c
// wrapper บน OpenSSL EVP_DigestUpdate — thread-safe
void sha256_chunk(const uint8_t *data, size_t len, uint8_t out[32]);

// hex string สำหรับ log/PDF
void sha256_to_hex(const uint8_t hash[32], char out[65]);
```

ใช้ OpenSSL EVP API (ไม่ใช้ deprecated SHA256() โดยตรง):
```c
EVP_MD_CTX *ctx = EVP_MD_CTX_new();
EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
EVP_DigestUpdate(ctx, data, len);
EVP_DigestFinal_ex(ctx, out, NULL);
EVP_MD_CTX_free(ctx);
```

---

### 5.3 random.c

```c
void fill_random_buffer(uint8_t *buf, size_t len);
```

```c
// Linux (kernel 3.17+)
#include <sys/random.h>
getrandom(buf, len, 0);

// Windows
#include <wincrypt.h>
HCRYPTPROV hProv;
CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
CryptGenRandom(hProv, (DWORD)len, buf);
```

> สำคัญ: อย่าใช้ `rand()` — predictable เกินไป ทำให้ loopback detection พลาดได้

---

### 5.4 report.c

```c
// Dump TestSession เป็น JSON ส่งไปยัง Python generator
int dump_session_json(TestSession *s, const char *out_path);

// Spawn Python process สร้าง PDF
int generate_pdf_report(const char *json_path, const char *pdf_path);
```

```c
// spawn PDF generator
char cmd[512];
snprintf(cmd, sizeof(cmd),
    "python3 %s/generate_report.py --input %s --output %s",
    get_script_dir(), json_path, pdf_path);
system(cmd);   // หรือ popen() เพื่อ capture stdout/stderr
```

---

## 6. GTK4 UI Spec

### 6.1 Widget Tree

```
GtkApplicationWindow "FlashVerify"
└── GtkBox (vertical, spacing=12, margin=16)
    ├── [Header] GtkLabel "FlashVerify" (large, bold)
    │
    ├── [Section] อุปกรณ์
    │   ├── GtkDropDown  (device list)
    │   └── GtkButton    "Refresh"
    │
    ├── [Section] ออเดอร์ (collapsible GtkExpander)
    │   ├── GtkEntry  order_id
    │   ├── GtkEntry  shop_name
    │   ├── GtkComboBoxText  platform  (Shopee / Lazada / อื่นๆ)
    │   └── GtkEntry  purchase_price
    │
    ├── [Section] ความจุ
    │   ├── GtkSpinButton  claimed_gb
    │   ├── GtkComboBoxText  unit  (GB / TB)
    │   └── GtkLabel  actual_size  (อัพเดทหลัง detect)
    │
    ├── [Progress] Write Pass
    │   ├── GtkProgressBar  write_bar
    │   └── GtkLabel  write_label  "0 / 1024 chunks  —  18.4 MB/s"
    │
    ├── [Progress] Verify Pass
    │   ├── GtkProgressBar  verify_bar
    │   └── GtkLabel  verify_label
    │
    ├── [Map] GtkDrawingArea  sector_map  (custom draw)
    │
    ├── [Result] GtkLabel  verdict_label  (hidden until done)
    │
    └── [Buttons] GtkBox (horizontal)
        ├── GtkButton  "Start Test"   (suggested-action)
        ├── GtkButton  "Cancel"       (destructive-action, visible during test)
        └── GtkButton  "Save Report"  (visible when done)
```

### 6.2 Threading Pattern

```c
// ── เริ่ม test ──────────────────────────────────────────
void on_start_clicked(GtkButton *btn, gpointer user_data) {
    AppData *app = user_data;
    app->session->on_progress = on_progress_cb;
    app->session->callback_data = app;

    GThread *t = g_thread_new("flashverify-test", test_thread_func, app->session);
    g_thread_unref(t);
}

// ── worker thread ───────────────────────────────────────
gpointer test_thread_func(gpointer data) {
    TestSession *s = data;
    int fd = open(s->device_path, O_RDWR | O_DIRECT | O_SYNC);
    write_pass(s, fd);
    close(fd);

    fd = open(s->device_path, O_RDONLY | O_DIRECT);
    verify_pass(s, fd);
    close(fd);

    s->state = STATE_DONE;
    g_idle_add(on_test_done_idle, s);
    return NULL;
}

// ── UI update (safe — main thread) ─────────────────────
gboolean on_progress_idle(gpointer data) {
    ProgressUpdate *u = data;
    gtk_progress_bar_set_fraction(u->bar, u->fraction);
    gtk_label_set_text(u->label, u->text);
    gtk_widget_queue_draw(u->sector_map);  // redraw chunk map
    g_free(u);
    return G_SOURCE_REMOVE;
}
```

### 6.3 Sector Map Drawing (GtkDrawingArea)

```c
void draw_sector_map(GtkDrawingArea *area, cairo_t *cr,
                     int width, int height, gpointer data) {
    TestSession *s = data;
    int cols = 40;
    int rows = (s->chunk_count + cols - 1) / cols;
    double cw = (double)width / cols;
    double ch = MAX(6.0, (double)height / rows);

    for (size_t i = 0; i < s->chunk_count; i++) {
        int col = i % cols;
        int row = i / cols;
        double x = col * cw + 1;
        double y = row * ch + 1;

        switch (s->chunks[i].status) {
            case CHUNK_PENDING:    cairo_set_source_rgb(cr, 0.82, 0.82, 0.78); break;
            case CHUNK_WRITTEN:    cairo_set_source_rgb(cr, 0.28, 0.56, 0.76); break;
            case CHUNK_OK:         cairo_set_source_rgb(cr, 0.23, 0.43, 0.06); break;
            case CHUNK_MISMATCH:   cairo_set_source_rgb(cr, 0.64, 0.18, 0.18); break;
            case CHUNK_UNREADABLE: cairo_set_source_rgb(cr, 0.52, 0.31, 0.04); break;
        }
        cairo_rectangle(cr, x, y, cw - 2, ch - 2);
        cairo_fill(cr);
    }
}
```

---

## 7. PDF Report Generator

### 7.1 Interface (JSON Input)

```json
{
  "report_id":       "FV-2025-0328-001",
  "report_date":     "28 มีนาคม 2568",
  "report_time":     "14:23:07",
  "software_ver":    "FlashVerify 1.0.0",

  "device_path":     "/dev/sdb",
  "device_name":     "Kingston DataTraveler 100 G3",
  "device_serial":   "0000E43F52FE",
  "vendor_id":       "0951",

  "claimed_bytes":   68719476736,
  "actual_bytes":    7999832064,
  "claimed_gb":      64.0,
  "actual_gb":       7.45,

  "verdict":         "COUNTERFEIT",
  "chunk_size_mb":   64,
  "total_chunks":    1024,
  "ok_chunks":       119,
  "fail_chunks":     905,

  "write_speed_mbps": 18.4,
  "read_speed_mbps":  22.1,
  "hash_algo":        "SHA-256",

  "order_id":        "250312ABCD1234",
  "shop_name":       "tech_bestdeal88",
  "platform":        "Shopee",
  "purchase_price":  "฿ 350",

  "fail_chunk_samples": [
    { "index": 120, "offset": 8053063680, "expected": "b2f711...", "got": "a3f8c2..." },
    { "index": 121, "offset": 8120172544, "expected": "9c3a44...", "got": "d4e210..." }
  ]
}
```

### 7.2 CLI Usage

```bash
# Manual
python3 generate_report.py --input session.json --output report.pdf

# Auto (spawn จาก C หลัง test เสร็จ)
python3 generate_report.py --input /tmp/fv_session.json --output ~/Desktop/flashverify_report.pdf
```

### 7.3 Dependencies (Python)

```bash
pip install reportlab
# fonts: ต้องมี FreeSerif.ttf / NotoSansThai หรือ font Thai อื่น
# Linux: apt install fonts-freefont-ttf
# หรือ download NotoSansThai-Regular.ttf ใส่ไว้ใน python/ dir
```

---

## 8. Build System

### 8.1 meson.build

```meson
project('flashverify', 'c',
  version: '1.0.0',
  default_options: ['c_std=c11', 'warning_level=2'])

gtk4  = dependency('gtk4')
ssl   = dependency('openssl')
glib  = dependency('glib-2.0')

src = files(
  'src/main.c',
  'src/ui.c',
  'src/device.c',
  'src/test.c',
  'src/hash.c',
  'src/random.c',
  'src/report.c',
)

executable('flashverify', src,
  dependencies: [gtk4, ssl, glib],
  install: true)
```

### 8.2 Build Commands

```bash
# setup
meson setup builddir

# build
meson compile -C builddir

# run (ต้องการ root หรือ disk group สำหรับ raw device access)
sudo ./builddir/flashverify

# หรือ add user ไปยัง disk group (Linux)
sudo usermod -aG disk $USER
```

---

## 9. Platform Notes

### Linux

| สิ่งที่ต้องทำ | วิธี |
|---|---|
| Raw device access | `open("/dev/sdb", O_RDWR \| O_DIRECT \| O_SYNC)` |
| ขนาด drive | `ioctl(fd, BLKGETSIZE64, &size)` |
| Permission | user ต้องอยู่ใน `disk` group หรือรัน `sudo` |
| List devices | อ่าน `/sys/block/*/removable` ค่า == "1" |
| Buffer alignment | `O_DIRECT` บังคับ buffer align 512 bytes → ใช้ `posix_memalign()` |

```c
// aligned buffer สำหรับ O_DIRECT
uint8_t *buf;
posix_memalign((void**)&buf, 512, chunk_size);
```

### Windows

| สิ่งที่ต้องทำ | วิธี |
|---|---|
| Raw device access | `CreateFile("\\\\.\\PhysicalDrive1", GENERIC_READ\|WRITE, ...)` |
| ขนาด drive | `DeviceIoControl(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX)` |
| Permission | ต้องรันเป็น Administrator |
| No-buffering | `FILE_FLAG_NO_BUFFERING \| FILE_FLAG_WRITE_THROUGH` |
| Buffer alignment | `VirtualAlloc()` (align อัตโนมัติตาม sector size) |

---

## 10. Implementation Checklist

### Phase 1 — Core Engine (ไม่มี UI)

- [ ] `hash.c` — `sha256_chunk()` + unit test เทียบกับ `sha256sum`
- [ ] `random.c` — `fill_random_buffer()` บน Linux + Windows
- [ ] `device.c` — `detect_device()` อ่าน actual bytes ได้ถูกต้อง
- [ ] `device.c` — `list_removable_devices()` แสดง USB drives
- [ ] `test.c` — `write_pass()` เขียน + เก็บ hash ครบ
- [ ] `test.c` — `verify_pass()` อ่านกลับ + เปรียบ hash
- [ ] `test.c` — `verdict()` ตัดสินผล
- [ ] ทดสอบด้วย loopback device (`dd if=/dev/zero bs=1G count=8 of=fake.img`)

### Phase 2 — GTK4 UI

- [ ] `ui.c` — layout ครบตาม widget tree
- [ ] threading — `GThread` + `g_idle_add()` ไม่ freeze UI
- [ ] `GtkDrawingArea` sector map render ด้วย cairo
- [ ] progress bar อัพเดท real-time
- [ ] cancel button หยุด test กลาง คืนไฟล์/device ได้สะอาด
- [ ] เพิ่ม GtkExpander สำหรับ order info

### Phase 3 — PDF Report

- [ ] `report.c` — `dump_session_json()` output ถูก schema
- [ ] `generate_report.py` — รับ JSON arg ได้
- [ ] `generate_report.py` — รองรับ Thai font (FreeSerif / Noto)
- [ ] "Save Report" button spawn Python + เปิด file manager

### Phase 4 — Polish

- [ ] Error dialog เมื่อ device ถูกดึงออกกลางคัน
- [ ] ป้องกัน select system drive (`/dev/sda`) โดยไม่ตั้งใจ
- [ ] ใส่ warning dialog ก่อน start ("ข้อมูลบน drive จะถูกลบทั้งหมด")
- [ ] AppImage / .exe installer
- [ ] README.md วิธีติดตั้ง + วิธีใช้

---

## Quick Reference — ลำดับ function calls

```
main()
 └─ gtk_application_new() → g_application_run()
     └─ app_activate()
         └─ build_ui() → show GtkWindow
             └─ on_start_clicked()
                 ├─ validate_inputs()
                 ├─ detect_device()          ← ตรวจขนาดจริง
                 ├─ session_init()           ← malloc ChunkRecord[]
                 └─ g_thread_new(test_thread_func)
                     ├─ write_pass()
                     │   └─ [loop] fill_random → sha256 → write → g_idle_add(update_ui)
                     ├─ verify_pass()
                     │   └─ [loop] read → sha256 → compare → g_idle_add(update_ui)
                     ├─ verdict()
                     ├─ dump_session_json()
                     └─ g_idle_add(on_test_done_idle)
                         └─ show verdict label + enable "Save Report"
                             └─ on_save_report_clicked()
                                 └─ generate_pdf_report()  ← spawn Python
```

---

*FlashVerify — ออกแบบมาเพื่อหลักฐานที่ชัดเจน เรียบง่าย และเชื่อถือได้*