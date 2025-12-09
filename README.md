# Calendar for the Dragon 🐉

An e-paper calendar and memo display system for the LilyGo-EPD47 (ESP32-based 960×540 e-ink display).

## Features

- **WiFi Access Point Setup**: Device creates its own WiFi network on first boot
- **WiFi QR Code**: Scan to automatically connect to WiFi (standard WIFI: format)
- **Captive Portal**: Auto-redirects to setup page when connected (no manual URL entry needed)
- **Web-Based Configuration**: Beautiful responsive web interface for designing and uploading content
- **Split Display Layout**:
  - Calendar area (640×540 pixels) - 2/3 of display
  - Memo area (320×540 pixels) - 1/3 of display
- **Image Support**: Upload custom calendar and memo images via phone browser
- **Visual Editor**: Draw calendar grids, add text, and upload background images
- **Deep Sleep**: Ultra-low power consumption when not in use
- **Button Wake**: Press button to re-enter setup mode

## Hardware Requirements

- LilyGo T5 4.7" EPD (EPD47) board
- ESP32 with PSRAM enabled
- Built-in button on GPIO 39

## How It Works

### First Boot / Setup Mode

1. **Power On**: Device boots and checks if content exists
2. **QR Code Display**: Shows WiFi QR code with connection info
   - SSID: `CalendarDragon`
   - No password required (open network)
   - QR code position randomized to prevent e-ink burn-in
3. **Connect**:
   - **Easiest**: Scan the WiFi QR code - auto-connects and opens browser
   - **Alternative**: Manually connect to `CalendarDragon` network - captive portal auto-redirects
   - **Manual**: Browse to `http://192.168.4.1`
4. **Configure**: Web interface opens automatically (captive portal)
5. **Upload**: Design and upload calendar/memo content
6. **Auto-Sleep**: After 2 minutes or successful upload, device enters deep sleep

### Normal Operation

1. **Wake Up**: Device wakes from deep sleep
2. **Display Content**: Shows previously uploaded calendar and memo
3. **Sleep**: Immediately returns to deep sleep
4. **Button Wake**: Press GPIO 39 button to re-enter setup mode

## Web Interface Features

The web interface ([index.html](data/index.html)) provides:

### Calendar Section (640×540)
- Text input for month/title
- Color picker for text
- Background image upload
- Visual calendar grid drawing
- Preview before upload

### Memo Section (320×540)
- Text input for memo title
- Color picker for text
- Background image upload
- Preview before upload

### Smart Features
- Real-time canvas preview
- Automatic 4-bit grayscale conversion
- Progress indicators
- Responsive mobile-friendly design
- Image format conversion (supports PNG, JPEG, etc.)

## File Structure

```
calendar-for-the-dragon/
├── src/
│   ├── calendar-for-the-dragon.cc  # Main application code
│   ├── qrcode.h                    # QR code header (using qrcodegen library)
│   └── qrcode.cc                   # QR code implementation
├── data/
│   └── index.html                  # Web interface (served via SPIFFS)
├── lib/
│   └── src/                        # EPD47 library (display driver)
├── platformio.ini                  # PlatformIO configuration
└── README.md                       # This file
```

## Building and Uploading

### Using PlatformIO

