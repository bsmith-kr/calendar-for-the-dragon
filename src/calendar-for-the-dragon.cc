#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM !!!"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <qrcode.h>

#include "epd_driver.h"
#include "font/firasans.h"

// Configuration
#define AP_SSID "CalendarDragon"
#define AP_PASSWORD ""  // Empty for open network, or set a password
#define AP_TIMEOUT_MS 5*60*1000  // 5 minutes timeout for AP mode
#define WAKEUP_BUTTON_PIN 39

// File paths in SPIFFS
#define CALENDAR_IMAGE_PATH "/calendar.bin"
#define MEMO_IMAGE_PATH "/memo.bin"
#define CONFIG_PATH "/config.txt"

// Display configuration
#define DISPLAY_WIDTH EPD_WIDTH   // 960
#define DISPLAY_HEIGHT EPD_HEIGHT // 540

// State machine
enum DeviceState {
    STATE_FIRST_BOOT,
    STATE_SHOW_QR,
    STATE_WAIT_UPLOAD,
    STATE_DISPLAY_CONTENT,
    STATE_SLEEP
};

// Global variables
Preferences preferences;
AsyncWebServer server(80);
DNSServer dnsServer;
uint8_t *framebuffer = nullptr;
DeviceState currentState = STATE_FIRST_BOOT;
unsigned long apStartTime = 0;
bool contentUploaded = false;

// Function declarations
void initDisplay();
void initWiFiAP();
void initWebServer();
void initSPIFFS();
void drawQRCode(const char* qrData, const char* displayUrl);
void drawContent();
void drawCalendarLayout();
void drawText(const char* text, int x, int y, const GFXfont* font);
void drawCenteredText(const char* text, int y, const GFXfont* font);
void enterDeepSleep();
bool hasStoredContent();
void handleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);

void setup()
{
    Serial.begin(115200);
    delay(100);

    log_i("=== Calendar for the Dragon Starting ===");

    // Initialize display
    initDisplay();

    // Initialize SPIFFS for file storage
    initSPIFFS();

    // Initialize preferences
    bool opened = preferences.begin("calendar", false);
    if (!opened) {
        log_e("Failed to open preferences");
        return;
    }

    // Check wake-up cause
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    bool isFirstBoot = preferences.getBool("initialized", false) == false;

    log_i("Wake-up cause: %d, First boot: %d", wakeup_reason, isFirstBoot);

    // Determine state based on boot reason
    if (isFirstBoot || !hasStoredContent()) {
        currentState = STATE_SHOW_QR;
        log_i("State: SHOW_QR (first boot or no content)");
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
        // Button pressed - enter setup mode
        currentState = STATE_SHOW_QR;
        log_i("State: SHOW_QR (button pressed)");
    } else {
        // Normal wake-up - show content
        currentState = STATE_DISPLAY_CONTENT;
        log_i("State: DISPLAY_CONTENT");
    }

    // State machine execution
    switch (currentState) {
        case STATE_SHOW_QR:
        {
            epd_poweron();
            epd_clear();

            // Start WiFi AP
            initWiFiAP();

            // Initialize web server
            initWebServer();

            // Get IP address
            String ip = WiFi.softAPIP().toString();
            log_i("AP IP: %s", ip.c_str());

            // Start DNS server for captive portal (redirect all DNS requests to our IP)
            dnsServer.start(53, "*", WiFi.softAPIP());
            log_i("DNS server started for captive portal");

            // Create WiFi QR code string (standard format for automatic WiFi connection)
            // Format: WIFI:T:<WPA|WEP|nopass>;S:<SSID>;P:<password>;H:<hidden>;;
            String qrData;
            if (strlen(AP_PASSWORD) == 0) {
                // Open network (no password)
                qrData = "WIFI:T:nopass;S:" + String(AP_SSID) + ";;";
            } else {
                // WPA network with password
                qrData = "WIFI:T:WPA;S:" + String(AP_SSID) + ";P:" + String(AP_PASSWORD) + ";;";
            }

            log_i("QR Data: %s", qrData.c_str());

            // Create URL with http:// prefix
            String url = "http://" + ip;

            // Draw WiFi QR code (includes text labels)
            drawQRCode(qrData.c_str(), url.c_str());

            epd_poweroff();

            // Mark as initialized
            preferences.putBool("initialized", true);

            // Wait for upload with timeout
            apStartTime = millis();
            currentState = STATE_WAIT_UPLOAD;
            break;
        }

        case STATE_DISPLAY_CONTENT:
        {
            epd_poweron();
            epd_clear();
            drawContent();
            epd_poweroff();

            // Go to sleep immediately
            currentState = STATE_SLEEP;
            break;
        }

        default:
            break;
    }
}

