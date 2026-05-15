// Week 4 — Weather + Time Display
// Heltec WiFi LoRa 32 V3 (ESP32-S3)
//
// Shows current weather and local time for Cambridge MA or Bucharest RO.
// Weather from wttr.in (no API key needed). Time from NTP.
// Built-in PRG button (GPIO 0) toggles between cities.
// Built-in LED (GPIO 35) pulses while fetching.
//
// Libraries needed (install via Library Manager):
//   - Adafruit SSD1306
//   - Adafruit GFX
//   - ArduinoJson  (v7 by Benoit Blanchon)

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// ── WiFi credentials ──────────────────────────────────────────────────────────
const char* WIFI_SSID = "MAKERSPACE";
const char* WIFI_PASS = "12345678";

// ── Cities ────────────────────────────────────────────────────────────────────
struct City {
    const char* label;   // shown on display
    const char* wttr;    // wttr.in query string
    const char* tz;      // POSIX timezone string
};

const City CITIES[] = {
    { "Cambridge  MA", "Cambridge+MA+USA",  "EST5EDT,M3.2.0,M11.1.0"        },
    { "Bucharest  RO", "Bucharest+Romania", "EET-2EEDT,M3.5.0/3,M10.5.0/4"  }
};

int cityIdx = 0;  // currently displayed city

// ── Hardware ──────────────────────────────────────────────────────────────────
static const uint8_t BUTTON_PIN = 0;   // PRG/boot button — active LOW
static const uint8_t LED_PIN    = 35;  // built-in white LED

// Heltec V3 OLED: SSD1306 128x64, I2C SDA=17 SCL=18, reset GPIO 21
Adafruit_SSD1306 display(128, 64, &Wire, 21);

// ── Weather state ─────────────────────────────────────────────────────────────
struct Weather {
    int  tempC;
    int  tempF;
    int  humidity;
    char desc[22];   // truncated to fit one OLED line
    bool valid;
};

Weather wx = {};

// ── Timing ────────────────────────────────────────────────────────────────────
unsigned long lastFetch   = 0;
unsigned long lastDraw    = 0;
bool          prevBtn     = HIGH;
unsigned long lastBtnTime = 0;

static const unsigned long FETCH_INTERVAL = 5UL * 60UL * 1000UL;  // 5 min
static const unsigned long DRAW_INTERVAL  = 1000UL;                // 1 s

// ── Helpers ───────────────────────────────────────────────────────────────────

void applyTimezone() {
    setenv("TZ", CITIES[cityIdx].tz, 1);
    tzset();
}

void fetchWeather() {
    if (WiFi.status() != WL_CONNECTED) return;

    ledcWrite(LED_PIN, 80);  // dim LED on during fetch

    WiFiClientSecure client;
    client.setInsecure();    // skip cert check — fine for a weather display

    HTTPClient http;
    String url = String("https://wttr.in/") + CITIES[cityIdx].wttr + "?format=j1";
    http.begin(client, url);
    http.setTimeout(10000);

    if (http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getStream())) {
            auto cc     = doc["current_condition"][0];
            wx.tempC    = cc["temp_C"].as<int>();
            wx.tempF    = cc["temp_F"].as<int>();
            wx.humidity = cc["humidity"].as<int>();
            const char* d = cc["weatherDesc"][0]["value"];
            strncpy(wx.desc, d ? d : "---", 21);
            wx.desc[21] = '\0';
            wx.valid    = true;
        }
    }

    http.end();
    ledcWrite(LED_PIN, 0);
    lastFetch = millis();
}

void drawDisplay() {
    struct tm t;
    bool gotTime = getLocalTime(&t, 100);

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    // ── Row 1: city name ─────────────────────────────────────────────────────
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(CITIES[cityIdx].label);

    // ── Row 2: temperature ───────────────────────────────────────────────────
    display.setCursor(0, 12);
    if (wx.valid) {
        display.print(wx.tempC);
        display.print("C  /  ");
        display.print(wx.tempF);
        display.print("F");
    } else {
        display.print("Fetching...");
    }

    // ── Row 3: condition ─────────────────────────────────────────────────────
    display.setCursor(0, 24);
    if (wx.valid) display.print(wx.desc);

    // ── Row 4: humidity ──────────────────────────────────────────────────────
    display.setCursor(0, 36);
    if (wx.valid) {
        display.print("Humidity: ");
        display.print(wx.humidity);
        display.print("%");
    }

    // ── Row 5: time (large) ──────────────────────────────────────────────────
    display.setTextSize(2);
    display.setCursor(0, 48);
    if (gotTime) {
        char buf[6];
        strftime(buf, sizeof(buf), "%H:%M", &t);
        display.print(buf);
    } else {
        display.print("--:--");
    }

    display.display();
}

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    Wire.begin(17, 18);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Connecting WiFi...");
    display.display();

    ledcAttach(LED_PIN, 5000, 8);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // Connect — blocking in setup() is fine
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(300);
    }

    if (WiFi.status() == WL_CONNECTED) {
        configTime(0, 0, "pool.ntp.org");  // sync UTC, then apply local tz
        applyTimezone();

        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Getting weather...");
        display.display();

        fetchWeather();
    } else {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("WiFi failed.");
        display.println("Check credentials.");
        display.display();
    }
}

void loop() {
    unsigned long now = millis();

    // ── Button: toggle city (active LOW, 300 ms debounce) ────────────────────
    bool btn = digitalRead(BUTTON_PIN);
    if (btn == LOW && prevBtn == HIGH && (now - lastBtnTime > 300)) {
        lastBtnTime = now;
        cityIdx = (cityIdx + 1) % 2;
        applyTimezone();
        wx.valid = false;   // clear stale data
        drawDisplay();      // show "Fetching..." right away
        fetchWeather();
    }
    prevBtn = btn;

    // ── Refresh weather every 5 minutes ──────────────────────────────────────
    if (now - lastFetch >= FETCH_INTERVAL) fetchWeather();

    // ── Redraw every second (keeps time ticking) ──────────────────────────────
    if (now - lastDraw >= DRAW_INTERVAL) {
        lastDraw = now;
        drawDisplay();
    }
}
