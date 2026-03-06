import serial
import time
from datetime import datetime
import re

def read_boot_timestamp():
    """Lese Build Timestamp beim Target-Start"""
    
    print("=== BUILD TIMESTAMP PRÜFUNG ===")
    print("Prüfe ob neue Firmware aktiv ist...")
    print()
    
    try:
        ser = serial.Serial('COM8', 115200, timeout=10)
        print("✓ COM8 geöffnet")
        
        # Methode 1: Warte auf natürlichen Boot-Output
        print("📡 Warte auf Boot-Nachrichten...")
        
        boot_output = ""
        start_time = time.time()
        timestamp_found = False
        
        # Sammle 30 Sekunden Boot-Output
        while time.time() - start_time < 30:
            if ser.in_waiting > 0:
                new_data = ser.read_all().decode('utf-8', errors='ignore')
                boot_output += new_data
                
                # Zeige Preview der Daten
                if new_data.strip():
                    preview = new_data.replace('\r', '').replace('\n', ' | ')[:60]
                    print(f"📥 {preview}...")
                
                # Prüfe auf Build Timestamp
                if "Build Timestamp:" in new_data:
                    timestamp_found = True
                    print("🎯 Build Timestamp gefunden!")
                    break
            
            time.sleep(0.1)
        
        # Wenn kein natürlicher Boot, versuche Reset
        if not timestamp_found:
            print("\n🔄 Kein Boot-Output - versuche Reset...")
            
            # Sende Reset-Kommando
            ser.write(b'\r\n')
            time.sleep(0.5)
            if ser.in_waiting > 0:
                ser.read_all()  # Leere Puffer
            
            ser.write(b'reset\r\n')
            time.sleep(2)
            
            # Sammle Reset-Output
            reset_start = time.time()
            while time.time() - reset_start < 15:
                if ser.in_waiting > 0:
                    new_data = ser.read_all().decode('utf-8', errors='ignore')
                    boot_output += new_data
                    
                    if "Build Timestamp:" in new_data:
                        timestamp_found = True
                        print("🎯 Build Timestamp nach Reset gefunden!")
                        break
                
                time.sleep(0.1)
        
        ser.close()
        
        # Analysiere Build Timestamp
        print("\n" + "="*60)
        print("GESAMTER BOOT-OUTPUT:")
        print("="*60)
        print(boot_output)
        print("="*60)
        
        # Extrahiere Timestamp
        timestamp_match = re.search(r'Build Timestamp:\s*(\w+\s+\d+\s+\d{4}\s+\d{2}:\d{2}:\d{2})', boot_output)
        
        if timestamp_match:
            build_timestamp = timestamp_match.group(1)
            print(f"\n🕒 GEFUNDENER BUILD TIMESTAMP: {build_timestamp}")
            
            # Parse Timestamp
            try:
                build_time = datetime.strptime(build_timestamp, "%b %d %Y %H:%M:%S")
                current_time = datetime.now()
                
                print(f"📅 Build-Zeit: {build_time}")
                print(f"🕐 Aktuelle Zeit: {current_time}")
                
                # Zeitdifferenz berechnen
                time_diff = current_time - build_time
                minutes_diff = abs(time_diff.total_seconds()) / 60
                
                print(f"⏱️  Zeitdifferenz: {minutes_diff:.1f} Minuten")
                
                if minutes_diff < 60:  # Weniger als 1 Stunde alt
                    print("\n🎉 SUCCESS: NEUE FIRMWARE BESTÄTIGT!")
                    print(f"   Build ist nur {minutes_diff:.1f} Minuten alt")
                    return True
                else:
                    print(f"\n⚠️  WARNING: Firmware ist {minutes_diff/60:.1f} Stunden alt")
                    print("   Möglicherweise nicht die neueste Version")
                    return False
                    
            except ValueError as e:
                print(f"\n❌ Fehler beim Parsen des Timestamps: {e}")
                print(f"   Roher Timestamp: '{build_timestamp}'")
                return False
        else:
            print("\n❌ KEIN BUILD TIMESTAMP GEFUNDEN!")
            print("   Entweder:")
            print("   1. Target bootet nicht korrekt")
            print("   2. Firmware hat kein Timestamp-Output")
            print("   3. COM-Port-Problem")
            return False
            
    except Exception as e:
        print(f"❌ Fehler: {e}")
        return False

def get_current_date_for_comparison():
    """Zeige aktuelles Datum für manuellen Vergleich"""
    now = datetime.now()
    print(f"\n📅 REFERENZ - Heutiges Datum: {now.strftime('%b %d %Y %H:%M:%S')}")
    print(f"   (Format wie Build Timestamp)")

if __name__ == "__main__":
    get_current_date_for_comparison()
    
    print("\n" + "="*60)
    success = read_boot_timestamp()
    print("="*60)
    
    if success:
        print("\n✅ FIRMWARE IST AKTUELL - Kann mit Tests fortfahren!")
        
        # Bonus: Teste direkt die Command-Funktionalität
        print("\n🧪 BONUS-TEST: Command-Funktionalität...")
        
        try:
            ser = serial.Serial('COM8', 115200, timeout=5)
            
            ser.write(b'\r\n')
            time.sleep(0.5)
            if ser.in_waiting > 0:
                ser.read_all()
            
            ser.write(b'Test help\r\n')
            time.sleep(2)
            
            cmd_response = ""
            while ser.in_waiting > 0:
                cmd_response += ser.read_all().decode('utf-8', errors='ignore')
                time.sleep(0.1)
                
            ser.close()
            
            if "lan_read" in cmd_response:
                print("🎯 PERFEKT: LAN8651-Kommandos sind verfügbar!")
            elif "ipdump" in cmd_response:
                print("⚠️ Test-Kommandos da, aber LAN8651 fehlt noch")
            else:
                print("❌ Test-Kommandos funktionieren noch nicht")
                
        except:
            print("⚠️ Konnte Command-Test nicht durchführen")
    else:
        print("\n❌ FIRMWARE NICHT AKTUELL - Bitte neu builden/flashen!")
    
    input("\nDrücken Sie Enter zum Beenden...")