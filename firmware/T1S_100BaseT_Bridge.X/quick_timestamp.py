#!/usr/bin/env python3
"""
Schneller Timestamp Check nach Reset
"""

import serial
import time

def check_current_firmware():
    print("=== FIRMWARE TIMESTAMP CHECK ===")
    
    try:
        ser = serial.Serial('COM8', 115200, timeout=3)
        time.sleep(1)
        print("✓ COM8 verbunden")
        
        # Send reset and capture boot output
        ser.reset_input_buffer()
        ser.write(b'reset\r\n')
        time.sleep(2)
        
        response = ser.read_all().decode('utf-8', errors='ignore')
        ser.close()
        
        print("Reset-Ausgabe:")
        print("=" * 50)
        print(response)
        print("=" * 50)
        
        # Look for timestamp
        if "Build Timestamp:" in response:
            lines = response.split('\n')
            for line in lines:
                if "Build Timestamp:" in line:
                    timestamp = line.strip()
                    print(f"\n📅 GEFUNDEN: {timestamp}")
                    
                    # Check if it's a recent build (today at 17:xx)
                    if "Mar  6 2026 17:" in timestamp:
                        print("✅ NEUE FIRMWARE LÄUFT!")
                        return True
                    else:
                        print("❌ Alte Firmware - Programmierung war nicht erfolgreich")
                        return False
        
        print("❌ Kein Build Timestamp gefunden")
        return False
        
    except Exception as e:
        print(f"❌ Fehler: {e}")
        return False

if __name__ == "__main__":
    check_current_firmware()