void loop()
{
    switch (currentState) {
        case STATE_WAIT_UPLOAD:
            // Process DNS requests for captive portal
            dnsServer.processNextRequest();

            // Check timeout
            if (millis() - apStartTime > AP_TIMEOUT_MS) {
                log_i("AP timeout reached");

                if (contentUploaded || hasStoredContent()) {
                    // Show the content (new or existing)
                    epd_poweron();
                    epd_clear();
                    drawContent();
                    epd_poweroff();
                }

                currentState = STATE_SLEEP;
            }

            delay(100);
            break;

        case STATE_SLEEP:
            enterDeepSleep();
            break;

        default:
            delay(100);
            break;
    }
}

void initDisplay()
{
    log_i("Initializing display...");
    epd_init();

    // Allocate framebuffer
    framebuffer = (uint8_t *)heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 2, MALLOC_CAP_SPIRAM);
    if (!framebuffer) {
        log_e("Failed to allocate framebuffer");
    } else {
        memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    }
}

void initWiFiAP()
{
    log_i("Starting Access Point...");
    WiFi.mode(WIFI_AP);

    if (strlen(AP_PASSWORD) == 0) {
        WiFi.softAP(AP_SSID);  // Open network
        log_i("AP: %s (Open network)", AP_SSID);
    } else {
        WiFi.softAP(AP_SSID, AP_PASSWORD);  // Password-protected
        log_i("AP: %s (Password-protected)", AP_SSID);
    }

    delay(100);

    IPAddress IP = WiFi.softAPIP();
    log_i("AP IP address: %s", IP.toString().c_str());
}

void initWebServer()
{
    log_i("Initializing web server...");

    // Captive portal - redirect all requests to our page
    server.onNotFound([](AsyncWebServerRequest *request) {
        // For captive portal detection
        if (request->host() != WiFi.softAPIP().toString()) {
            request->redirect("http://" + WiFi.softAPIP().toString());
        } else {
            request->send(SPIFFS, "/index.html", "text/html");
        }
    });

    // Serve main page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/index.html", "text/html");
    });

    // Captive portal detection endpoints
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/");  // Android
    });

    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/");  // iOS
    });

    server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/");  // Windows
    });

    // Handle calendar image upload
    server.on("/upload/calendar", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            request->send(200, "text/plain", "Calendar uploaded successfully");
            contentUploaded = true;
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            handleFileUpload(request, CALENDAR_IMAGE_PATH, index, data, len, final);
        }
    );

    // Handle memo image upload
    server.on("/upload/memo", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            request->send(200, "text/plain", "Memo uploaded successfully");
            contentUploaded = true;
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            handleFileUpload(request, MEMO_IMAGE_PATH, index, data, len, final);
        }
    );

    // Status endpoint
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{\"status\":\"ready\",\"display\":{\"width\":" + String(DISPLAY_WIDTH) +
                      ",\"height\":" + String(DISPLAY_HEIGHT) + "}}";
        request->send(200, "application/json", json);
    });

    // Save memo settings endpoint
    server.on("/save-memo", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                // Start of upload - open file for writing
                File file = SPIFFS.open("/memo-settings.json", FILE_WRITE);
                if (file) {
                    file.write(data, len);
                    file.close();
                    log_i("Memo settings saved (%d bytes)", len);
                }
            }

            if (index + len == total) {
                // Upload complete
                request->send(200, "application/json", "{\"status\":\"saved\"}");
            }
        }
    );

    // Load memo settings endpoint
    server.on("/load-memo", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (SPIFFS.exists("/memo-settings.json")) {
            File file = SPIFFS.open("/memo-settings.json", FILE_READ);
            if (file) {
                String content = file.readString();
                file.close();
                request->send(200, "application/json", content);
                log_i("Memo settings loaded");
            } else {
                request->send(200, "application/json", "{\"title\":\"Memo\",\"contents\":\"\"}");
            }
        } else {
            request->send(200, "application/json", "{\"title\":\"Memo\",\"contents\":\"\"}");
        }
    });

    server.begin();
    log_i("Web server started");
}

