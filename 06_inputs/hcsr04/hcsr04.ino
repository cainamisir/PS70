// HC-SR04 Ultrasonic Distance Sensor — Heltec WiFi LoRa 32 V3
// No delay() anywhere. millis() paces the trigger; echo pin ISR times the pulse.

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Heltec V3 OLED: SSD1306 128x64, I2C on SDA=17 SCL=18, reset on GPIO 21
Adafruit_SSD1306 display(128, 64, &Wire, 21);

// ─── Pin assignments ──────────────────────────────────────────────────────────
static const uint8_t TRIG_PIN = 4;
static const uint8_t ECHO_PIN = 5;
static const uint8_t LED_PIN  = 35;

// ─── Forward declaration ──────────────────────────────────────────────────────
void IRAM_ATTR globalEchoISR();

// ─── HCSR04 Class ────────────────────────────────────────────────────────────

class HCSR04 {
public:
    static constexpr float CM_PER_US = 0.01717f;

    HCSR04(uint8_t trig, uint8_t echo, uint8_t led)
        : _trig(trig), _echo(echo), _led(led),
          _pulseStart(0), _pulseDuration(0), _triggerTime(0),
          _measuring(false), _newData(false), _distanceCm(-1.0f) {}

    void begin() {
        pinMode(_trig, OUTPUT);
        pinMode(_echo, INPUT);
        digitalWrite(_trig, LOW);
        ledcAttach(_led, 5000, 8);
        attachInterrupt(digitalPinToInterrupt(_echo), globalEchoISR, CHANGE);
    }

    void trigger() {
        // If no echo after 30 ms (beyond 400 cm range), reset and try again.
        if (_measuring && (micros() - _triggerTime > 30000)) {
            _measuring = false;
        }
        if (_measuring) return;
        _measuring = true;
        _triggerTime = micros();
        digitalWrite(_trig, HIGH);
        delayMicroseconds(10);
        digitalWrite(_trig, LOW);
    }

    void IRAM_ATTR echoHandler() {
        if (digitalRead(_echo) == HIGH) {
            _pulseStart = micros();
        } else {
            _pulseDuration = micros() - _pulseStart;
            _measuring = false;
            _newData = true;
        }
    }

    bool update() {
        if (!_newData) return false;
        _newData = false;
        float raw = _pulseDuration * CM_PER_US;
        if (raw >= 2.0f && raw <= 400.0f) {
            // Linear calibration fit from measured data (valid 10–50 cm):
            //   actual ≈ 1.023 × raw + 0.68
            _distanceCm = 1.023f * raw + 0.68f;
            int brightness = constrain(map((long)_distanceCm, 2, 50, 255, 0), 0, 255);
            ledcWrite(_led, brightness);
            return true;
        }
        return false;
    }

    float getDistanceCm() const { return _distanceCm; }

private:
    uint8_t _trig, _echo, _led;
    volatile unsigned long _pulseStart, _pulseDuration, _triggerTime;
    volatile bool _measuring, _newData;
    float _distanceCm;
};

// ─── Globals ──────────────────────────────────────────────────────────────────

HCSR04 sensor(TRIG_PIN, ECHO_PIN, LED_PIN);

void IRAM_ATTR globalEchoISR() { sensor.echoHandler(); }

// ─── Arduino entry points ─────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    Wire.begin(17, 18);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.println("HC-SR04");
    display.println("ready");
    display.display();

    sensor.begin();
}

void loop() {
    static unsigned long lastTrigger = 0;
    if (millis() - lastTrigger >= 100) {
        lastTrigger = millis();
        sensor.trigger();
    }

    if (sensor.update()) {
        float d = sensor.getDistanceCm();

        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("Distance:");
        display.setTextSize(3);
        display.setCursor(0, 16);
        display.print(d, 1);
        display.println(" cm");
        display.display();

        Serial.print("Distance_cm\t");
        Serial.println(d, 1);
    }
}
