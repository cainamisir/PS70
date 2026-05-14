#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <FS.h>
#include <RadioLib.h>
#include <math.h> 

// --- PINS ---
#define SD_CS_PIN     26
#define SD_MOSI_PIN   46  
#define SD_MISO_PIN   33
#define SD_SCK_PIN    41
#define BUTTON_PIN    42
#define LED_PIN       35
#define I2S_BCK_PIN   4
#define I2S_WS_PIN    5
#define I2S_SD_PIN    6

// --- LORA PINS ---
#define LORA_NSS      8
#define LORA_DIO1     14
#define LORA_RST      12
#define LORA_BUSY     13

// --- HW-484 SOUND SENSOR ---
#define HW484_AO      7   // Analog output
#define HW484_DO      2   // Digital output (RTC GPIO for wakeup)

// --- SETTINGS ---
#define RECORD_TIME   10      // Seconds
#define SAMPLE_RATE   16000   // 16kHz (Phone quality - faster transmission)
#define GAIN_BOOST    1       // No gain boost
#define BUFFER_SAMPLES 1024
#define COOLDOWN_SEC  10      // Seconds to wait after sending before listening again   

SPIClass sdSPI(HSPI); 
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

File file;
int fileIndex = 1;
char filename[32];
unsigned long lastHW484Log = 0;
RTC_DATA_ATTR int bootCount = 0;

// Global buffers to avoid stack overflow
int32_t i2sBuffer[BUFFER_SAMPLES];
int16_t outBuffer[BUFFER_SAMPLES];

// IMA ADPCM state
int16_t adpcmPredicted = 0;
int8_t adpcmIndex = 0;

// IMA ADPCM step table
const int16_t stepTable[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
  253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
  3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
  12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

const int8_t indexTable[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
};

const i2s_config_t i2s_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = SAMPLE_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Mono
  .communication_format = I2S_COMM_FORMAT_STAND_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 8,
  .dma_buf_len = 1024,
  .use_apll = false,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};

const i2s_pin_config_t pin_config = {
  .bck_io_num = I2S_BCK_PIN,
  .ws_io_num = I2S_WS_PIN,
  .data_out_num = I2S_PIN_NO_CHANGE,
  .data_in_num = I2S_SD_PIN
};

uint8_t encodeIMA(int16_t sample) {
  int16_t step = stepTable[adpcmIndex];
  int16_t diff = sample - adpcmPredicted;
  int16_t value = 0;

  if (diff < 0) {
    value = 8;
    diff = -diff;
  }

  int16_t vpdiff = (step >> 3);
  if (diff >= step) {
    value |= 4;
    diff -= step;
    vpdiff += step;
  }
  step >>= 1;
  if (diff >= step) {
    value |= 2;
    diff -= step;
    vpdiff += step;
  }
  step >>= 1;
  if (diff >= step) {
    value |= 1;
    vpdiff += step;
  }

  if (value & 8)
    adpcmPredicted -= vpdiff;
  else
    adpcmPredicted += vpdiff;

  if (adpcmPredicted > 32767) adpcmPredicted = 32767;
  if (adpcmPredicted < -32768) adpcmPredicted = -32768;

  adpcmIndex += indexTable[value];
  if (adpcmIndex < 0) adpcmIndex = 0;
  if (adpcmIndex > 88) adpcmIndex = 88;

  return value & 0x0F;
}

void enterDeepSleep() {
  Serial.println("\n=== Entering Deep Sleep ===");
  Serial.println("Will wake on loud sound (HW-484 HIGH)");
  Serial.flush();
  delay(100);

  // Configure wakeup on sound sensor HIGH
  esp_sleep_enable_ext0_wakeup((gpio_num_t)HW484_DO, 1);

  // Enter deep sleep
  esp_deep_sleep_start();
}

void printWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  Serial.print("Boot #");
  Serial.print(bootCount);
  Serial.print(" - Wakeup: ");

  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("SOUND DETECTED!");
      break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
      Serial.println("Power-on or reset");
      break;
  }
}

void logHW484() {
  // Take multiple readings and average for stability
  long sum = 0;
  for(int i = 0; i < 10; i++) {
    sum += analogRead(HW484_AO);
    delayMicroseconds(100);
  }
  int analogValue = sum / 10;

  // Convert to approximate dB (relative scale, not calibrated SPL)
  float dB = 20.0 * log10(analogValue + 1) - 20.0;  // Offset to reasonable range
  if (dB < 0) dB = 0;

  int digitalValue = digitalRead(HW484_DO);

  Serial.print("HW-484 -> ");
  Serial.print(dB, 1);
  Serial.print(" dB | Digital: ");
  Serial.println(digitalValue == HIGH ? "HIGH (SOUND!)" : "LOW (quiet)");
}

