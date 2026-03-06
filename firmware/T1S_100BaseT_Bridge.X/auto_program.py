#!/usr/bin/env python3
"""
MPLABX Auto-Programmer
Automatisierte Firmware-Programmierung für ATSAME54P20A
"""

import subprocess
import os
import time
import sys
from pathlib import Path

def find_mplabx_tools():
    """Find MPLABX installation and tools"""
    possible_paths = [
        r"C:\Program Files\Microchip\MPLABX\v6.25",
        r"C:\Program Files (x86)\Microchip\MPLABX\v6.25",
        r"C:\Program Files\Microchip\MPLABX\v6.20"
    ]
    
    for path in possible_paths:
        if os.path.exists(path):
            return path
    return None

def program_firmware():
    """Program the firmware using various methods"""
    print("=== MPLABX AUTO-PROGRAMMER ===")
    
    # Check if hex file exists
    hex_file = Path("_build/T1S_100BaseT_Bridge/default/default.hex")
    if not hex_file.exists():
        print(f"❌ Hex file not found: {hex_file}")
        return False
    
    print(f"✓ Hex file found: {hex_file}")
    
    # Find MPLABX installation
    mplabx_path = find_mplabx_tools()
    if not mplabx_path:
        print("❌ MPLABX installation not found")
        return False
    
    print(f"✓ MPLABX found: {mplabx_path}")
    
    # Try different programming methods
    methods = [
        # Method 1: IPE with correct syntax
        {
            "name": "MPLAB IPE",
            "cmd": [
                os.path.join(mplabx_path, "mplab_platform", "bin", "mplab_ipe64.exe"),
                "-TP", "PK4",
                "-P", "ATSAME54P20A", 
                "-F", str(hex_file.absolute()),
                "-M"
            ]
        },
        # Method 2: MDB with project
        {
            "name": "MPLAB MDB",
            "cmd": [
                os.path.join(mplabx_path, "mplab_platform", "bin", "mdb.bat"),
                "T1S_100BaseT_Bridge.X",
                "-M",
                "-P", "ATSAME54P20A",
                "-T", "PK4", 
                "-F", str(hex_file.absolute())
            ]
        }
    ]
    
    for method in methods:
        print(f"\n--- Trying {method['name']} ---")
        try:
            # Check if tool exists
            tool_path = method['cmd'][0]
            if not os.path.exists(tool_path):
                print(f"❌ Tool not found: {tool_path}")
                continue
                
            print(f"Command: {' '.join(method['cmd'])}")
            
            # Run the programming command
            result = subprocess.run(
                method['cmd'],
                capture_output=True,
                text=True, 
                timeout=120  # 2 minutes timeout
            )
            
            print(f"Return code: {result.returncode}")
            if result.stdout:
                print(f"Output: {result.stdout}")
            if result.stderr:
                print(f"Error: {result.stderr}")
                
            if result.returncode == 0:
                print(f"✅ {method['name']} SUCCESS!")
                return True
            else:
                print(f"❌ {method['name']} failed with code {result.returncode}")
                
        except subprocess.TimeoutExpired:
            print(f"❌ {method['name']} timed out")
        except Exception as e:
            print(f"❌ {method['name']} error: {e}")
    
    print("\n❌ All programming methods failed")
    return False

def verify_programming():
    """Verify that new firmware is running"""
    print("\n=== VERIFICATION ===")
    print("Waiting 5 seconds for device to restart...")
    time.sleep(5)
    
    try:
        import serial
        ser = serial.Serial('COM8', 115200, timeout=3)
        time.sleep(1)
        
        # Send reset to get fresh output
        ser.write(b'reset\r\n')
        time.sleep(2)
        
        response = ser.read_all().decode('utf-8', errors='ignore')
        ser.close()
        
        # Look for build timestamp
        if "Build Timestamp:" in response:
            lines = response.split('\n')
            for line in lines:
                if "Build Timestamp:" in line:
                    timestamp = line.strip()
                    print(f"📅 {timestamp}")
                    
                    # Check if it's today's build  
                    from datetime import datetime
                    now = datetime.now()
                    today_str = now.strftime("%b %d %Y")  # Mar 06 2026
                    
                    if today_str.replace(" 0", " ") in timestamp:
                        print("✅ NEW FIRMWARE CONFIRMED!")
                        return True
                    else:
                        print("⚠️ Build timestamp is old")
                        return False
        
        print("❌ No build timestamp found in reset output")
        return False
        
    except Exception as e:
        print(f"❌ Verification error: {e}")
        return False

if __name__ == "__main__":
    print("Starting firmware programming process...")
    
    # Step 1: Program firmware
    if program_firmware():
        # Step 2: Verify new firmware
        if verify_programming():
            print("\n🎉 FIRMWARE PROGRAMMING COMPLETE!")
            sys.exit(0)
        else:
            print("\n⚠️ Programming may have succeeded but verification failed")
            sys.exit(1)
    else:
        print("\n❌ FIRMWARE PROGRAMMING FAILED")
        print("\nMANUAL OPTION:")
        print("1. Open MPLAB IDE (already started)")
        print("2. Load project T1S_100BaseT_Bridge.X") 
        print("3. Build project (Ctrl+F11)")
        print("4. Program device (Ctrl+F5)")
        sys.exit(1)