void initSPIFFS()
{
    log_i("Mounting SPIFFS...");
    if (!SPIFFS.begin(true)) {
        log_e("SPIFFS Mount Failed");
        return;
    }

    size_t totalBytes = SPIFFS.totalBytes();
    size_t usedBytes = SPIFFS.usedBytes();
    log_i("SPIFFS: %d/%d bytes used", usedBytes, totalBytes);
}

void handleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
    static File uploadFile;

    if (index == 0) {
        log_i("Upload start: %s", filename.c_str());
        uploadFile = SPIFFS.open(filename, "w");
        if (!uploadFile) {
            log_e("Failed to open file for writing");
            return;
        }
    }

    if (uploadFile) {
        uploadFile.write(data, len);
    }

    if (final) {
        uploadFile.close();
        log_i("Upload complete: %s (%d bytes)", filename.c_str(), index + len);
    }
}

bool hasStoredContent()
{
    return SPIFFS.exists(CALENDAR_IMAGE_PATH) || SPIFFS.exists(MEMO_IMAGE_PATH);
}

void drawQRCode(const char* qrData, const char* displayUrl)
{
    if (!framebuffer) return;

    log_i("Drawing dual QR codes");

    // QR code settings
    int scale = 6;  // Each QR module is 6x6 pixels (smaller to fit two)
    int qr_spacing = 180;  // Space between the two QR codes (3x to prevent overlap)
    int top_margin = 60;

    // Create WiFi QR code
    QRCode qrcode_wifi;
    uint8_t qrcodeBytes_wifi[qrcode_getBufferSize(6)];
    int8_t result1 = qrcode_initText(&qrcode_wifi, qrcodeBytes_wifi, 6, ECC_LOW, qrData);

    // Create URL QR code
    QRCode qrcode_url;
    uint8_t qrcodeBytes_url[qrcode_getBufferSize(4)];
    int8_t result2 = qrcode_initText(&qrcode_url, qrcodeBytes_url, 4, ECC_LOW, displayUrl);

    if (result1 != 0 || result2 != 0) {
        log_e("QR code generation failed");
        return;
    }

    int qr_size_wifi = qrcode_wifi.size;
    int qr_pixel_size_wifi = qr_size_wifi * scale;

    int qr_size_url = qrcode_url.size;
    int qr_pixel_size_url = qr_size_url * scale;

    // Calculate positions (centered, side by side)
    int total_width = qr_pixel_size_wifi + qr_spacing + qr_pixel_size_url;
    int start_x = (DISPLAY_WIDTH - total_width) / 2;

    int wifi_x = start_x;
    int url_x = start_x + qr_pixel_size_wifi + qr_spacing;
    int y_offset = top_margin;

    log_i("QR codes position: WiFi x=%d, URL x=%d, y=%d", wifi_x, url_x, y_offset);

    // Draw WiFi QR code
    for (int py = y_offset - 20; py < y_offset + qr_pixel_size_wifi + 20; py++) {
        for (int px = wifi_x - 20; px < wifi_x + qr_pixel_size_wifi + 20; px++) {
            epd_draw_pixel(px, py, 255, framebuffer);
        }
    }

    for (int y = 0; y < qr_size_wifi; y++) {
        for (int x = 0; x < qr_size_wifi; x++) {
            if (qrcode_getModule(&qrcode_wifi, x, y)) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int px = wifi_x + x * scale + dx;
                        int py = y_offset + y * scale + dy;
                        epd_draw_pixel(px, py, 0, framebuffer);
                    }
                }
            }
        }
    }

    // Draw URL QR code
    for (int py = y_offset - 20; py < y_offset + qr_pixel_size_url + 20; py++) {
        for (int px = url_x - 20; px < url_x + qr_pixel_size_url + 20; px++) {
            epd_draw_pixel(px, py, 255, framebuffer);
        }
    }

    for (int y = 0; y < qr_size_url; y++) {
        for (int x = 0; x < qr_size_url; x++) {
            if (qrcode_getModule(&qrcode_url, x, y)) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int px = url_x + x * scale + dx;
                        int py = y_offset + y * scale + dy;
                        epd_draw_pixel(px, py, 0, framebuffer);
                    }
                }
            }
        }
    }

    // Draw text labels below each QR code
    int text_y = y_offset + max(qr_pixel_size_wifi, qr_pixel_size_url) + 30;

    // WiFi QR label
    drawText("WiFi Connect", wifi_x, text_y, &FiraSans);
    char ssid_text[64];
    snprintf(ssid_text, sizeof(ssid_text), "SSID: %s", AP_SSID);
    drawText(ssid_text, wifi_x, text_y + 35, &FiraSans);

    // URL QR label
    drawText("Web Interface", url_x, text_y, &FiraSans);
    drawText(displayUrl, url_x, text_y + 35, &FiraSans);

    // Draw framebuffer to display
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
}

