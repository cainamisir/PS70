import serial
import time
import binascii
import wave

# --- CONFIGURATION ---
OUTPUT_FILE = "hq_audio.wav"
BAUD_RATE = 921600   # <--- UPDATED TO MATCH ESP32
SAMPLE_RATE = 16000  

def save_audio(filename, data_bytes):
    print(f"Saving {len(data_bytes)} bytes...")
    with wave.open(filename, 'wb') as wav_file:
        wav_file.setnchannels(1)      
        wav_file.setsampwidth(1)      
        wav_file.setframerate(SAMPLE_RATE)
        wav_file.writeframes(data_bytes)
    print(f"\n[SUCCESS] Saved to {filename}")

def main():
    print("--- LoRa HQ Audio Receiver (High Speed) ---")
    # REPLACE WITH YOUR PORT
    port = "/dev/cu.usbserial-0001" 
    
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=1)
        print(f"Connected to {port} at {BAUD_RATE} baud.")
    except Exception as e:
        print(f"Error: {e}")
        return

    audio_data = bytearray()
    recording = False
    start_time = 0

    while True:
        try:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            
            if line:
                if "### START_OF_FILE ###" in line:
                    print("\n[STARTED] Receiving...")
                    audio_data = bytearray()
                    recording = True
                    start_time = time.time()
                
                elif "### END_OF_FILE ###" in line:
                    duration = time.time() - start_time
                    print(f"\n[DONE] Time: {duration:.2f}s")
                    save_audio(OUTPUT_FILE, audio_data)
                    recording = False

                elif line.startswith("MSG:"):
                    print(line) 

                elif line.startswith("DATA:") and recording:
                    raw_hex = line.split(":")[1]
                    try:
                        chunk = binascii.unhexlify(raw_hex)
                        audio_data.extend(chunk)
                    except:
                        pass 

        except KeyboardInterrupt:
            break

if __name__ == "__main__":
    main()