import serial
import time

print("=== SCHNELL-TEST NACH PROGRAMMIERUNG ===")

try:
    ser = serial.Serial('COM8', 115200, timeout=3)
    
    # CLI aktivieren 
    ser.write(b'\r\n')
    time.sleep(0.5)
    if ser.in_waiting > 0:
        ser.read_all()
    
    # Test help kommando
    ser.write(b'Test help\r\n')
    time.sleep(2)
    
    response = ""
    while ser.in_waiting > 0:
        response += ser.read_all().decode('utf-8', errors='ignore')
        time.sleep(0.1)
    
    print("ANTWORT:")
    print(response)
    
    if "lan_read" in response:
        print("\n🎉 SUCCESS: LAN8651-Kommandos verfügbar!")
    elif "ipdump" in response:
        print("\n✅ Test-Kommandos da, LAN8651 evtl. nicht")
    else:
        print("\n❌ Unerwartete Antwort")
    
    ser.close()
    
except Exception as e:
    print(f"Fehler: {e}")