// Week 4 — Weather + Time Display
// Heltec WiFi LoRa 32 V3 (ESP32-S3)
//
// Shows weather + local time for Cambridge MA / Bucharest RO.
// PRG button (GPIO 0) toggles cities.
// External LED on GPIO 6 lights up when temp > 10 C.
//
// Libraries: Adafruit SSD1306, Adafruit GFX, ArduinoJson v7

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// ── Credentials ───────────────────────────────────────────────────────────────
const char* WIFI_SSID = "MAKERSPACE";
const char* WIFI_PASS = "12345678";

// ── Cities ────────────────────────────────────────────────────────────────────
struct City { const char* label; const char* wttr; const char* tz; };
const City CITIES[] = {
    { "Cambridge  MA", "Cambridge+MA+USA",  "EST5EDT,M3.2.0,M11.1.0"       },
    { "Bucharest  RO", "Bucharest+Romania", "EET-2EEDT,M3.5.0/3,M10.5.0/4" }
};
int cityIdx = 0;

// ── Pins ──────────────────────────────────────────────────────────────────────
static const uint8_t BUTTON_PIN  = 0;   // PRG button, active LOW
static const uint8_t LED_ONBOARD = 35;  // built-in white LED (pulses on fetch)
static const uint8_t LED_EXT     = 6;   // external LED: GPIO6 → 220Ω → LED → GND

Adafruit_SSD1306 display(128, 64, &Wire, 21);

// ── State ─────────────────────────────────────────────────────────────────────
struct Weather { int tempC, tempF, humidity; char desc[22]; bool valid; };
Weather wx = {};
unsigned long lastFetch = 0, lastDraw = 0, lastBtnTime = 0;
bool prevBtn = HIGH;

// ── Helpers ───────────────────────────────────────────────────────────────────

void applyTimezone() { setenv("TZ", CITIES[cityIdx].tz, 1); tzset(); }

void updateTempLED() {
    bool on = wx.valid && wx.tempC > 10;
    Serial.printf("LED: valid=%d tempC=%d → %s\n", wx.valid, wx.tempC, on ? "ON" : "OFF");
    digitalWrite(LED_EXT, on ? HIGH : LOW);
}

void fetchWeather() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("fetch: WiFi not connected");
        return;
    }

    ledcWrite(LED_ONBOARD, 80);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = String("https://wttr.in/") + CITIES[cityIdx].wttr + "?format=j1";
    Serial.println("Fetching: " + url);

    http.begin(client, url);
    http.setTimeout(12000);
    http.addHeader("User-Agent", "curl/7.68.0");  // wttr.in needs a real UA

    int code = http.GET();
    Serial.println("HTTP code: " + String(code));

    if (code == 200) {
        // Read full response into a String — more reliable than streaming on ESP32
        String body = http.getString();
        Serial.println("Body length: " + String(body.length()));

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err) {
            Serial.println("JSON error: " + String(err.c_str()));
        } else {
            auto cc     = doc["current_condition"][0];
            wx.tempC    = cc["temp_C"].as<int>();
            wx.tempF    = cc["temp_F"].as<int>();
            wx.humidity = cc["humidity"].as<int>();
            const char* d = cc["weatherDesc"][0]["value"];
            strncpy(wx.desc, d ? d : "---", 21);
            wx.desc[21] = '\0';
            wx.valid    = true;
            Serial.printf("Got: %dC / %dF  %s  hum:%d%%\n",
                          wx.tempC, wx.tempF, wx.desc, wx.humidity);
            updateTempLED();
        }
    } else {
        Serial.println("HTTP error: " + String(code));
    }

    http.end();
    ledcWrite(LED_ONBOARD, 0);
    lastFetch = millis();
}

void drawDisplay() {
    struct tm t;
    bool gotTime = getLocalTime(&t, 100);

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    display.setTextSize(1);
    display.setCursor(0,  0); display.print(CITIES[cityIdx].label);
    display.setCursor(0, 12);
    if (wx.valid) {
        display.print(wx.tempC); display.print("C  /  ");
        display.print(wx.tempF); display.print("F");
    } else {
        display.print("Fetching...");
    }
    display.setCursor(0, 24); if (wx.valid) display.print(wx.desc);
    display.setCursor(0, 36);
    if (wx.valid) {
        display.print("Humidity: "); display.print(wx.humidity); display.print("%");
    }

    display.setTextSize(2);
    display.setCursor(0, 48);
    if (gotTime) { char buf[6]; strftime(buf, sizeof(buf), "%H:%M", &t); display.print(buf); }
    else          display.print("--:--");

    display.display();
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);  // give serial monitor time to connect

    Wire.begin(17, 18);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Connecting WiFi...");
    display.display();

    ledcAttach(LED_ONBOARD, 5000, 8);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_EXT, OUTPUT);
    // Self-test: blink 3 times to verify wiring
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_EXT, HIGH); delay(200);
        digitalWrite(LED_EXT, LOW);  delay(200);
    }

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to " + String(WIFI_SSID));
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected. IP: " + WiFi.localIP().toString());
        configTime(0, 0, "pool.ntp.org");
        applyTimezone();

        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Getting weather...");
        display.display();

        fetchWeather();
    } else {
        Serial.println("WiFi failed!");
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("WiFi failed.");
        display.println(WIFI_SSID);
        display.display();
    }
}

void loop() {
    unsigned long now = millis();

    // Button toggle (active LOW, debounced)
    bool btn = digitalRead(BUTTON_PIN);
    if (btn == LOW && prevBtn == HIGH && (now - lastBtnTime > 300)) {
        lastBtnTime = now;
        cityIdx = (cityIdx + 1) % 2;
        applyTimezone();
        wx.valid = false;
        updateTempLED();
        drawDisplay();
        fetchWeather();
    }
    prevBtn = btn;

    // Refresh weather every 5 minutes
    if (now - lastFetch >= 300000UL) fetchWeather();

    // Redraw every second
    if (now - lastDraw >= 1000UL) { lastDraw = now; drawDisplay(); }
}
