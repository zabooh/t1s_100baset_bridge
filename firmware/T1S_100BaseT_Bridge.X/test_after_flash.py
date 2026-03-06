#!/usr/bin/env python3
"""
Nach dem Firmware-Update: Teste LAN8651 Kommandos
"""

import serial
import time

def quick_test():
    """Schneller Test nach Firmware-Update"""
    
    try:
        ser = serial.Serial("COM8", 115200, timeout=5)
        print("COM8 geöffnet - Teste nach Firmware-Update...")
        
        # CLI aktivieren
        ser.write(b'\r\n')
        time.sleep(0.5)
        if ser.in_waiting > 0:
            ser.read_all()
        
        tests = [
            ("help", "Basis Help"),
            ("Test help", "Test-Gruppe Help"),
            ("Test lan_read 0x0", "LAN8651 Register Read"),
        ]
        
        for cmd, desc in tests:
            print(f"\n--- {desc} ---")
            print(f"Kommando: {cmd}")
            
            ser.write((cmd + '\r\n').encode())
            time.sleep(2)
            
            response = ""
            for _ in range(20):
                if ser.in_waiting > 0:
                    response += ser.read_all().decode('utf-8', errors='ignore')
                time.sleep(0.1)
            
            if response.strip():
                print("✓ Antwort erhalten:")
                lines = response.strip().split('\n')
                for line in lines[:5]:  # Erste 5 Zeilen
                    print(f"  {line.strip()}")
                if len(lines) > 5:
                    print(f"  ... + {len(lines)-5} weitere Zeilen")
                    
                # Check for success indicators
                if "lan_read" in response or "lan_write" in response:
                    print("🎉 SUCCESS: LAN8651-Kommandos gefunden!")
                elif "unknown command" in response:
                    print("⚠️ Kommando noch nicht verfügbar")
                    
            else:
                print("✗ Keine Antwort")
            
            print("-" * 40)
        
        ser.close()
        print("\n✓ Test abgeschlossen")
        
    except Exception as e:
        print(f"Fehler: {e}")

if __name__ == "__main__":
    print("=== FIRMWARE UPDATE TEST ===")
    quick_test()
    input("\nDrücken Sie Enter zum Beenden...")