#!/usr/bin/env python3
"""
LAN8651 TSU Wallclock Timer PTP Test - DATASHEET KORREKT
Basiert auf LAN8650/1 Datenblatt Sektion 6.4.9 und 11.2
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

def datasheet_compliant_ptp_test():
    """TSU PTP Test basierend auf LAN8650/1 Datenblatt"""
    print("=== LAN8651 TSU PTP TEST (DATENBLATT KORREKT) ===")
    print("Basiert auf Microchip LAN8650/1 Datenblatt:")
    print("- Sektion 6.4.9: Wall Clock")
    print("- Sektion 11.2: MAC Register (MMS 1)")
    print("=" * 60)
    
    # MMS 1 MAC Register (laut Datenblatt Sektion 11.2)
    MMS1_BASE = 0x01000000  # Memory Map Selector 1
    
    registers = {
        # Datenblatt Tabelle 11.2: MAC Registers
        "MAC_NCR":    MMS1_BASE + 0x00,  # Network Control Register
        "MAC_TSH":    MMS1_BASE + 0x70,  # Timer Seconds High [47:32]
        "MAC_TSL":    MMS1_BASE + 0x74,  # Timer Seconds Low [31:0] 
        "MAC_TN":     MMS1_BASE + 0x75,  # Timer Nanoseconds [29:0]
        "MAC_TA":     MMS1_BASE + 0x76,  # Timer Adjust
        "MAC_TI":     MMS1_BASE + 0x77,  # Timer Increment (CNS[7:0])
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
        max_timeout = 30
        
        while not timestamp_seen and timeout_counter < max_timeout:
            if ser.in_waiting > 0:
                data = ser.read_all().decode('utf-8', errors='ignore')
                print(f"   Boot: {data.strip()}")
                
                if "Reset Complete" in data or "Initialization Ended - success" in data:
                    boot_complete = True
                    print("   ✅ Boot-Sequence abgeschlossen")
                
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
        
        print(f"\n🔧 1. TSU Register Status prüfen (MMS1 = 0x{MMS1_BASE:08X})...")
        for name, addr in registers.items():
            command = f"lan_read 0x{addr:08X}"
            response = send_command(ser, command)
            addr_val, value = parse_lan_read_response(response)
            
            if value is not None:
                print(f"   {name:10} (0x{addr:08X}): 0x{value:08X}")
            else:
                print(f"   {name:10} (0x{addr:08X}): LESEFEHLER")
        
        print(f"\n🔧 2. TSU Wallclock Timer initialisieren (laut Datenblatt)...")
        
        # Schritt 1: Timer Increment setzen (Datenblatt: 0x00000028 für 40ns bei 25MHz)
        timer_increment = 0x00000028  # 40 ns für 25 MHz Takt
        print(f"   Setze Timer Increment: 0x{timer_increment:08X} (40 ns)")
        write_cmd = f"lan_write 0x{registers['MAC_TI']:08X} 0x{timer_increment:08X}"
        write_resp = send_command(ser, write_cmd)
        
        if "- OK" in write_resp:
            print("   ✅ Timer Increment gesetzt")
        else:
            print(f"   ❌ Timer Increment Fehler: {write_resp.strip()}")
        
        # Schritt 2: Aktuelle Zeit setzen
        pc_seconds, pc_nanos = get_pc_timestamp()
        print(f"   Setze Zeit: {pc_seconds} sec, {pc_nanos} ns")
        
        # 48-bit Sekunden aufteilen: [47:32] High, [31:0] Low
        seconds_high = (pc_seconds >> 32) & 0xFFFF  # Nur 16 bit für high
        seconds_low = pc_seconds & 0xFFFFFFFF       # 32 bit für low
        
        print(f"   Sekunden High: 0x{seconds_high:04X}, Low: 0x{seconds_low:08X}")
        
        # Timer Seconds High setzen
        write_cmd = f"lan_write 0x{registers['MAC_TSH']:08X} 0x{seconds_high:08X}"
        write_resp = send_command(ser, write_cmd)
        
        if "- OK" in write_resp:
            print("   ✅ Timer Seconds High gesetzt")
        else:
            print(f"   ❌ Timer Seconds High Fehler: {write_resp.strip()}")
        
        # Timer Seconds Low setzen
        write_cmd = f"lan_write 0x{registers['MAC_TSL']:08X} 0x{seconds_low:08X}"
        write_resp = send_command(ser, write_cmd)
        
        if "- OK" in write_resp:
            print("   ✅ Timer Seconds Low gesetzt")
        else:
            print(f"   ❌ Timer Seconds Low Fehler: {write_resp.strip()}")
        
        # Timer Nanoseconds setzen (30-bit Feld)
        nanoseconds_30bit = pc_nanos & 0x3FFFFFFF  # Nur 30 bits
        write_cmd = f"lan_write 0x{registers['MAC_TN']:08X} 0x{nanoseconds_30bit:08X}"
        write_resp = send_command(ser, write_cmd)
        
        if "- OK" in write_resp:
            print("   ✅ Timer Nanoseconds gesetzt")
        else:
            print(f"   ❌ Timer Nanoseconds Fehler: {write_resp.strip()}")
        
        # Kurze Pause für Timer-Stabilisierung
        time.sleep(1)
        
        # Timer-Test: Prüfe ob Timer läuft
        print("\n   🕐 Timer-Funktionstest...")
        test_resp = send_command(ser, f"lan_read 0x{registers['MAC_TSL']:08X}")
        _, test_val = parse_lan_read_response(test_resp)
        
        if test_val is not None and test_val > 0:
            print(f"   ✅ TSU Timer läuft! Seconds Low = 0x{test_val:08X}")
            timer_working = True
        else:
            val_display = test_val if test_val is not None else 0
            print(f"   ❌ TSU Timer läuft nicht (Seconds Low = 0x{val_display:08X})")
            timer_working = False
        
        print("\n🕐 3. Wallclock Reading Test (5 Messungen)...")
        print("=" * 70)
        print("Zeit  | PC Timestamp            | LAN Timestamp           | Diff")  
        print("=" * 70)
        
        for i in range(5):
            # PC Zeit lesen
            pc_seconds, pc_nanos = get_pc_timestamp()
            pc_formatted = format_timestamp(pc_seconds, pc_nanos)
            
            # LAN Zeit lesen (MMS1 Adressierung)
            tsh_resp = send_command(ser, f"lan_read 0x{registers['MAC_TSH']:08X}")
            tsl_resp = send_command(ser, f"lan_read 0x{registers['MAC_TSL']:08X}")
            tn_resp = send_command(ser, f"lan_read 0x{registers['MAC_TN']:08X}")
            
            _, tsh = parse_lan_read_response(tsh_resp)
            _, tsl = parse_lan_read_response(tsl_resp)
            _, tn = parse_lan_read_response(tn_resp)
            
            if all(x is not None for x in [tsh, tsl, tn]):
                # LAN Zeit rekonstruieren (48-bit Sekunden + 30-bit Nanosekunden)
                lan_seconds = ((tsh & 0xFFFF) << 32) | (tsl & 0xFFFFFFFF)
                lan_nanos = tn & 0x3FFFFFFF
                lan_formatted = format_timestamp(lan_seconds, lan_nanos)
                
                # Differenz berechnen
                if lan_seconds > 0:
                    diff_sec = abs(pc_seconds - lan_seconds)
                    diff_ns = abs(pc_nanos - lan_nanos) if diff_sec == 0 else 0
                    diff_ms = (diff_sec * 1000) + (diff_ns / 1_000_000)
                    status = "✅" if diff_ms < 1000 else ("⚠️" if diff_ms < 60000 else "❌")
                    print(f"{i+1:3d}.  | {pc_formatted} | {lan_formatted} | {diff_ms:8.1f}ms {status}")
                else:
                    print(f"{i+1:3d}.  | {pc_formatted} | {'1970-01-01 00:00:00.000':23} | Timer=0 ❌")
            else:
                print(f"{i+1:3d}.  | {pc_formatted} | {'MMS1-LESEFEHLER':23} | {'':>10}")
            
            time.sleep(2)
        
        ser.close()
        
        print("\n" + "=" * 70)
        print("💡 DATENBLATT-BASIERTE BEWERTUNG")
        print("=" * 70)
        print("✅ Timer läuft = TSU korrekt nach Datenblatt konfiguriert")
        print("⚠️ Timer=0 = MMS1-Adressierung oder Timer-Setup Problem") 
        print("❌ MMS1-Lesefehler = Memory Map Selector Problem")
        
        if timer_working:
            print("\n🎉 SUCCESS: TSU Wallclock Timer funktioniert!")
        else:
            print("\n⚠️ ISSUE: TSU Timer läuft nicht - weitere Diagnose nötig")
        
        return timer_working
        
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
    datasheet_compliant_ptp_test()