void recordAudio() {
  while (SD.exists("/hq" + String(fileIndex) + ".adpcm")) fileIndex++;
  sprintf(filename, "/hq%d.adpcm", fileIndex);

  Serial.print("Recording to: "); Serial.println(filename);
  logHW484();  // Log sound level before recording

  file = SD.open(filename, FILE_WRITE);
  if (!file) { Serial.println("SD Error"); return; }

  // Reset IMA ADPCM state
  adpcmPredicted = 0;
  adpcmIndex = 0;

  // Write IMA ADPCM header (predicted value and index)
  file.write((uint8_t*)&adpcmPredicted, 2);
  file.write((uint8_t*)&adpcmIndex, 1);
  file.write((uint8_t)0);  // Padding

  digitalWrite(LED_PIN, HIGH);
  unsigned long startT = millis();

  unsigned long totalSamplesNeeded = SAMPLE_RATE * RECORD_TIME;
  unsigned long samplesSaved = 0;
  size_t bytesRead;
  uint8_t adpcmBuffer[BUFFER_SAMPLES / 2];  // 4-bit samples, 2 per byte

  while (samplesSaved < totalSamplesNeeded) {
    i2s_read(I2S_NUM_0, (void *)i2sBuffer, sizeof(i2sBuffer), &bytesRead, portMAX_DELAY);

    int samplesCount = bytesRead / 4;

    // Convert to 16-bit
    for (int i = 0; i < samplesCount; i++) {
      outBuffer[i] = (int16_t)(i2sBuffer[i] >> 14);
    }

    // Compress to IMA ADPCM (two 4-bit samples per byte)
    int adpcmCount = 0;
    for (int i = 0; i < samplesCount; i += 2) {
      uint8_t nibble1 = encodeIMA(outBuffer[i]);
      uint8_t nibble2 = (i + 1 < samplesCount) ? encodeIMA(outBuffer[i + 1]) : 0;
      adpcmBuffer[adpcmCount++] = (nibble2 << 4) | nibble1;
    }

    file.write(adpcmBuffer, adpcmCount);
    samplesSaved += samplesCount;
  }

  file.close();
  digitalWrite(LED_PIN, LOW);

  unsigned long duration = millis() - startT;
  Serial.print("Done. Actual Duration: "); Serial.print(duration); Serial.println(" ms");
  Serial.print("Compressed size: "); Serial.print(file.size()); Serial.println(" bytes");
  Serial.println("(Should be close to 10000 ms)");
  logHW484();  // Log sound level after recording
}

void sendFile() {
  Serial.println("Starting LoRa...");
  file = SD.open(filename, FILE_READ);
  if (!file) return;
  
  // Header - Send multiple times to ensure receiver catches it
  String header = "START|" + String(filename) + "|" + String(file.size());
  Serial.println("Sending START header...");
  for(int i = 0; i < 10; i++) {
    radio.transmit(header);
    delay(200);
  }
  Serial.println("Starting data transmission...");
  delay(500); 

  // Data
  uint8_t txBuffer[250];
  int packetCount = 0;
  while (file.available()) {
    int bytesRead = file.read(txBuffer, 250);
    radio.transmit(txBuffer, bytesRead);
    delay(6);  // Reduced delay for 300 kbps

    packetCount++;
    if (packetCount % 100 == 0) {
      Serial.print("Sent "); Serial.print(packetCount); Serial.println(" packets");
    }
  }
  
  file.close();
  
  // Robust Footer
  Serial.println("Data sent. Sending Footer...");
  delay(500); 
  for(int i=0; i<5; i++) {
    radio.transmit("END");
    delay(50);
  }
  Serial.println("Sent!");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  bootCount++;
  printWakeupReason();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(HW484_DO, INPUT_PULLDOWN);  // GPIO 10 with pulldown

  // Configure ADC for 3.3V full scale
  analogSetAttenuation(ADC_11db);  // 0-3.3V range
  
  // FSK Mode (Maximum speed!)
  if (radio.beginFSK(915.0, 300.0, 150.0, 467.0) == RADIOLIB_ERR_NONE) {
    // freq, bitrate, freqDev, rxBW
    // 300 kbps - maximum speed
    radio.setOutputPower(22);  // Max power: +22 dBm
    radio.setDataShaping(RADIOLIB_SHAPING_0_5);
    radio.setSyncWord((uint8_t[]){0xA5, 0xA5}, 2);
    Serial.println("FSK 300kbps OK @ +22dBm");
  } else {
    Serial.println("Radio Failed");
  }

  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, sdSPI)) Serial.println("SD Failed");

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_zero_dma_buffer(I2S_NUM_0);

  // Check wakeup reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    // Woke from sound - record and send
    Serial.println("Sound triggered! Recording...");
    logHW484();
    recordAudio();
    sendFile();

    // Cooldown
    Serial.print("Cooldown ");
    Serial.print(COOLDOWN_SEC);
    Serial.println("s...");
    delay(COOLDOWN_SEC * 1000);

    // Back to sleep
    enterDeepSleep();
  } else {
    // First boot - check for button, then sleep
    Serial.println("Ready. Press button for manual record, or will sleep in 5s...");
    logHW484();
  }
}

void loop() {
  static unsigned long bootTime = millis();

  // Check for button press (5 second window after boot)
  if (millis() - bootTime < 5000) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      delay(200);  // Debounce
      Serial.println("\n=== BUTTON TRIGGERED ===");
      recordAudio();
      sendFile();

      // Cooldown
      Serial.print("Cooldown ");
      Serial.print(COOLDOWN_SEC);
      Serial.println("s...");
      delay(COOLDOWN_SEC * 1000);

      while (digitalRead(BUTTON_PIN) == LOW);  // Wait for release

      // Back to sleep
      enterDeepSleep();
    }
    delay(100);
  } else {
    // 5 seconds passed, no button press - enter sleep
    enterDeepSleep();
  }
}