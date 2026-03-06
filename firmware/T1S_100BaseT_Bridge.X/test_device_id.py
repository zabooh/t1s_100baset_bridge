#!/usr/bin/env python3
"""
LAN8651 Device ID Verification Test
Basierend auf Microchip Datenblatt
"""

import serial
import time
import re

class LAN8651DeviceIDTest:
    def __init__(self, port='COM8', baudrate=115200):
        self.port = port
        self.baudrate = baudrate
        
        # Expected values from datasheet
        self.expected_values = {
            # OA_ID Register (0x00000000) - Open Alliance Version
            "0x00000000": {
                "name": "OA_ID (Open Alliance ID)",
                "expected": 0x00000011,  # MAJVER=1, MINVER=1 (Version 1.1)
                "description": "Open Alliance 10BASE-T1x specification version",
                "fields": {
                    "MAJVER": {"bits": "7:4", "expected": 1},
                    "MINVER": {"bits": "3:0", "expected": 1}
                }
            },
            
            # OA_PHYID Register (0x00000001) - PHY Identification  
            "0x00000001": {
                "name": "OA_PHYID (PHY ID)",
                "expected": None,  # Varies by device
                "description": "Contains OUI, Model, and Revision",
                "fields": {
                    "OUI": {"bits": "31:10", "expected": None},
                    "MODEL": {"bits": "9:4", "expected": None}, 
                    "REVISION": {"bits": "3:0", "expected": None}
                }
            },
            
            # What we previously tested (0x00000004)
            "0x00000004": {
                "name": "OA_CONFIG0 (Configuration)",
                "expected": None,  # Configuration dependent
                "description": "Configuration register (not device ID)",
                "fields": {}
            }
        }
    
    def send_command(self, ser, command):
        """Send command and get response"""
        ser.reset_input_buffer()
        ser.write(f'{command}\r\n'.encode())
        time.sleep(1.5)  # Wait for async callback
        response = ser.read_all().decode('utf-8', errors='ignore')
        return response
    
    def parse_lan_read_response(self, response):
        """Parse LAN865X read response"""
        # Look for: "LAN865X Read: Addr=0x12345678 Value=0xABCDEF12"
        match = re.search(r'LAN865X Read: Addr=0x([0-9A-Fa-f]+) Value=0x([0-9A-Fa-f]+)', response)
        if match:
            addr = int(match.group(1), 16)
            value = int(match.group(2), 16)
            return addr, value
        return None, None
    
    def analyze_register_value(self, addr_hex, value, expected_info):
        """Analyze register value against expected"""
        print(f"\n📋 REGISTER ANALYSE: {expected_info['name']}")
        print(f"   Adresse: {addr_hex}")
        print(f"   Funktion: {expected_info['description']}")
        print(f"   Gelesener Wert: 0x{value:08X}")
        
        if expected_info['expected'] is not None:
            expected = expected_info['expected']
            print(f"   Erwarteter Wert: 0x{expected:08X}")
            
            if value == expected:
                print("   ✅ WERT KORREKT!")
                return True
            else:
                print("   ❌ Wert weicht ab")
                return False
        else:
            print("   ℹ️  Wert variabel (device-abhängig)")
            
        # Analyze bit fields if available
        if expected_info['fields']:
            print("\n   🔍 BIT-FELD ANALYSE:")
            for field_name, field_info in expected_info['fields'].items():
                if 'bits' in field_info:
                    bits = field_info['bits']
                    # Simple bit extraction (assumes format like "7:4")
                    if ':' in bits:
                        high, low = map(int, bits.split(':'))
                        mask = ((1 << (high - low + 1)) - 1) << low
                        field_value = (value & mask) >> low
                        print(f"      {field_name} [{bits}]: 0x{field_value:X} ({field_value})")
                        
                        if field_info['expected'] is not None:
                            exp_field = field_info['expected']
                            status = "✅" if field_value == exp_field else "❌"
                            print(f"        Expected: {exp_field} {status}")
        
        return None
    
    def run_device_id_test(self):
        """Main test function"""
        print("=== LAN8651 DEVICE ID VERIFICATION ===")
        print("Basierend auf Microchip LAN8651 Datenblatt")
        print("=" * 50)
        
        try:
            ser = serial.Serial(self.port, self.baudrate, timeout=3)
            time.sleep(1)
            print(f"✓ {self.port} verbunden")
            
            results = {}
            
            for addr_hex, reg_info in self.expected_values.items():
                print(f"\n🔸 Teste Register {addr_hex} ({reg_info['name']})...")
                
                # Send lan_read command
                command = f"lan_read {addr_hex}"
                response = self.send_command(ser, command)
                
                # Parse response
                read_addr, read_value = self.parse_lan_read_response(response)
                
                if read_value is not None:
                    results[addr_hex] = {
                        'value': read_value,
                        'valid': self.analyze_register_value(addr_hex, read_value, reg_info)
                    }
                else:
                    print(f"❌ Lesefehler für {addr_hex}")
                    print(f"   Antwort: {response.strip()}")
                    results[addr_hex] = {'value': None, 'valid': False}
            
            ser.close()
            
            # Summary
            print("\n" + "=" * 50)
            print("📊 ERGEBNISSE ZUSAMMENFASSUNG")
            print("=" * 50)
            
            valid_count = 0
            total_count = 0
            
            for addr_hex, result in results.items():
                reg_name = self.expected_values[addr_hex]['name']
                if result['value'] is not None:
                    print(f"✓ {addr_hex}: {reg_name} = 0x{result['value']:08X}")
                    if result['valid'] is True:
                        valid_count += 1
                    total_count += 1
                else:
                    print(f"✗ {addr_hex}: {reg_name} = LESEFEHLER")
            
            print(f"\n🎯 VALIDIERUNG: {valid_count}/{total_count} Register korrekt")
            
            # Specific device ID recommendation  
            if '0x00000000' in results and results['0x00000000']['value'] is not None:
                oa_id_value = results['0x00000000']['value']
                majver = (oa_id_value >> 4) & 0xF
                minver = oa_id_value & 0xF
                
                print(f"\n🆔 DEVICE IDENTIFICATION:")
                print(f"   Open Alliance Version: {majver}.{minver}")
                
                if oa_id_value == 0x00000011:
                    print("   ✅ LAN8651 Device bestätigt (Version 1.1 compliant)")
                else:
                    print(f"   ⚠️  Abweichende Version (erwartet 1.1, gefunden {majver}.{minver})")
            
            return results
            
        except Exception as e:
            print(f"❌ Test Fehler: {e}")
            return None

def main():
    test = LAN8651DeviceIDTest()
    results = test.run_device_id_test()
    
    if results:
        print("\n🎉 Device ID Test abgeschlossen!")
        
        # Check if main device ID is correct
        if ('0x00000000' in results and 
            results['0x00000000']['value'] == 0x00000011):
            print("✅ LAN8651 Device ID erfolgreich verifiziert!")
            exit(0)
        else:
            print("⚠️  Device ID Verification teilweise erfolgreich")
            exit(1)
    else:
        print("❌ Device ID Test fehlgeschlagen")
        exit(2)

if __name__ == "__main__":
    main()