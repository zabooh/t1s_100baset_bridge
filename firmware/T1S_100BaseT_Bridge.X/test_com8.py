#!/usr/bin/env python3
"""
COM8 Test Script für LAN8651 Target
Ersetzt das PowerShell Script für stabilere serielle Kommunikation
"""

import serial
import time
import sys

def test_com8(command="help", port="COM8", baudrate=115200, timeout=10):
    """
    Teste COM8 Verbindung mit dem LAN8651 Target
    """
    print("=" * 60)
    print("         LAN8651 COM8 Test Script (Python)")
    print("=" * 60)
    print(f"Port: {port}")
    print(f"Baudrate: {baudrate}")
    print(f"Kommando: '{command}'")
    print()
    
    try:
        # Prüfe verfügbare Ports
        import serial.tools.list_ports
        available_ports = [port.device for port in serial.tools.list_ports.comports()]
        print(f"Verfügbare Ports: {', '.join(available_ports)}")
        
        if port not in available_ports:
            print(f"FEHLER: {port} nicht verfügbar!")
            return False
            
        print(f"OK: {port} ist verfügbar")
        print()
        
        # Öffne seriellen Port
        print(f"Öffne {port}...")
        ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=timeout,
            xonxoff=False,
            rtscts=False,
            dsrdtr=False
        )
        
        if not ser.is_open:
            print(f"FEHLER: {port} konnte nicht geöffnet werden!")
            return False
            
        print(f"OK: {port} erfolgreich geöffnet!")
        
        # Port-Stabilisierung
        print("Warte 500ms für Port-Stabilisierung...")
        time.sleep(0.5)
        
        # CLI aktivieren (mehrere ENTER)
        print("Sende ENTER zur CLI-Aktivierung...")
        ser.write(b'\r')
        time.sleep(0.2)
        ser.write(b'\n')
        time.sleep(0.2)
        ser.write(b'\r\n')
        time.sleep(0.5)
        
        # Leere Eingangspuffer
        if ser.in_waiting > 0:
            old_data = ser.read_all().decode('utf-8', errors='ignore')
            print(f"Puffer geleert: {len(old_data)} Bytes")
            if len(old_data) > 0:
                preview = old_data.replace('\r', '<CR>').replace('\n', '<LF>')
                print(f"Inhalt-Preview: '{preview[:100]}{'...' if len(preview) > 100 else ''}'")
        
        # Hauptkommando senden
        print(f"Sende Kommando: '{command}'...")
        command_bytes = (command + '\r\n').encode('utf-8')
        ser.write(command_bytes)
        
        # Auf Antwort warten
        print(f"Warte auf Antwort (max. {timeout}s)...")
        
        response = ""
        start_time = time.time()
        last_data_time = time.time()
        bytes_read = 0
        
        while True:
            elapsed = time.time() - start_time
            time_since_last_data = time.time() - last_data_time
            
            # Timeout check
            if elapsed > timeout:
                print("Timeout erreicht!")
                break
                
            # Stoppe wenn 3 Sekunden keine neuen Daten UND wir bereits Daten haben
            if response and time_since_last_data > 3:
                print("Keine neuen Daten seit 3s - fertig!")
                break
            
            # Lese verfügbare Daten
            if ser.in_waiting > 0:
                new_data = ser.read_all().decode('utf-8', errors='ignore')
                response += new_data
                bytes_read += len(new_data)
                last_data_time = time.time()
                
                print(f"+{len(new_data)} bytes (gesamt: {bytes_read})")
                
                # Debug preview
                preview = new_data.replace('\r', '<CR>').replace('\n', '<LF>')
                if len(preview) > 30:
                    preview = preview[:30] + "..."
                print(f"   Preview: '{preview}'")
            
            time.sleep(0.1)  # Kurze Pause
        
        # Port schließen
        ser.close()
        print(f"Port {port} geschlossen")
        
        # Ergebnisse anzeigen
        print()
        print("=" * 60)
        print("                TARGET ANTWORT")
        print("=" * 60)
        
        if response.strip():
            print(response)
            
            # Analyse
            print("=" * 60)
            print("ANALYSE:")
            print(f"   Bytes erhalten: {len(response)}")
            print(f"   Zeilen: {len(response.split(chr(10)))}")
            
            # Prüfe auf Test-Gruppe
            if "Test" in response and "Commands" in response:
                print("   ✓ Test-Gruppe gefunden!")
            else:
                print("   ✗ Test-Gruppe nicht gefunden")
            
            # Prüfe auf LAN8651-Kommandos
            if "lan_read" in response or "lan_write" in response:
                print("   ✓ LAN8651-Kommandos verfügbar!")
                return "lan_commands_found"
            else:
                print("   ✗ LAN8651-Kommandos fehlen - Firmware neu flashen!")
                return "no_lan_commands"
                
        else:
            print("FEHLER: KEINE ANTWORT ERHALTEN")
            print()
            print("Mögliche Ursachen:")
            print("   - Target nicht eingeschaltet")
            print("   - Falsche Baudrate")
            print("   - Target hängt oder nicht programmiert")
            print("   - Hardware-Problem")
            return False
            
        print("=" * 60)
        return True
        
    except serial.SerialException as e:
        print(f"SERIELLER FEHLER: {e}")
        return False
    except PermissionError:
        print(f"ZUGRIFF VERWEIGERT: {port} wird von anderem Programm verwendet!")
        return False
    except Exception as e:
        print(f"UNERWARTETER FEHLER: {e}")
        return False

