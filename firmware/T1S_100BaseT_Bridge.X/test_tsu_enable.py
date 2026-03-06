#!/usr/bin/env python3
"""
LAN8651 TSU Timer Enable Test
Teste TX/RX Enable für TSU Timer Start
"""

import serial
import time
import re

def send_command(ser, command):
    """Send command and get response"""
    ser.reset_input_buffer()  
    ser.write(f'{command}\r\n'.encode())
    time.sleep(1.5)
    response = ser.read_all().decode('utf-8', errors='ignore')
    return response

def parse_lan_read_response(response):
    """Parse LAN865X read response"""
    match = re.search(r'LAN865X Read: Addr=0x([0-9A-Fa-f]+) Value=0x([0-9A-Fa-f]+)', response)
    if match:
        addr = int(match.group(1), 16)
        value = int(match.group(2), 16)
        return addr, value
    return None, None

def tsu_enable_test():
    """Test TSU Timer activation via MAC_NCR TX/RX Enable"""
    print("=== LAN8651 TSU TIMER ENABLE TEST ===")
    
    MMS1_BASE = 0x01000000
    MAC_NCR = MMS1_BASE + 0x00  # Network Control
    MAC_TSL = MMS1_BASE + 0x74  # Timer Seconds Low
    MAC_TI = MMS1_BASE + 0x77   # Timer Increment
    
    try:
        ser = serial.Serial('COM8', 115200, timeout=3)
        time.sleep(1)
        print("✓ COM8 verbunden")
        
        # Reset Board
        print("\n🔄 Board Reset...")
        ser.reset_input_buffer()
        ser.write(b'reset\r\n')
        time.sleep(3)
        
        # Wait for boot  
        while ser.in_waiting > 0:
            data = ser.read_all().decode('utf-8', errors='ignore')
            if "Build Timestamp" in data:
                break
        time.sleep(2)
        
        print("\n🔧 1. Aktueller MAC_NCR Status...")
        response = send_command(ser, f"lan_read 0x{MAC_NCR:08X}")
        _, ncr_current = parse_lan_read_response(response)
        print(f"   MAC_NCR: 0x{ncr_current:08X}")
        print(f"   RXEN (Bit 2): {bool(ncr_current & 0x04)}")
        print(f"   TXEN (Bit 3): {bool(ncr_current & 0x08)}")
        
        print(f"\n🔧 2. Timer Increment setzen...")
        write_cmd = f"lan_write 0x{MAC_TI:08X} 0x00000028"
        send_command(ser, write_cmd)
        print(f"   ✅ Timer Increment: 0x00000028 (40 ns)")
        
        print(f"\n🔧 3. TX/RX Enable für TSU...")
        
        # Test verschiedene NCR Kombinationen
        test_values = [
            (ncr_current | 0x08, "TXEN aktiviert"),           # TX enable hinzufügen
            (ncr_current | 0x0C, "TX+RX beide aktiviert"),    # TX + RX enable  
            (ncr_current | 0x10, "Bit 4 zusätzlich"),        # Noch ein Bit testen
            (ncr_current | 0x18, "TX+Bit4 aktiviert"),       # TX + Bit 4
        ]
        
        for ncr_value, description in test_values:
            print(f"\n   Teste: {description} (0x{ncr_value:08X})")
            
            # NCR setzen
            write_cmd = f"lan_write 0x{MAC_NCR:08X} 0x{ncr_value:08X}"
            write_resp = send_command(ser, write_cmd)
            
            if "- OK" in write_resp:
                time.sleep(0.5)  # Kurze Pause
                
                # TSL Register prüfen (Timer läuft?)
                test_resp = send_command(ser, f"lan_read 0x{MAC_TSL:08X}")
                _, tsl_val = parse_lan_read_response(test_resp)
                
                if tsl_val is not None and tsl_val > 0:
                    print(f"   🎉 SUCCESS! TSU Timer läuft: 0x{tsl_val:08X}")
                    return True
                else:
                    val_display = tsl_val if tsl_val is not None else 0
                    print(f"   ❌ Timer läuft nicht: 0x{val_display:08X}")
            else:
                print(f"   ❌ NCR Write fehlgeschlagen")
        
        print(f"\n⚠️ Alle TX/RX Kombinationen getestet - kein Timer Start")
        
        ser.close()
        return False
        
    except Exception as e:
        print(f"❌ Test Fehler: {e}")
        return False

if __name__ == "__main__":
    success = tsu_enable_test()
    
    if success:
        print("\n🎉 TSU Timer erfolgreich aktiviert!")
    else:
        print("\n💡 NÄCHSTE SCHRITTE:")
        print("- Andere MAC_NCR Bits testen")
        print("- MAC_NCFGR (Network Config) Register prüfen") 
        print("- TSU eventuell nicht in dieser LAN8651 Revision verfügbar")