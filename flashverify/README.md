# FlashVerify

ตรวจสอบ Flash Drive / SSD ว่าแท้หรือเก๊ ด้วยการเขียน-อ่าน-ตรวจสอบ SHA-256 hash เต็มความจุ

## Features

- เขียน random data ทุก chunk แล้วอ่านกลับมาตรวจ hash
- ตรวจจับ firmware loopback (drive ปลอมที่ map ข้อมูลซ้ำ)
- แสดง chunk map สี realtime (GTK4 UI)
- สร้าง PDF report รองรับ Thai font
- รองรับ Windows (MSYS2) และ Linux

---

## ติดตั้ง (MSYS2 + Windows)

### 1. ติดตั้ง dependencies

เปิด **MSYS2 MinGW 64-bit** แล้วรัน:

```bash
pacman -S mingw-w64-x86_64-gcc \
           mingw-w64-x86_64-meson \
           mingw-w64-x86_64-ninja \
           mingw-w64-x86_64-openssl \
           mingw-w64-x86_64-gtk4 \
           mingw-w64-x86_64-pkg-config \
           mingw-w64-x86_64-python \
           mingw-w64-x86_64-python-pip
```

### 2. ติดตั้ง Python deps

```bash
pip install reportlab
```

### 3. (Optional) Thai font

ดาวน์โหลด [NotoSansThai-Regular.ttf](https://fonts.google.com/noto/specimen/Noto+Sans+Thai)
แล้ววางไว้ที่ `flashverify/python/NotoSansThai-Regular.ttf`

### 4. Build

```bash
cd flashverify
meson setup builddir
meson compile -C builddir
```

---

## วิธีใช้

### GUI (แนะนำ)

> ต้องรันเป็น Administrator

```bash
# คลิกขวา MSYS2 terminal → "Run as Administrator"
./builddir/flashverify.exe
```

1. เลือก device จาก dropdown (กด **Refresh** ถ้าไม่เห็น)
2. กรอก Claimed Capacity ตามที่กล่องบอก
3. กรอก Order Info (optional)
4. กด **Start Test**
5. รอให้ Write + Verify pass เสร็จ
6. กด **Save Report** เพื่อสร้าง PDF

### CLI

```bash
# list removable drives
./builddir/flashverify.exe list

# test drive (claimed 64 GB)
./builddir/flashverify.exe \\.\PhysicalDrive1 64
```

Exit code: `0` = GENUINE, `2` = COUNTERFEIT/WARNING

---

## ติดตั้ง (Linux)

```bash
# Ubuntu/Debian
sudo apt install gcc meson ninja-build libssl-dev libgtk-4-dev python3-pip
pip install reportlab

cd flashverify
meson setup builddir
meson compile -C builddir

# ต้อง root หรืออยู่ใน disk group
sudo ./builddir/flashverify

# หรือเพิ่ม user ใน disk group
sudo usermod -aG disk $USER
```

---

## Verdict

| Verdict | ความหมาย |
|---|---|
| **GENUINE** | ขนาดจริง ≥ ขนาดอ้าง และ hash ตรงทุก chunk |
| **WARNING** | hash ไม่ตรง < 5% ของ chunks |
| **COUNTERFEIT** | ขนาดจริง < ขนาดอ้าง หรือ hash ไม่ตรง ≥ 5% |

---

## Project Structure

```
flashverify/
├── src/
│   ├── main.c          entry point (GUI + CLI)
│   ├── ui.c / ui.h     GTK4 UI
│   ├── rawio.c / .h    raw device I/O abstraction (Win/Linux)
│   ├── device.c / .h   detect_device, list_removable_devices
│   ├── test.c / .h     write_pass, verify_pass, verdict
│   ├── hash.c / .h     SHA-256 via OpenSSL EVP
│   ├── random.c / .h   cryptographic random (getrandom/CryptGenRandom)
│   └── report.c / .h   dump_session_json, generate_pdf_report
├── python/
│   └── generate_report.py   PDF generator (ReportLab)
├── meson.build
└── README.md
```