def test_lan8651_registers(port="COM8", baudrate=115200):
    """
    Teste LAN8651 Register-Kommandos falls verfügbar
    """
    print("\n" + "=" * 60)
    print("         LAN8651 REGISTER TEST")
    print("=" * 60)
    
    test_commands = [
        ("Test help", "Zeige verfügbare Test-Kommandos"),
        ("Test lan_read 0x00000000", "Lese Chip ID Register"),
        ("Test lan_read 0x00000001", "Lese Status Register"),
        ("Test lan_write 0x00000050 0x12345678", "Schreibe Test-Register")
    ]
    
    try:
        ser = serial.Serial(port, baudrate, timeout=5)
        
        for command, description in test_commands:
            print(f"\n--- {description} ---")
            print(f"Kommando: {command}")
            
            # CLI aktivieren
            ser.write(b'\r\n')
            time.sleep(0.3)
            if ser.in_waiting > 0:
                ser.read_all()  # Puffer leeren
            
            # Kommando senden
            ser.write((command + '\r\n').encode())
            time.sleep(2)
            
            # Antwort lesen
            response = ""
            for _ in range(30):  # Max 3 Sekunden
                if ser.in_waiting > 0:
                    response += ser.read_all().decode('utf-8', errors='ignore')
                    time.sleep(0.1)
                else:
                    time.sleep(0.1)
            
            if response.strip():
                print("Antwort:")
                print(response.strip())
            else:
                print("Keine Antwort erhalten")
            
            print("-" * 40)
            
        ser.close()
        
    except Exception as e:
        print(f"Fehler beim Register-Test: {e}")

if __name__ == "__main__":
    print("Starte COM8 Test...")
    
    # Basis-Test mit 'help'
    result = test_com8("help")
    
    if result == "lan_commands_found":
        print("\n🎉 LAN8651-Kommandos gefunden! Starte Register-Tests...")
        test_lan8651_registers()
    elif result == "no_lan_commands":
        print("\n💡 Test-Gruppe ist da, aber LAN-Kommandos fehlen noch.")
        print("   Teste trotzdem die Test-Gruppe...")
        test_com8("Test help")
    elif result:
        print("\n✓ Basis-Test erfolgreich")
    else:
        print("\n✗ Test fehlgeschlagen")
    
    input("\nDrücken Sie Enter zum Beenden...")