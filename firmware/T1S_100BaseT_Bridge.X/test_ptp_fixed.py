#!/usr/bin/env python3
"""
LAN8651 TSU Wallclock Timer PTP Test (KORRIGIERT)
Mit korrekten direkten MAC Register-Adressen
"""

import serial
import time
import re
from datetime import datetime, timezone

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

def quick_ptp_test():
    """Quick PTP functionality test with corrected addresses"""
    print("=== LAN8651 TSU PTP TEST (KORRIGIERT) ===")
    
    # KORREKTE MAC Register Adressen (direkt, nicht MMS1)
    registers = {
        "MAC_NCR":    "0x00000000",  # Network Control
        "MAC_TSH":    "0x00000070",  # Timer Seconds High      
        "MAC_TSL":    "0x00000074",  # Timer Seconds Low
        "MAC_TN":     "0x00000075",  # Timer Nanoseconds
        "MAC_TI":     "0x00000077",  # Timer Increment
        "MAC_TA":     "0x00000076",  # Timer Adjust
    }
    
    try:
        ser = serial.Serial('COM8', 115200, timeout=3)
        time.sleep(1)
        print("✓ COM8 verbunden")
        
        print("\n🔄 Board Reset und Boot-Sequence warten...")
        
        # 1. Reset Kommando senden
        print("   Sende Reset-Kommando...")
        ser.reset_input_buffer()
        ser.write(b'reset\r\n')
        time.sleep(1)
        
        # 2. Warten auf Boot-Sequence
        print("   Warte auf Boot-Sequence...")
        boot_complete = False
        timestamp_seen = False
        timeout_counter = 0
        max_timeout = 30  # 30 Sekunden timeout
        
        while not timestamp_seen and timeout_counter < max_timeout:
            if ser.in_waiting > 0:
                data = ser.read_all().decode('utf-8', errors='ignore')
                print(f"   Boot: {data.strip()}")
                
                # Prüfe auf Boot-Complete
                if "Reset Complete" in data or "Initialization Ended - success" in data:
                    boot_complete = True
                    print("   ✅ Boot-Sequence abgeschlossen")
                
                # Prüfe auf Timestamp (Build Info Ausgabe)
                if boot_complete and ("Build Timestamp" in data or "T1S Packet Sniffer" in data):
                    timestamp_seen = True
                    print("   ✅ Timestamp-Ausgabe erkannt - System bereit")
                    break
            
            time.sleep(0.5)
            timeout_counter += 0.5
        
        if not timestamp_seen:
            if boot_complete:
                print("   ⚠️ Boot komplett, aber kein Timestamp - starte trotzdem")
            else:
                print("   ❌ Boot-Timeout - starte trotzdem Test")
        
        # 3. Zusätzliche Bereitschaftszeit
        time.sleep(2)
        print("   🎯 System bereit für Tests")
        
        print("\n🔧 1. TSU Register Status prüfen...")
        for name, addr in registers.items():
            command = f"lan_read {addr}"
            response = send_command(ser, command)
            addr_val, value = parse_lan_read_response(response)
            
            if value is not None:
                print(f"   {name:10} ({addr}): 0x{value:08X}")
            else:
                print(f"   {name:10} ({addr}): LESEFEHLER")
        
        print("\n🔧 2. TSU aktivieren (MAC_NCR)...")
        # Current MAC_NCR value
        response = send_command(ser, f"lan_read {registers['MAC_NCR']}")
        _, ncr_current = parse_lan_read_response(response)
        print(f"   Aktuell: 0x{ncr_current:08X}")
        
        # Enable TSU (verschiedene Bits probieren)
        tsu_enable_attempts = [
            ncr_current | 0x00000010,  # Bit 4
            ncr_current | 0x00000800,  # Bit 11  
            ncr_current | 0x00001000,  # Bit 12
            ncr_current | 0x00000020,  # Bit 5
        ]
        
        tsu_working = False
        for attempt in tsu_enable_attempts:
            print(f"   Probiere NCR = 0x{attempt:08X}")
            
            # Write new NCR value
            write_cmd = f"lan_write {registers['MAC_NCR']} 0x{attempt:08X}"
            write_resp = send_command(ser, write_cmd)
            
            if "- OK" in write_resp:
                time.sleep(0.5)
                
                # Initialize timer to current time
                pc_seconds, pc_nanos = get_pc_timestamp()
                print(f"   Setze Zeit: {pc_seconds} sec, {pc_nanos} ns")
                
                # Split seconds for 48-bit register
                seconds_high = (pc_seconds >> 32) & 0xFFFF
                seconds_low = pc_seconds & 0xFFFFFFFF
                
                # Write time registers
                send_command(ser, f"lan_write {registers['MAC_TSH']} 0x{seconds_high:08X}")
                send_command(ser, f"lan_write {registers['MAC_TSL']} 0x{seconds_low:08X}")
                send_command(ser, f"lan_write {registers['MAC_TN']} 0x{pc_nanos:08X}")
                
                time.sleep(1)
                
                # Test if timer is running
                test_resp = send_command(ser, f"lan_read {registers['MAC_TSL']}")
                _, test_val = parse_lan_read_response(test_resp)
                
                if test_val is not None and test_val > 0:
                    print(f"   ✅ TSU läuft! Timer Low = 0x{test_val:08X}")
                    tsu_working = True
                    break
                else:
                    timer_val = test_val if test_val is not None else 0
                    print(f"   ❌ TSU läuft nicht (Timer = 0x{timer_val:08X})")
            else:
                print(f"   ❌ NCR Write fehlgeschlagen")
        
        if not tsu_working:
            print("\n⚠️ TSU nicht aktivierbar - teste trotzdem Wallclock Reading...")
        
        print("\n🕐 3. Wallclock Reading Test (5 Messungen)...")
        print("=" * 70)
        print("Zeit  | PC Timestamp            | LAN Timestamp           | Diff")  
        print("=" * 70)
        
        for i in range(5):
            # Read PC time
            pc_seconds, pc_nanos = get_pc_timestamp()
            pc_formatted = format_timestamp(pc_seconds, pc_nanos)
            
            # Read LAN time  
            tsh_resp = send_command(ser, f"lan_read {registers['MAC_TSH']}")
            tsl_resp = send_command(ser, f"lan_read {registers['MAC_TSL']}")
            tn_resp = send_command(ser, f"lan_read {registers['MAC_TN']}")
            
            _, tsh = parse_lan_read_response(tsh_resp)
            _, tsl = parse_lan_read_response(tsl_resp)
            _, tn = parse_lan_read_response(tn_resp)
            
            if all(x is not None for x in [tsh, tsl, tn]):
                # Reconstruct LAN time
                lan_seconds = ((tsh & 0xFFFF) << 32) | (tsl & 0xFFFFFFFF)
                lan_nanos = tn & 0x3FFFFFFF
                lan_formatted = format_timestamp(lan_seconds, lan_nanos)
                
                # Calculate difference
                if lan_seconds > 0:
                    diff_sec = abs(pc_seconds - lan_seconds)
                    diff_ns = abs(pc_nanos - lan_nanos) if diff_sec == 0 else 0
                    diff_ms = (diff_sec * 1000) + (diff_ns / 1_000_000)
                    status = "✅" if diff_ms < 1000 else ("⚠️" if diff_ms < 60000 else "❌")
                    print(f"{i+1:3d}.  | {pc_formatted} | {lan_formatted} | {diff_ms:8.1f}ms {status}")
                else:
                    print(f"{i+1:3d}.  | {pc_formatted} | {'1970-01-01 00:00:00.000':23} | Timer=0 ❌")
            else:
                print(f"{i+1:3d}.  | {pc_formatted} | {'LESEFEHLER':23} | {'':>10}")
            
            time.sleep(2)
        
        ser.close()
        
        print("\n" + "=" * 70)
        print("💡 BEWERTUNG")
        print("=" * 70)
        print("✅ Timer läuft = TSU Wallclock funktional")
        print("⚠️ Timer=0 = TSU nicht aktiviert oder nicht verfügbar")
        print("❌ Lesefehler = Register-Zugriffsproblem")
        
        return True
        
    except Exception as e:
        print(f"❌ Test Fehler: {e}")
        return False

def get_pc_timestamp():
    """Get current PC timestamp in seconds and nanoseconds"""
    now = datetime.now(timezone.utc)
    epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
    total_seconds = int((now - epoch).total_seconds())
    nanoseconds = int((now.microsecond * 1000) + ((now - epoch).total_seconds() % 1) * 1e9) % 1000000000
    return total_seconds, nanoseconds

def format_timestamp(seconds, nanoseconds):
    """Format timestamp for display"""
    dt = datetime.fromtimestamp(seconds, tz=timezone.utc)
    return f"{dt.strftime('%Y-%m-%d %H:%M:%S')}.{nanoseconds//1000000:03d}"

if __name__ == "__main__":
    quick_ptp_test()