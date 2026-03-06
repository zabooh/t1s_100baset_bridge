import serial
import time

print("=== TEST NACH LOGIC-FIX ===")
print("⚠️  Nur ausführen NACH Build + Program!")
print()

try:
    ser = serial.Serial('COM8', 115200, timeout=3)
    print("✓ COM8 geöffnet")
    
    # CLI aktivieren
    ser.write(b'\r\n')
    time.sleep(0.5)
    if ser.in_waiting > 0:
        ser.read_all()
    
    print("📤 Teste: Test help")
    ser.write(b'Test help\r\n')
    time.sleep(2)
    
    response = ""
    while ser.in_waiting > 0:
        response += ser.read_all().decode('utf-8', errors='ignore')
        time.sleep(0.1)
    
    print("📥 ANTWORT:")
    print("-" * 40)
    print(response)
    print("-" * 40)
    
    if "lan_read" in response or "lan_write" in response:
        print("\n🎉 SUCCESS! LAN8651-Kommandos sind verfügbar!")
        print("   Versuche LAN Register Test...")
        
        # Test eines echten LAN8651 Kommandos
        ser.write(b'Test lan_read 0x00000000\r\n')  # Chip ID
        time.sleep(3)
        
        lan_response = ""
        while ser.in_waiting > 0:
            lan_response += ser.read_all().decode('utf-8', errors='ignore')
            time.sleep(0.1)
        
        print("\n📱 LAN8651 REGISTER TEST:")
        print(lan_response)
        
    elif "ipdump" in response:
        print("\n🔧 Test-Kommandos funktionieren jetzt!")
        print("   Aber LAN8651-Kommandos sind nicht sichtbar - Code-Problem?")
        
    elif "unknown command" in response:
        print("\n❌ FEHLER: Test help immer noch unknown!")
        print("   Command_Init() Problem oder Build/Flash nicht erfolgt")
    else:
        print("\n❓ Unerwartete Antwort - analysiere...")
    
    ser.close()
    
except Exception as e:
    print(f"❌ Fehler: {e}")

print("\n" + "="*50)