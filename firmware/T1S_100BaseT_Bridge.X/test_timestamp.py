#!/usr/bin/env python3
"""
Test des neuen timestamp Kommandos
"""

import serial
import time

def test_timestamp_command():
    print("=== TIMESTAMP KOMMANDO TEST ===") 
    
    try:
        ser = serial.Serial('COM8', 115200, timeout=3)
        time.sleep(1)
        print("✓ COM8 verbunden")
        
        # Test timestamp command
        print("\n--- Test: timestamp Kommando ---")
        ser.reset_input_buffer()
        ser.write(b'Test timestamp\r\n')
        time.sleep(1)
        
        response = ser.read_all().decode('utf-8', errors='ignore')
        print(f"Kommando: Test timestamp")
        print(f"Antwort:\n{response}")
        
        # Check for build timestamp
        if "Build Timestamp:" in response:
            # Extract timestamp
            lines = response.split('\n')
            for line in lines:
                if "Build Timestamp:" in line:
                    timestamp = line.strip()
                    print(f"\n📅 TIMESTAMP: {timestamp}")
                    
                    # Check if it's today's build
                    from datetime import datetime
                    now = datetime.now()
                    today_str = now.strftime("%b %d %Y")  # Mar 06 2026
                    current_hour = now.hour
                    
                    if today_str.replace(" 0", " ") in timestamp:
                        if f"{current_hour}:" in timestamp or f"{current_hour-1}:" in timestamp:
                            print("✅ NEUE FIRMWARE BESTÄTIGT!")
                            return True
                        else:
                            print("⚠️ Firmware von heute, aber älter")
                            return True
                    else:
                        print("❌ Alte Firmware")
                        return False
        elif "unknown command" in response:
            print("❌ timestamp Kommando noch nicht verfügbar - alte Firmware")
            return False
        else:
            print("⚠️ Unerwartete Antwort")
            return False
        
    except Exception as e:
        print(f"❌ Fehler: {e}")
        return False
    finally:
        if 'ser' in locals():
            ser.close()

if __name__ == "__main__":
    result = test_timestamp_command()
    print(f"\nErgebnis: {'✅ Neue Firmware' if result else '❌ Alte Firmware'}")