1. **Install PlatformIO**: Follow [PlatformIO installation guide](https://platformio.org/install)

2. **Build the project**:
   ```bash
   pio run
   ```

3. **Upload filesystem** (web interface):
   ```bash
   pio run --target uploadfs
   ```

4. **Upload firmware**:
   ```bash
   pio run --target upload
   ```

5. **Monitor serial output**:
   ```bash
   pio device monitor
   ```

### Configuration

Edit [platformio.ini](platformio.ini) if needed:
- Upload speed: `921600` (adjust if you have connection issues)
- Monitor speed: `115200`
- Board: `lilygo-t5-47`

## Code Architecture

### State Machine

The device operates in multiple states:

1. **STATE_FIRST_BOOT**: Initial startup, check for existing content
2. **STATE_SHOW_QR**: Display QR code and connection info
3. **STATE_WAIT_UPLOAD**: Wait for user to upload content (2min timeout)
4. **STATE_DISPLAY_CONTENT**: Show uploaded calendar/memo
5. **STATE_SLEEP**: Enter deep sleep mode

### Key Components

#### Display Management ([calendar-for-the-dragon.cc](src/calendar-for-the-dragon.cc))
- `initDisplay()`: Initialize EPD47 display and allocate framebuffer
- `drawQRCode()`: Generate and display QR code using qrcodegen library
- `drawContent()`: Load and display calendar/memo from SPIFFS
- `drawCalendarLayout()`: Draw placeholder calendar grid
- `drawText()` / `drawCenteredText()`: Text rendering utilities

#### Network & Web Server
- `initWiFiAP()`: Create WiFi access point
- `initWebServer()`: Setup async web server with endpoints
- `handleFileUpload()`: Process uploaded binary image files

#### Storage
- **SPIFFS**: Store uploaded images and web interface
  - `/calendar.bin`: Calendar image (4-bit grayscale)
  - `/memo.bin`: Memo image (4-bit grayscale)
  - `/index.html`: Web interface
- **Preferences**: Store device state (initialized flag)

#### Power Management
- `enterDeepSleep()`: Clean up and enter deep sleep
- Wake on GPIO 39 button press
- Optional: Timer wake-up (commented out, can enable for daily refresh)

### Image Format

Images are stored in 4-bit grayscale format:
- 2 pixels per byte
- Values: 0-15 (0=black, 15=white)
- Calendar: 640×540 = 172,800 bytes
- Memo: 320×540 = 86,400 bytes

The web interface automatically converts any image format to this format.

## Customization

### Modify Layout

Edit [calendar-for-the-dragon.cc](src/calendar-for-the-dragon.cc:353-405):

```cpp
// Change calendar/memo width ratio
int calendar_width = (DISPLAY_WIDTH * 2) / 3;  // Current: 2/3
int memo_width = DISPLAY_WIDTH - calendar_width;  // Current: 1/3
```

### Change WiFi Settings

Edit [calendar-for-the-dragon.cc](src/calendar-for-the-dragon.cc:15-16):

```cpp
#define AP_SSID "CalendarDragon"      // Change WiFi name
#define AP_TIMEOUT_MS 120000           // Change timeout (ms)
```

### Modify Display Behavior

Uncomment timer wake-up for daily refresh:

```cpp
// Wake up once per day to refresh
esp_sleep_enable_timer_wakeup(24ULL * 60 * 60 * 1000000); // 24 hours
```

## API Endpoints

The web server provides these endpoints:

- `GET /` - Serve main web interface
- `GET /status` - Get device status and display info
- `POST /upload/calendar` - Upload calendar image
- `POST /upload/memo` - Upload memo image

## Troubleshooting

### Display shows nothing
- Check PSRAM is enabled in board configuration
- Verify EPD47 connections
- Check serial monitor for error messages

### Can't connect to WiFi
- Look for "CalendarDragon" network
- Device IP is usually `192.168.4.1`
- Check QR code on display for exact URL

### Upload fails
- Ensure images are converted to correct format by web interface
- Check SPIFFS has enough space
- Monitor serial output for upload progress

### Display shows old content
- Press button (GPIO 39) to enter setup mode
- Upload new content via web interface
- Device will show new content after timeout

## Future Enhancements

Potential features to add:
- [ ] Password-protected WiFi AP
- [ ] Multiple calendar layouts
- [ ] Weather integration
- [ ] Google Calendar sync
- [ ] Battery status display
- [ ] Touch input support
- [ ] Animated transitions
- [ ] Cloud storage integration

## License

This project uses:
- LilyGo EPD47 library (from LilyGo)
- QR Code generator library by Nayuki (MIT License)
- ESP Async WebServer (LGPL)

## Credits

Built for the LilyGo T5 4.7" EPD47 e-paper display board.

Hardware: [LilyGo-EPD47](https://github.com/Xinyuan-LilyGO/LilyGo-EPD47)
