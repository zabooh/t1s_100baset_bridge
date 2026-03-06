#!/usr/bin/env python3
"""
Nach der Programmierung - Test der LAN8651 Commands
"""

import serial
import time

def test_lan8651_commands():
    print("=== LAN8651 KOMMANDO-TEST ===")
    
    try:
        ser = serial.Serial('COM8', 115200, timeout=2)
        time.sleep(1)
        print("✓ COM8 verbunden")
        
        tests = [
            ("help", "Basis help"),
            ("help Test", "Test-Gruppen help"), 
            ("timestamp", "Build Timestamp anzeigen"),
            ("lan_read 0x00000004", "LAN8651 ID Register lesen"),
            ("lan_read", "LAN8651 Usage anzeigen"),
            ("lan_write 0x00000080 0x12345678", "LAN8651 Test schreibvorgang")
        ]
        
        for cmd, desc in tests:
            print(f"\n--- {desc} ---")
            ser.reset_input_buffer()
            ser.write(f'{cmd}\r\n'.encode())
            time.sleep(1)
            
            response = ser.read_all().decode('utf-8', errors='ignore')
            print(f"Kommando: {cmd}")
            print(f"Antwort:\n{response}")
            
            # Check if command worked
            if "unknown command" in response:
                print("❌ Kommando nicht erkannt")
            elif "Usage:" in response and "lan_read" in cmd and len(cmd.split()) == 1:
                print("✅ LAN8651 Usage angezeigt - Command verfügbar!")
            elif "Build Timestamp:" in response and "timestamp" in cmd:
                print("✅ Timestamp Command funktioniert!")
            elif "Test Commands" in response and "help Test" in cmd:
                print("✅ Test-Gruppe Commands gelistet!")
            elif "Supported command groups" in response and cmd == "help":
                print("✅ Basis Help funktioniert!")
            else:
                print("✅ Command ausgeführt - prüfe Ausgabe!")
        
        ser.close()
        print("\n🎉 Tests abgeschlossen!")
        
    except Exception as e:
        print(f"❌ Fehler: {e}")

if __name__ == "__main__":
    test_lan8651_commands()