void drawContent()
{
    if (!framebuffer) return;

    log_i("Drawing content...");
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    // Layout: Calendar on left, Memo on right
    int calendar_width = 540;
    int memo_width = DISPLAY_WIDTH - calendar_width;

    // Draw divider line
    epd_draw_vline(calendar_width, 0, DISPLAY_HEIGHT, 0x00, framebuffer);

    // Load and draw calendar image if exists
    if (SPIFFS.exists(CALENDAR_IMAGE_PATH)) {
        File file = SPIFFS.open(CALENDAR_IMAGE_PATH, "r");
        if (file) {
            size_t file_size = file.size();
            log_i("Loading calendar image (%d bytes)", file_size);

            // Expected size for calendar area (4-bit grayscale, 2 pixels per byte)
            size_t expected_size = (calendar_width * DISPLAY_HEIGHT) / 2;

            if (file_size == expected_size) {
                // Read calendar image data
                uint8_t *temp_buffer = (uint8_t *)malloc(expected_size);
                if (temp_buffer) {
                    file.read(temp_buffer, expected_size);

                    log_i("Calendar: %dx%d, bytes/row: src=%d dst=%d",
                          calendar_width, DISPLAY_HEIGHT,
                          calendar_width / 2, DISPLAY_WIDTH / 2);

                    // Copy to framebuffer row by row (calendar area is on left)
                    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
                        uint8_t *dst = framebuffer + (y * DISPLAY_WIDTH / 2);
                        uint8_t *src = temp_buffer + (y * calendar_width / 2);
                        memcpy(dst, src, calendar_width / 2);
                    }

                    free(temp_buffer);
                    log_i("Calendar image loaded successfully");
                } else {
                    log_e("Failed to allocate temp buffer for calendar");
                }
            } else {
                log_e("Calendar image size mismatch: expected %d, got %d", expected_size, file_size);
            }
            file.close();
        }
    } else {
        // Draw placeholder calendar
        drawCalendarLayout();
    }

    // Load and draw memo image if exists
    if (SPIFFS.exists(MEMO_IMAGE_PATH)) {
        File file = SPIFFS.open(MEMO_IMAGE_PATH, "r");
        if (file) {
            size_t file_size = file.size();
            log_i("Loading memo image (%d bytes)", file_size);

            size_t expected_size = (memo_width * DISPLAY_HEIGHT) / 2;

            if (file_size == expected_size) {
                // Read memo image data
                uint8_t *temp_buffer = (uint8_t *)malloc(expected_size);
                if (temp_buffer) {
                    file.read(temp_buffer, expected_size);

                    // Copy to framebuffer row by row (memo area is on right)
                    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
                        uint8_t *dst = framebuffer + (y * DISPLAY_WIDTH / 2) + (calendar_width / 2);
                        uint8_t *src = temp_buffer + (y * memo_width / 2);
                        memcpy(dst, src, memo_width / 2);
                    }

                    free(temp_buffer);
                    log_i("Memo image loaded successfully");
                } else {
                    log_e("Failed to allocate temp buffer for memo");
                }
            } else {
                log_e("Memo image size mismatch: expected %d, got %d", expected_size, file_size);
            }
            file.close();
        }
    } else {
        // Draw placeholder memo area
        int memo_x = calendar_width + 10;
        drawText("MEMO", memo_x, 40, &FiraSans);
        epd_draw_hline(memo_x, 60, memo_width - 20, 0x00, framebuffer);
    }

    // Draw to display
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
}

