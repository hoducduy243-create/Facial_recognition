# Hướng Dẫn Nạp Hệ Thống Kiểm Soát Cửa Bằng ESP-IDF Thuần

Dự án này được viết hoàn toàn bằng **C / ESP-IDF chuẩn (v4.4 - v6.x)**, tối ưu bộ nhớ PSRAM và truyền nhận FreeRTOS đa nhiệm.

Thư mục dự án:
`C:\Users\ACER\.gemini\antigravity-ide\scratch\smart_dual_camera_access_idf\`

```text
smart_dual_camera_access_idf/
├── slave_esp32cam/                  # Cụm Slave: ESP32-CAM + OV3660 + Serial/PIR Trigger
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/main.c
├── master_esp32s3/                  # Cụm Master: ESP32-S3 + OV5640 + ST7789 + Relay
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/
│       ├── display_st7789.c / .h
│       └── main.c
└── central_server_python/           # Central Server Python (AI Face Recognition + Telegram)
    ├── server.py
    └── requirements.txt
```

---

## 1. Hướng dẫn nạp Cụm Master (ESP32-S3 + Màn hình ST7789 + Relay)

1. Mở Terminal trong Antigravity và nạp biến môi trường ESP-IDF:
   ```powershell
   Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass; . "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
   ```

2. Di chuyển vào thư mục Master:
   ```powershell
   cd C:\Users\ACER\.gemini\antigravity-ide\scratch\smart_dual_camera_access_idf\master_esp32s3
   ```

3. Đặt chip mục tiêu và biên dịch:
   ```powershell
   idf.py set-target esp32s3
   idf.py build
   ```

4. Nạp vào bo ESP32-S3 (thay `COM4` bằng cổng của bạn):
   ```powershell
   idf.py -p COM4 flash monitor
   ```

---

## 2. Hướng dẫn nạp Cụm Slave (ESP32-CAM + Camera OV3660)

1. Nối chân **`IO0`** vào **`GND`** của ESP32-CAM và cắm mạch nạp USB-UART vào máy tính.
2. Di chuyển vào thư mục Slave:
   ```powershell
   cd C:\Users\ACER\.gemini\antigravity-ide\scratch\smart_dual_camera_access_idf\slave_esp32cam
   ```
3. Đặt chip mục tiêu và biên dịch:
   ```powershell
   idf.py set-target esp32
   idf.py build
   ```
4. Nạp vào bo ESP32-CAM (thay `COMx` bằng cổng COM của mạch nạp):
   ```powershell
   idf.py -p COMx flash monitor
   ```
5. **Sau khi nạp xong 100%**: Rút chân `IO0` khỏi `GND` và nhấn nút `Reset` trên bo ESP32-CAM.

---

## 3. Chạy Central Server AI (Python)

```powershell
cd C:\Users\ACER\.gemini\antigravity-ide\scratch\smart_dual_camera_access\central_server_python
python server.py
```

---

## 4. Cách Test hệ thống:
1. Mở cửa sổ Serial Monitor của **Slave (ESP32-CAM)**.
2. Gõ phím bất kỳ (ví dụ số `1`) và nhấn **Enter**.
3. ESP32-CAM bắn ESP-NOW sang ESP32-S3 $\rightarrow$ ESP32-S3 sáng màn hình chuẩn bị $\rightarrow$ Hai camera cùng chụp ảnh góc kép và gửi về Server Python $\rightarrow$ Server nhận diện và phản hồi mở khóa Relay!
