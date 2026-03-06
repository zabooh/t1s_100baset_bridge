import serial
import time

def test_command(cmd, description):
    print(f"\n--- {description} ---")
    print(f"Kommando: {cmd}")
    
    try:
        ser = serial.Serial('COM8', 115200, timeout=3)
        
        # CLI aktivieren
        ser.write(b'\r\n')
        time.sleep(0.3)
        if ser.in_waiting > 0:
            ser.read_all()
        
        # Kommando senden
        ser.write((cmd + '\r\n').encode())
        time.sleep(1.5)
        
        response = ""
        while ser.in_waiting > 0:
            response += ser.read_all().decode('utf-8', errors='ignore')
            time.sleep(0.1)
            
        ser.close()
        
        if response.strip():
            print("Antwort:")
            lines = response.strip().split('\n')
            for line in lines[:3]:  # Erste 3 Zeilen
                print(f"  {line.strip()}")
            
            if "unknown command" in response:
                print("  ❌ Kommando nicht erkannt")
                return False
            else:
                print("  ✅ Kommando funktioniert")
                return True
        else:
            print("  ⚠️ Keine Antwort")
            return False
            
    except Exception as e:
        print(f"  ❌ Fehler: {e}")
        return False

print("=== KOMMANDO-GRUPPEN DIAGNOSE ===")

# Teste verschiedene Kommando-Gruppen
tests = [
    ("help", "Basis Help"),
    ("tcpip help", "TCP/IP Kommandos"),
    ("iperf help", "iPerf Kommandos"), 
    ("Test help", "Test Kommandos (unsere)"),
    ("reset", "Reset Kommando"),
]

working_commands = 0
for cmd, desc in tests:
    if test_command(cmd, desc):
        working_commands += 1

print(f"\n=== ERGEBNIS ===")
print(f"Funktionierende Kommandos: {working_commands}/{len(tests)}")

if working_commands == 0:
    print("❌ KEINE Kommandos funktionieren - CLI Problem!")
elif working_commands < len(tests):
    print("⚠️ Nur MANCHE Kommandos funktionieren - Test-Gruppe Problem!")
else:
    print("✅ ALLE Kommandos funktionieren!")