void drawCalendarLayout()
{
    // Draw a simple placeholder calendar layout
    const char* month = "December 2025";
    drawCenteredText(month, 40, &FiraSans);

    int cal_x = 50;
    int cal_y = 100;
    int cell_width = 80;
    int cell_height = 60;

    // Draw weekday headers
    const char* weekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (int i = 0; i < 7; i++) {
        drawText(weekdays[i], cal_x + i * cell_width, cal_y, &FiraSans);
    }

    // Draw grid lines
    for (int row = 0; row <= 6; row++) {
        int y = cal_y + 20 + row * cell_height;
        epd_draw_hline(cal_x, y, cell_width * 7, 0x00, framebuffer);
    }

    for (int col = 0; col <= 7; col++) {
        int x = cal_x + col * cell_width;
        epd_draw_vline(x, cal_y + 20, cell_height * 6, 0x00, framebuffer);
    }
}

void drawText(const char* text, int x, int y, const GFXfont* font)
{
    if (!framebuffer || !text) return;

    FontProperties props = {
        .fg_color = 0,
        .bg_color = 15,
        .fallback_glyph = 0,
        .flags = 0
    };

    Rect_t area = {
        .x = x,
        .y = y,
        .width = DISPLAY_WIDTH - x,
        .height = 100
    };

    write_mode(font, text, &area.x, &area.y, framebuffer, WHITE_ON_WHITE, &props);
}

void drawCenteredText(const char* text, int y, const GFXfont* font)
{
    if (!framebuffer || !text) return;

    int x1, y1, w, h;
    int cursor_x = 0, cursor_y = 0;
    get_text_bounds(font, text, &cursor_x, &cursor_y, &x1, &y1, &w, &h, NULL);

    int x = (DISPLAY_WIDTH - w) / 2;
    drawText(text, x, y, font);
}

void enterDeepSleep()
{
    log_i("Entering deep sleep...");

    // Clean up
    dnsServer.stop();
    server.end();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    if (framebuffer) {
        heap_caps_free(framebuffer);
        framebuffer = nullptr;
    }

    preferences.end();

    // Power off display
    epd_poweroff_all();

    // Configure wake-up on button press (GPIO 39)
    esp_sleep_enable_ext1_wakeup(GPIO_SEL_39, ESP_EXT1_WAKEUP_ALL_LOW);

    // Also wake up once per day to refresh (optional)
    // esp_sleep_enable_timer_wakeup(24ULL * 60 * 60 * 1000000); // 24 hours

    log_i("Good night!");
    delay(100);

    esp_deep_sleep_start();
}
