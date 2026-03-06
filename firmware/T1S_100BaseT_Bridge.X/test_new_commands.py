#!/usr/bin/env python3
"""
Test der neuen LAN8651 Commands
Prüft ob die help-Command-Fixes funktionieren
"""

import serial
import time
import sys

def test_commands():
    print("=== TEST DER NEUEN COMMANDS ===")
    
    try:
        # COM8 öffnen
        ser = serial.Serial('COM8', 115200, timeout=2)
        time.sleep(1)
        print("✓ COM8 geöffnet")
        
        # Clear buffer
        ser.reset_input_buffer()
        
        # Test 1: help Command
        print("\n--- Test 1: Basis help ---")
        ser.write(b'help\r\n')
        time.sleep(0.5)
        response = ser.read_all().decode('utf-8', errors='ignore')
        print(f"Antwort: {response.strip()}")
        
        if "Test: Test Commands" in response:
            print("✅ Test-Gruppe ist registriert")
        else:
            print("❌ Test-Gruppe NICHT gefunden")
        
        # Test 2: Test help (das war das Problem)
        print("\n--- Test 2: Test help ---")
        ser.reset_input_buffer()
        ser.write(b'Test help\r\n')
        time.sleep(1)
        response = ser.read_all().decode('utf-8', errors='ignore')
        print(f"Antwort: {response.strip()}")
        
        if "Test group commands:" in response:
            print("🎉 SUCCESS! Test help funktioniert jetzt!")
            return True
        elif "unknown command" in response:
            print("❌ Test help funktioniert noch nicht")
            return False
        else:
            print("⚠️ Unerwartete Antwort")
            return False
            
    except serial.SerialException as e:
        print(f"❌ COM8 Fehler: {e}")
        return False
    except Exception as e:
        print(f"❌ Fehler: {e}")
        return False
    finally:
        if 'ser' in locals():
            ser.close()

if __name__ == "__main__":
    result = test_commands()
    if result:
        print("\n✅ NEUE FIRMWARE IST AKTIV!")
        sys.exit(0)
    else:
        print("\n❌ ALTE FIRMWARE - Programmierung nötig")
        sys.exit(1)