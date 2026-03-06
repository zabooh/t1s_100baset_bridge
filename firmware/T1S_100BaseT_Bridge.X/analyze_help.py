import serial
import time

def get_help_output():
    """Hole die komplette help-Ausgabe für Analyse"""
    try:
        ser = serial.Serial('COM8', 115200, timeout=5)
        
        # CLI aktivieren
        ser.write(b'\r\n')
        time.sleep(0.5)
        if ser.in_waiting > 0:
            ser.read_all()
        
        # Help kommando
        ser.write(b'help\r\n')
        time.sleep(2)
        
        response = ""
        while ser.in_waiting > 0:
            response += ser.read_all().decode('utf-8', errors='ignore')
            time.sleep(0.1)
        
        ser.close()
        return response
        
    except Exception as e:
        return f"Fehler: {e}"

print("=== VOLLSTÄNDIGE HELP-ANALYSE ===")
help_output = get_help_output()

print("HELP-AUSGABE:")
print("-" * 50)
print(help_output)
print("-" * 50)

# Analysiere die Gruppen
lines = help_output.split('\n')
command_groups = []

for line in lines:
    line = line.strip()
    if '***' in line and ':' in line:
        # Extrahiere Gruppennamen
        if 'Test:' in line:
            command_groups.append("TEST-GRUPPE GEFUNDEN ✓")
        elif 'iperf:' in line:
            command_groups.append("iperf-Gruppe gefunden ✓") 
        elif 'tcpip:' in line:
            command_groups.append("tcpip-Gruppe gefunden ✓")

print("\n=== GRUPPEN-ANALYSE ===")
for group in command_groups:
    print(f"  {group}")

# Prüfe ob unsere Test-Gruppe da ist
if "TEST-GRUPPE GEFUNDEN" in "\n".join(command_groups):
    print("\n🔍 DIAGNOSE:")
    print("✓ Test-Gruppe ist registriert")
    print("❌ Aber 'Test help' funktioniert nicht")
    print("💡 Problem: Kommando-Handler-Verknüpfung")
    
    print("\n🛠️ MÖGLICHE URSACHEN:")
    print("1. SYS_CMD_ADDGRP hat teilweise funktioniert")
    print("2. Kommando-Funktionen sind nicht richtig verlinkt") 
    print("3. Memory/Stack-Problem bei der Initialisierung")
    print("4. Command_Init() Return-Wert Problem")
else:
    print("\n❌ Test-Gruppe wurde NICHT registriert")
    print("💡 Command_Init() wird nicht oder falsch aufgerufen")