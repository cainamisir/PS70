#include <Arduino.h>
#include <RadioLib.h>

// --- HELTEC V3 PINS ---
#define LORA_NSS      8
#define LORA_DIO1     14
#define LORA_RST      12
#define LORA_BUSY     13

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

bool receivingFile = false;
unsigned long startTime = 0;
unsigned long totalBytes = 0;
unsigned long lastStatTime = 0;
unsigned long lastPacketTime = 0;
float lastRSSI = 0;
float lastSNR = 0; 

void setup() {
  // CRITICAL CHANGE: Increased Baud Rate to 921600
  Serial.begin(921600);
  delay(1000);
  Serial.println("Receiver Initializing (High Speed Serial)...");

  int state = radio.beginFSK(915.0, 300.0, 150.0, 467.0);
  if (state == RADIOLIB_ERR_NONE) {
    // freq, bitrate, freqDev, rxBW
    radio.setDataShaping(RADIOLIB_SHAPING_0_5);
    radio.setSyncWord((uint8_t[]){0xA5, 0xA5}, 2);
    Serial.println("Radio Initialized (FSK 300kbps)!");
  } else {
    Serial.print("Failed: "); Serial.println(state);
    while(1);
  }
}

void loop() {
  uint8_t buffer[255];

  // Blocking receive
  int state = radio.receive(buffer, 255);

  if (state == RADIOLIB_ERR_NONE) {
    lastPacketTime = millis();
    int len = radio.getPacketLength();
    lastRSSI = radio.getRSSI();
    lastSNR = radio.getSNR();

    // Quick header check
    // We avoid using String() where possible for speed, but this is fine for headers
    if (len > 0 && buffer[0] == 'S' && buffer[1] == 'T' && buffer[2] == 'A') {
       // Likely START header
       String str = "";
       for(int i=0; i<min(len, 20); i++) str += (char)buffer[i];
       if(str.startsWith("START")) {
         receivingFile = true;
         startTime = millis();
         totalBytes = 0;
         Serial.println("### START_OF_FILE ###");
         Serial.print("MSG: Signal - RSSI: ");
         Serial.print(lastRSSI);
         Serial.print(" dBm, SNR: ");
         Serial.print(lastSNR);
         Serial.println(" dB");
       }
    }
    else if (len > 0 && buffer[0] == 'E' && buffer[1] == 'N' && buffer[2] == 'D') {
       finishFile();
    } 
    else {
      if (receivingFile) {
        // FAST PRINTING: Raw Hex
        Serial.print("DATA:");
        for(int i=0; i<len; i++) {
          if(buffer[i] < 16) Serial.print("0");
          Serial.print(buffer[i], HEX);
        }
        Serial.println(); 

        totalBytes += len;
        // Don't print stats too often, it slows us down
        if (millis() - lastStatTime > 1000) {
           lastStatTime = millis();
           Serial.print("MSG: Bytes: ");
           Serial.print(totalBytes);
           Serial.print(" | RSSI: ");
           Serial.print(lastRSSI);
           Serial.print(" dBm | SNR: ");
           Serial.print(lastSNR);
           Serial.println(" dB");
        }
      }
    }
  }

  // Safety Timeout (2.5 seconds)
  if (receivingFile && (millis() - lastPacketTime > 2500)) {
    Serial.println("MSG: Timeout. Finishing.");
    finishFile();
  }
}

void finishFile() {
  if (!receivingFile) return;
  receivingFile = false;
  unsigned long duration = millis() - startTime;
  Serial.println("### END_OF_FILE ###");
  Serial.print("MSG: Done! Total Bytes: "); Serial.println(totalBytes);
  Serial.print("MSG: Final RSSI: "); Serial.print(lastRSSI);
  Serial.print(" dBm | SNR: "); Serial.print(lastSNR); Serial.println(" dB");
}