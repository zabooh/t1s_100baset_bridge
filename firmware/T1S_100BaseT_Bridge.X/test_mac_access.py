#!/usr/bin/env python3
"""
LAN8651 MAC Register Access Test
Testet korrekt Zugriff auf MMS 1 (MAC) Register
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

def test_mac_register_access():
    """Test MAC register access in different addressing modes"""
    print("=== LAN8651 MAC REGISTER ACCESS TEST ===")
    
    # Test different possible MAC register addressing schemes
    test_addresses = {
        # Different possible ways to address MMS 1 registers
        "Direct MAC_NCR": "0x00000000",           # Direct register
        "MMS1 + MAC_NCR": "0x01000000",          # MMS 1 + offset  
        "Alt MMS1": "0x10000000",                 # Alternative MMS 1
        "Shifted MMS1": "0x00010000",             # Shifted MMS 1
        "Page MMS1": "0x40000000",                # Page-based MMS 1
        
        # Test TSU registers with different bases
        "TSU Direct": "0x00000070",               # Direct TSU
        "TSU MMS1": "0x01000070",                 # MMS 1 + TSU offset
        "TSU Alt": "0x10000070",                  # Alt MMS 1 + TSU
    }
    
    try:
        ser = serial.Serial('COM8', 115200, timeout=3)
        time.sleep(1)
        print("✓ COM8 verbunden")
        
        print("\n🔍 Teste verschiedene MAC Register Adressierungen...")
        
        for description, address in test_addresses.items():
            print(f"\n--- {description} ({address}) ---")
            
            try:
                command = f"lan_read {address}"
                response = send_command(ser, command)
                addr, value = parse_lan_read_response(response)
                
                if value is not None:
                    print(f"✅ Erfolgreich: 0x{value:08X}")
                    
                    # Special analysis for MAC_NCR-like registers
                    if "NCR" in description and value != 0:
                        print(f"   MAC_NCR Bits: EN={bool(value & 0x04)}, "
                              f"TXEN={bool(value & 0x08)}, RXEN={bool(value & 0x10)}")
                        
                    # Check for TSU-related bits  
                    if value & 0x7FF:  # If lower bits set
                        print(f"   Niedrige Bits gesetzt - möglicherweise aktives Control Register")
                        
                else:
                    print("❌ Lesefehler")
                    print(f"   Antwort: {response.strip()}")
                    
            except Exception as e:
                print(f"❌ Exception: {e}")
        
        ser.close()
        
        print("\n" + "=" * 60)
        print("💡 ANALYSE UND EMPFEHLUNGEN")
        print("=" * 60)
        print("- Erfolgreich gelesene Register = korrekte Adressierung")
        print("- Werte != 0 deuten auf aktive Register hin")  
        print("- MAC_NCR sollte Control-Bits für TSU enthalten")
        print("- TSU Timer Register sollten bei korrekter Adresse lesbar sein")
        
        return True
        
    except Exception as e:
        print(f"❌ Test Fehler: {e}")
        return False

if __name__ == "__main__":
    test_mac_register_access()