#!/usr/bin/env python3
"""
MPU RX Performance Diagnostic Test Suite
Automated diagnostic runner based on MPU RX Diagnostic Plan

Systematically analyzes 85% packet loss issue on MCU→MPU T1S bridge
Tests interrupt load, network stack limits, memory pressure, hardware registers,
and kernel parameter optimization.

Usage:
  python mpu_rx_diagnostic_test.py
  python mpu_rx_diagnostic_test.py --mcu-port COM8 --mpu-port COM9
  python mpu_rx_diagnostic_test.py --skip-phases 1,2 --test-duration 15
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Set

import serial


@dataclass
class TestResult:
    """Single test result with timing and success info."""
    phase: str
    step: str
    command: str
    output: str
    success: bool
    duration_sec: float
    error_message: str = ""


@dataclass 
class DiagnosticReport:
    """Complete diagnostic report with all phases."""
    timestamp: str
    config: Dict[str, object] = field(default_factory=dict)
    baseline: Dict[str, object] = field(default_factory=dict)
    phases: Dict[str, object] = field(default_factory=dict)
    optimization_results: Dict[str, object] = field(default_factory=dict)
    analysis: Dict[str, object] = field(default_factory=dict)
    recommendations: List[str] = field(default_factory=list)


class SerialCLI:
    """Serial command line interface for MCU/MPU control."""
    
    def __init__(self, port: str, baudrate: int, timeout: float = 0.3):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser: Optional[serial.Serial] = None

    def open(self) -> None:
        self.ser = serial.Serial(
            port=self.port,
            baudrate=self.baudrate,
            timeout=self.timeout,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
        )

    def close(self) -> None:
        if self.ser and self.ser.is_open:
            self.ser.close()

    def clear_input(self) -> None:
        if not self.ser:
            return
        _ = self.ser.read_all()

    def write_line(self, line: str) -> None:
        if not self.ser:
            raise RuntimeError(f"Serial port {self.port} is not open")
        self.ser.write((line + "\n").encode("utf-8", errors="ignore"))
        self.ser.flush()

    def write_ctrl_c(self) -> None:
        if not self.ser:
            raise RuntimeError(f"Serial port {self.port} is not open")
        self.ser.write(b"\x03")
        self.ser.flush()

    def run_command(
        self,
        command: str,
        timeout: float = 10.0,
        expect_prompt: bool = True,
        live_output: bool = False,
        label: str = ""
    ) -> TestResult:
        """Execute command and return structured result."""
        start_time = time.time()
        
        if not self.ser:
            return TestResult(
                phase="", step="", command=command, output="",
                success=False, duration_sec=0.0,
                error_message="Serial port not open"
            )

        self.clear_input()
        if live_output and label:
            print(f"[{label}] $ {command}")

        try:
            self.write_line(command)
            output = ""
            end_time = start_time + timeout
            
            while time.time() < end_time:
                chunk = self.ser.read_all().decode("utf-8", errors="ignore")
                if chunk:
                    output += chunk
                    if live_output and label:
                        # Print new lines as they arrive
                        for line in chunk.splitlines():
                            if line.strip():
                                print(f"[{label}] {line}")
                    
                    # Check for prompt (simple heuristic)
                    if expect_prompt and re.search(r'[>#$]\s*$', output):
                        break
                
                time.sleep(0.1)

            duration = time.time() - start_time
            success = not ("command not found" in output.lower() or 
                          "no such file" in output.lower() or
                          "error" in output.lower())
            
            return TestResult(
                phase="", step="", command=command, output=output,
                success=success, duration_sec=duration
            )
            
        except Exception as e:
            duration = time.time() - start_time
            return TestResult(
                phase="", step="", command=command, output="",
                success=False, duration_sec=duration,
                error_message=str(e)
            )


class MPURXDiagnosticRunner:
    """Main diagnostic test runner implementing the diagnostic plan."""
    
    def __init__(
        self,
        mcu_port: str = "COM8",
        mpu_port: str = "COM9", 
        baudrate: int = 115200,
        mcu_ip: str = "192.168.0.200",
        mpu_ip: str = "192.168.0.5",
        test_duration: int = 30,
        skip_phases: Optional[Set[int]] = None
    ):
        self.mcu = SerialCLI(mcu_port, baudrate)
        self.mpu = SerialCLI(mpu_port, baudrate)
        self.mcu_ip = mcu_ip
        self.mpu_ip = mpu_ip
        self.test_duration = test_duration
        self.skip_phases = skip_phases or set()
        
        self.report = DiagnosticReport(
            timestamp=datetime.now().isoformat(),
            config={
                "mcu_port": mcu_port,
                "mpu_port": mpu_port, 
                "baudrate": baudrate,
                "mcu_ip": mcu_ip,
                "mpu_ip": mpu_ip,
                "test_duration": test_duration,
                "skip_phases": list(self.skip_phases) if self.skip_phases else [],
            }
        )

    def run_diagnostic(self) -> DiagnosticReport:
        """Execute complete diagnostic test suite."""
        print("🔍 MPU RX Performance Diagnostic Test Suite")
        print("=" * 60)
        print(f"Timestamp: {self.report.timestamp}")
        print(f"Configuration: {json.dumps(self.report.config, indent=2)}")
        print("=" * 60)

        try:
            print("[INFO] Opening serial connections...")
            self.mcu.open()
            self.mpu.open()
            
            # Wake up both systems
            print("[INFO] Synchronizing prompts...")
            self.mcu.clear_input()
            self.mpu.clear_input()
            self.mcu.write_line("")
            self.mpu.write_line("")
            time.sleep(0.5)

            # Execute diagnostic phases
            if 1 not in self.skip_phases:
                self._run_phase1_baseline()
                
            if 2 not in self.skip_phases:
                self._run_phase2_interrupt_analysis()
                
            if 3 not in self.skip_phases:
                self._run_phase3_network_stack()
                
            if 4 not in self.skip_phases:
                self._run_phase4_hardware_diagnostics()
                
            if 5 not in self.skip_phases:
                self._run_phase5_kernel_optimization()
                
            self._run_phase6_analysis_and_report()

        except Exception as e:
            print(f"[ERROR] Diagnostic failed: {e}")
            self.report.analysis["fatal_error"] = str(e)
            
        finally:
            print("[INFO] Closing serial connections...")
            self.mcu.close()
            self.mpu.close()

        return self.report

    def _run_phase1_baseline(self) -> None:
        """Phase 1: Baseline System Information."""
        print("\n📊 Phase 1: Baseline System Information")
        print("-" * 40)
        
        phase_data = {"start_time": datetime.now().isoformat(), "results": []}

        # 1.1 System Status Collection
        print("  1.1 System Status Collection...")
        system_commands = [
            "cat /proc/cpuinfo | grep -E '(processor|model name|cpu MHz|bogomips)'",
            "cat /proc/meminfo | grep -E '(MemTotal|MemFree|MemAvailable|Buffers|Cached)'", 
            "cat /proc/version",
            "uname -a",
            "ip addr show eth0",
            "ip link show eth0", 
            "ifconfig eth0",
            "cat /proc/sys/net/core/netdev_max_backlog",
            "cat /proc/sys/net/core/rmem_default",
            "cat /proc/sys/net/core/rmem_max",
            "cat /proc/sys/net/core/netdev_budget",
        ]
        
        for cmd in system_commands:
            result = self.mpu.run_command(cmd, timeout=5.0, live_output=True, label="MPU")
            result.phase = "Phase1"
            result.step = "SystemInfo"
            phase_data["results"].append({
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        # 1.2 Hardware Register Snapshot
        print("  1.2 Hardware Register Snapshot...")
        register_commands = [
            "lan_read 0x00800004",  # PHY_STS - Link Quality
            "lan_read 0x00800100",  # SQI - Signal Quality Index
            "lan_read 0x00800304",  # PLCA_CTRL0 - PLCA Config
            "lan_read 0x00800306",  # PLCA_CTRL1 - Node ID
            "lan_read 0x00010000",  # MAC_NET_CTL
            "lan_read 0x00010001",  # MAC_NET_CFG
            "lan_read 0x00000008",  # IRQ_STS
            "lan_read 0x000000E0",  # DMY_CTRL
            "lan_read 0x00020000",  # RX_GOOD_FRAMES (estimated)
            "lan_read 0x00020004",  # RX_BAD_FRAMES
            "lan_read 0x00020040",  # RX_OVERSIZE_FRAMES
        ]
        
        for cmd in register_commands:
            result = self.mpu.run_command(cmd, timeout=3.0, live_output=True, label="MPU-REG")
            result.phase = "Phase1"
            result.step = "RegisterSnapshot"
            phase_data["results"].append({
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        phase_data["end_time"] = datetime.now().isoformat()
        self.report.phases["phase1_baseline"] = phase_data
        print(f"  ✓ Phase 1 completed: {len(phase_data['results'])} commands executed")

    def _run_phase2_interrupt_analysis(self) -> None:
        """Phase 2: Interrupt and CPU Load Analysis."""
        print("\n🚨 Phase 2: Interrupt and CPU Load Analysis")
        print("-" * 40)
        
        phase_data = {"start_time": datetime.now().isoformat(), "results": []}

        # 2.1 Pre-Test Interrupt Baseline
        print("  2.1 Pre-Test Interrupt Baseline...")
        baseline_commands = [
            "cat /proc/interrupts",
            "cat /proc/stat | grep ctxt",
            "cat /proc/loadavg",
            "cat /proc/net/dev | grep eth0",
        ]
        
        baseline_data = {}
        for cmd in baseline_commands:
            result = self.mpu.run_command(cmd, timeout=5.0, live_output=True, label="MPU-BASE")
            baseline_data[cmd] = result.output
            phase_data["results"].append({
                "step": "baseline",
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        # 2.2 Concurrent iperf + monitoring test
        print(f"  2.2 Running {self.test_duration}s iperf test with monitoring...")
        
        # Start MPU iperf server
        print("    Starting MPU iperf server...")
        iperf_start = self.mpu.run_command(
            "iperf -s -u -i 1 > /tmp/iperf_server.log 2>&1 &",
            timeout=5.0, expect_prompt=True, live_output=True, label="MPU-IPERF"
        )
        time.sleep(2)  # Give server time to start

        # Start monitoring scripts on MPU
        print("    Starting monitoring scripts...")
        monitor_commands = [
            "while sleep 1; do echo \"$(date '+%H:%M:%S'): $(cat /proc/interrupts | grep -E 'eth0|lan865' | head -3)\"; done > /tmp/irq_monitor.log &",
            "while sleep 1; do echo \"$(date '+%H:%M:%S'): Load=$(cat /proc/loadavg), NetDev=$(cat /proc/net/dev | grep eth0)\"; done > /tmp/system_load.log &",
        ]
        
        for cmd in monitor_commands:
            result = self.mpu.run_command(cmd, timeout=3.0, expect_prompt=True, label="MPU-MON")
            phase_data["results"].append({
                "step": "monitoring_start",
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        # Start MCU iperf client (this should cause 85% packet loss)
        print("    Starting MCU iperf client...")
        mcu_iperf = self.mcu.run_command(
            f"iperf -u -c {self.mpu_ip}",
            timeout=float(self.test_duration + 10), 
            expect_prompt=True, live_output=True, label="MCU-CLIENT"
        )
        
        phase_data["results"].append({
            "step": "mcu_client",
            "command": mcu_iperf.command,
            "output": mcu_iperf.output,
            "success": mcu_iperf.success,
            "duration": mcu_iperf.duration_sec
        })

        # Stop monitoring and iperf server
        print("    Stopping monitoring and server...")
        time.sleep(2)  # Let monitoring catch final data
        
        stop_commands = [
            "killall iperf 2>/dev/null || true",
            "kill %1 %2 2>/dev/null || true",  # Kill background jobs
            "cat /tmp/iperf_server.log",
            "cat /tmp/irq_monitor.log | tail -10",
            "cat /tmp/system_load.log | tail -10",
        ]
        
        for cmd in stop_commands:
            result = self.mpu.run_command(cmd, timeout=5.0, live_output=True, label="MPU-STOP")
            phase_data["results"].append({
                "step": "monitoring_stop",
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        # 2.3 Post-test comparison
        print("  2.3 Post-Test Analysis...")
        for cmd in baseline_commands:
            result = self.mpu.run_command(cmd, timeout=5.0, live_output=True, label="MPU-POST")
            phase_data["results"].append({
                "step": "post_analysis", 
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        phase_data["end_time"] = datetime.now().isoformat()
        self.report.phases["phase2_interrupt_analysis"] = phase_data
        print(f"  ✓ Phase 2 completed: {len(phase_data['results'])} commands executed")

    def _run_phase3_network_stack(self) -> None:
        """Phase 3: Kernel Network Stack Deep-Dive."""
        print("\n📦 Phase 3: Kernel Network Stack Deep-Dive")
        print("-" * 40)
        
        phase_data = {"start_time": datetime.now().isoformat(), "results": []}

        # 3.1 Network Stack Counter Analysis
        print("  3.1 Network Stack Counter Analysis...")
        
        # Baseline counters
        baseline_commands = [
            "cat /proc/net/snmp",
            "cat /proc/net/dev",
            "cat /proc/net/softnet_stat",
        ]
        
        for cmd in baseline_commands:
            result = self.mpu.run_command(f"{cmd} > /tmp/{cmd.split('/')[-1]}_baseline.txt", 
                                        timeout=5.0, expect_prompt=True, label="MPU-NET-BASE")
            phase_data["results"].append({
                "step": "network_baseline",
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        # Continuous network monitoring script
        monitoring_script = '''
#!/bin/bash
while true; do
    TIMESTAMP=$(date '+%H:%M:%S')
    DROPS=$(cat /proc/net/dev | grep eth0 | awk '{print $4}')
    FIFO=$(cat /proc/net/dev | grep eth0 | awk '{print $5}')
    UDP_ERRORS=$(cat /proc/net/snmp | grep "Udp:" | tail -1 | awk '{print $4}')
    UDP_RCVBUF=$(cat /proc/net/snmp | grep "Udp:" | tail -1 | awk '{print $6}')
    SOFTNET=$(cat /proc/net/softnet_stat | head -1)
    echo "$TIMESTAMP: drops=$DROPS fifo=$FIFO udp_err=$UDP_ERRORS udp_rcvbuf=$UDP_RCVBUF softnet=[$SOFTNET]"
    sleep 0.5
done > /tmp/detailed_net_stats.log
'''
        
        # Create and start monitoring script
        result = self.mpu.run_command(f'cat > /tmp/net_monitor.sh << \'EOF\'\n{monitoring_script}\nEOF', 
                                     timeout=3.0, expect_prompt=True, label="MPU-NET-SCRIPT")
        
        result = self.mpu.run_command("chmod +x /tmp/net_monitor.sh", timeout=2.0, expect_prompt=True, label="MPU-NET-CHMOD")
        
        result = self.mpu.run_command("/tmp/net_monitor.sh &", timeout=3.0, expect_prompt=True, label="MPU-NET-START")
        
        # Run iperf test with network monitoring
        print(f"  3.2 Running {self.test_duration}s iperf with detailed network monitoring...")
        
        # Start server
        result = self.mpu.run_command("iperf -s -u -i 1 > /tmp/iperf_netstack.log 2>&1 &", 
                                     timeout=5.0, expect_prompt=True, label="MPU-IPERF-NET")
        time.sleep(2)
        
        # MCU client
        mcu_iperf = self.mcu.run_command(
            f"iperf -u -c {self.mpu_ip}",
            timeout=float(self.test_duration + 10),
            expect_prompt=True, live_output=True, label="MCU-NET-CLIENT"
        )
        
        phase_data["results"].append({
            "step": "network_iperf",
            "command": mcu_iperf.command,
            "output": mcu_iperf.output,
            "success": mcu_iperf.success,
            "duration": mcu_iperf.duration_sec
        })

        # Stop monitoring and collect results
        time.sleep(1)
        stop_commands = [
            "killall iperf net_monitor.sh 2>/dev/null || true",
            "cat /tmp/iperf_netstack.log",
            "cat /tmp/detailed_net_stats.log | tail -20",
        ]
        
        for cmd in stop_commands:
            result = self.mpu.run_command(cmd, timeout=5.0, live_output=True, label="MPU-NET-STOP")
            phase_data["results"].append({
                "step": "network_stop",
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        # Final comparison
        for cmd in baseline_commands:
            result = self.mpu.run_command(cmd, timeout=5.0, live_output=True, label="MPU-NET-FINAL")
            phase_data["results"].append({
                "step": "network_final",
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        phase_data["end_time"] = datetime.now().isoformat() 
        self.report.phases["phase3_network_stack"] = phase_data
        print(f"  ✓ Phase 3 completed: {len(phase_data['results'])} commands executed")

    def _run_phase4_hardware_diagnostics(self) -> None:
        """Phase 4: Hardware Layer Diagnostics."""
        print("\n🔧 Phase 4: Hardware Layer Diagnostics")  
        print("-" * 40)
        
        phase_data = {"start_time": datetime.now().isoformat(), "results": []}

        # 4.1 T1S Link Quality During Load
        print("  4.1 T1S Link Quality During Load...")
        
        phy_monitoring_script = '''
#!/bin/bash
while true; do
    TIMESTAMP=$(date '+%H:%M:%S')
    PHY_STS=$(lan_read 0x00800004 2>/dev/null | grep -o "0x[0-9A-Fa-f]*" | head -1)
    SQI_VAL=$(lan_read 0x00800100 2>/dev/null | grep -o "0x[0-9A-Fa-f]*" | head -1)
    PLCA_STS=$(lan_read 0x00800304 2>/dev/null | grep -o "0x[0-9A-Fa-f]*" | head -1)
    IRQ_STS=$(lan_read 0x00000008 2>/dev/null | grep -o "0x[0-9A-Fa-f]*" | head -1)
    echo "$TIMESTAMP: PHY=$PHY_STS SQI=$SQI_VAL PLCA=$PLCA_STS IRQ=$IRQ_STS"
    sleep 2
done > /tmp/phy_quality.log
'''
        
        # Create PHY monitoring script
        result = self.mpu.run_command(f'cat > /tmp/phy_monitor.sh << \'EOF\'\n{phy_monitoring_script}\nEOF',
                                     timeout=3.0, expect_prompt=True, label="MPU-PHY-SCRIPT")
        
        result = self.mpu.run_command("chmod +x /tmp/phy_monitor.sh", timeout=2.0, expect_prompt=True, label="MPU-PHY-CHMOD")
        
        result = self.mpu.run_command("/tmp/phy_monitor.sh &", timeout=3.0, expect_prompt=True, label="MPU-PHY-START")
        
        # Combined iperf + PHY monitoring test
        print(f"  4.2 Running {self.test_duration}s iperf with PHY monitoring...")
        
        result = self.mpu.run_command("iperf -s -u -i 1 > /tmp/iperf_phy.log 2>&1 &",
                                     timeout=5.0, expect_prompt=True, label="MPU-IPERF-PHY")
        time.sleep(2)
        
        mcu_iperf = self.mcu.run_command(
            f"iperf -u -c {self.mpu_ip}",
            timeout=float(self.test_duration + 10),
            expect_prompt=True, live_output=True, label="MCU-PHY-CLIENT"
        )
        
        phase_data["results"].append({
            "step": "phy_iperf",
            "command": mcu_iperf.command,
            "output": mcu_iperf.output,
            "success": mcu_iperf.success,
            "duration": mcu_iperf.duration_sec
        })

        # Stop monitoring and analyze
        time.sleep(1)
        stop_commands = [
            "killall iperf phy_monitor.sh 2>/dev/null || true",
            "cat /tmp/iperf_phy.log",
            "cat /tmp/phy_quality.log",
        ]
        
        for cmd in stop_commands:
            result = self.mpu.run_command(cmd, timeout=5.0, live_output=True, label="MPU-PHY-STOP")
            phase_data["results"].append({
                "step": "phy_analysis",
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        # 4.3 Register Access Performance Impact Test
        print("  4.3 Register Access Performance Impact Test...")
        
        # Test 1: Baseline (no register access)
        print("    Test 1: Baseline (no register access)")
        result = self.mpu.run_command("timeout 30s iperf -s -u -i 1 > /tmp/iperf_baseline.log 2>&1 &",
                                     timeout=5.0, expect_prompt=True, label="MPU-REG-BASE")
        
        mcu_baseline = self.mcu.run_command(f"iperf -u -c {self.mpu_ip}", timeout=35.0, 
                                          expect_prompt=True, live_output=True, label="MCU-REG-BASE")
        phase_data["results"].append({
            "step": "register_baseline",
            "command": mcu_baseline.command,
            "output": mcu_baseline.output,
            "success": mcu_baseline.success,
            "duration": mcu_baseline.duration_sec
        })
        
        time.sleep(2)
        self.mpu.run_command("killall iperf 2>/dev/null || true", timeout=3.0, expect_prompt=True, label="MPU-KILL")

        # Test 2: With moderate register access
        print("    Test 2: With moderate register access (every 5s)")
        result = self.mpu.run_command("timeout 30s iperf -s -u -i 1 > /tmp/iperf_moderate.log 2>&1 &",
                                     timeout=5.0, expect_prompt=True, label="MPU-REG-MOD")

        # Start background register access
        reg_script = '''
#!/bin/bash
while sleep 5; do
    lan_read 0x00800004 >/dev/null 2>&1
    lan_read 0x00010000 >/dev/null 2>&1
done &
'''
        self.mpu.run_command(f'bash -c \'{reg_script}\'', timeout=3.0, expect_prompt=True, label="MPU-REG-ACCESS")
        
        mcu_moderate = self.mcu.run_command(f"iperf -u -c {self.mpu_ip}", timeout=35.0,
                                          expect_prompt=True, live_output=True, label="MCU-REG-MOD")
        phase_data["results"].append({
            "step": "register_moderate",
            "command": mcu_moderate.command,
            "output": mcu_moderate.output,
            "success": mcu_moderate.success,
            "duration": mcu_moderate.duration_sec
        })
        
        self.mpu.run_command("killall iperf bash 2>/dev/null || true", timeout=3.0, expect_prompt=True, label="MPU-KILL")

        phase_data["end_time"] = datetime.now().isoformat()
        self.report.phases["phase4_hardware_diagnostics"] = phase_data
        print(f"  ✓ Phase 4 completed: {len(phase_data['results'])} commands executed")

    def _run_phase5_kernel_optimization(self) -> None:
        """Phase 5: Kernel Parameter Optimization Testing."""
        print("\n🛠️  Phase 5: Kernel Parameter Optimization Testing")
        print("-" * 40)
        
        phase_data = {"start_time": datetime.now().isoformat(), "tests": [], "results": []}

        # Backup current settings
        print("  5.1 Backup current kernel parameters...")
        backup_commands = [
            "cat /proc/sys/net/core/netdev_max_backlog",
            "cat /proc/sys/net/core/netdev_budget",
        ]
        
        original_settings = {}
        for cmd in backup_commands:
            result = self.mpu.run_command(cmd, timeout=3.0, live_output=True, label="MPU-BACKUP")
            param_name = cmd.split('/')[-1]
            original_settings[param_name] = result.output.strip()
            phase_data["results"].append({
                "step": "backup",
                "command": result.command,
                "output": result.output,
                "success": result.success,
                "duration": result.duration_sec
            })

        # Define optimization tests
        optimization_tests = [
            {
                "name": "Baseline (default settings)",
                "settings": {},  # No changes
            },
            {
                "name": "Increased netdev_max_backlog to 10000",
                "settings": {"netdev_max_backlog": "10000"},
            },
            {
                "name": "Increased netdev_max_backlog to 50000", 
                "settings": {"netdev_max_backlog": "50000"},
            },
            {
                "name": "Increased netdev_budget to 600",
                "settings": {"netdev_max_backlog": original_settings.get("netdev_max_backlog", "300"), 
                           "netdev_budget": "600"},
            },
        ]

        # Run optimization tests
        for i, test_config in enumerate(optimization_tests, 1):
            print(f"  5.{i} {test_config['name']}")
            
            # Apply settings
            for param, value in test_config["settings"].items():
                cmd = f"echo {value} > /proc/sys/net/core/{param}"
                result = self.mpu.run_command(cmd, timeout=3.0, expect_prompt=True, label="MPU-OPT-SET")
                phase_data["results"].append({
                    "step": f"optimization_set_{param}",
                    "command": result.command,
                    "output": result.output,
                    "success": result.success,
                    "duration": result.duration_sec
                })

            # Run iperf test
            result = self.mpu.run_command("timeout 30s iperf -s -u -i 1 > /tmp/iperf_opt.log 2>&1 &",
                                         timeout=5.0, expect_prompt=True, label="MPU-OPT-IPERF")
            time.sleep(2)

            mcu_iperf = self.mcu.run_command(f"iperf -u -c {self.mpu_ip}", timeout=35.0,
                                           expect_prompt=True, live_output=True, label=f"MCU-OPT-{i}")
            
            # Get iperf server results
            server_result = self.mpu.run_command("cat /tmp/iperf_opt.log", timeout=3.0, live_output=True, label="MPU-OPT-RESULT")
            
            # Parse packet loss from results
            packet_loss = self._extract_packet_loss(mcu_iperf.output, server_result.output)
            
            test_result = {
                "test_name": test_config["name"],
                "settings": test_config["settings"],
                "mcu_client": {
                    "command": mcu_iperf.command,
                    "output": mcu_iperf.output,
                    "success": mcu_iperf.success,
                    "duration": mcu_iperf.duration_sec,
                    "packet_loss": packet_loss.get("client_loss", "unknown")
                },
                "mpu_server": {
                    "command": server_result.command,
                    "output": server_result.output,
                    "success": server_result.success,
                    "duration": server_result.duration_sec,
                    "packet_loss": packet_loss.get("server_loss", "unknown")
                }
            }
            
            phase_data["tests"].append(test_result)
            
            # Stop iperf
            self.mpu.run_command("killall iperf 2>/dev/null || true", timeout=3.0, expect_prompt=True, label="MPU-OPT-KILL")
            time.sleep(1)

        # Restore original settings
        print("  5.5 Restoring original kernel parameters...")
        for param, value in original_settings.items():
            if value and value.isdigit():
                cmd = f"echo {value} > /proc/sys/net/core/{param}"
                result = self.mpu.run_command(cmd, timeout=3.0, expect_prompt=True, label="MPU-RESTORE")
                phase_data["results"].append({
                    "step": f"restore_{param}",
                    "command": result.command,
                    "output": result.output,
                    "success": result.success,
                    "duration": result.duration_sec
                })

        phase_data["end_time"] = datetime.now().isoformat()
        self.report.phases["phase5_kernel_optimization"] = phase_data
        self.report.optimization_results = {
            "tests": phase_data["tests"],
            "original_settings": original_settings,
        }
        print(f"  ✓ Phase 5 completed: {len(phase_data['tests'])} optimization tests executed")

    def _run_phase6_analysis_and_report(self) -> None:
        """Phase 6: Analysis and Recommendations."""
        print("\n📋 Phase 6: Analysis and Recommendations")
        print("-" * 40)
        
        analysis = {
            "timestamp": datetime.now().isoformat(),
            "root_cause_analysis": {},
            "performance_impact": {},
            "recommendations": []
        }

        # Analyze optimization results
        if "optimization_results" in self.report.__dict__ and self.report.optimization_results.get("tests"):
            best_result = None
            baseline_loss = None
            
            for test in self.report.optimization_results["tests"]:
                test_name = test.get("test_name", "unknown")
                client_loss = test.get("mcu_client", {}).get("packet_loss", "unknown")
                
                print(f"    {test_name}: {client_loss} packet loss")
                
                if "Baseline" in test_name:
                    baseline_loss = client_loss
                elif best_result is None or (isinstance(client_loss, (int, float)) and 
                                           isinstance(best_result.get("packet_loss"), (int, float)) and
                                           client_loss < best_result["packet_loss"]):
                    best_result = {"test_name": test_name, "packet_loss": client_loss}

            # Generate recommendations based on results
            if best_result and baseline_loss:
                if isinstance(best_result["packet_loss"], (int, float)) and isinstance(baseline_loss, (int, float)):
                    improvement = baseline_loss - best_result["packet_loss"]
                    if improvement > 20:  # More than 20% improvement
                        analysis["recommendations"].append(
                            f"✓ Buffer tuning shows significant improvement: "
                            f"{best_result['test_name']} reduced packet loss by {improvement}% - make permanent"
                        )
                    elif improvement > 5:
                        analysis["recommendations"].append(
                            f"⚠ Buffer tuning shows modest improvement: "
                            f"{best_result['test_name']} reduced packet loss by {improvement}% - consider implementing"
                        )
                    else:
                        analysis["recommendations"].append(
                            "⚠ Buffer tuning has minimal impact - investigate hardware limits"
                        )
                else:
                    analysis["recommendations"].append(
                        "⚠ Unable to parse packet loss results - manual analysis required"
                    )

        # Additional recommendations based on phases executed
        if 2 not in self.skip_phases:
            analysis["recommendations"].append(
                "📊 Check Phase 2 interrupt monitoring logs for IRQ storms or excessive context switches"
            )
            
        if 4 not in self.skip_phases:
            analysis["recommendations"].append(
                "🔧 Review Phase 4 PHY quality logs for T1S link degradation during high traffic"
            )

        # Final assessment
        analysis["recommendations"].extend([
            "🔍 If buffer tuning ineffective: Root cause is likely hardware bottleneck (LAN8651/LAN9662 or ARM-CPU)",
            "⚡ If register access impacts performance: Consider conditional compilation of debugging features",
            "🚨 If IRQ storms detected: Implement interrupt coalescing or NAPI tuning",
            "📈 Consider MCU firmware fix for rate control (eliminate 1ms timer floor)"
        ])

        self.report.analysis = analysis
        
        print("\n" + "=" * 60)
        print("📋 DIAGNOSTIC ANALYSIS COMPLETE")
        print("=" * 60)
        print("\nRECOMMENDations:")
        for i, rec in enumerate(analysis["recommendations"], 1):
            print(f"  {i}. {rec}")

        print(f"\n💾 Full report available in generated files")
        print("🏁 Diagnostic execution completed successfully")

    def _extract_packet_loss(self, mcu_output: str, mpu_output: str) -> Dict[str, object]:
        """Extract packet loss percentages from iperf outputs."""
        result = {}
        
        # MCU client output pattern: [0.0-10.0 sec] 123/1000 (12%) 5000 Kbps
        mcu_pattern = r'\[\s*[\d\.]+\s*-\s*[\d\.]+\s*sec\]\s*(\d+)\s*/\s*(\d+)\s*\(\s*(\d+)%\)'
        mcu_match = re.search(mcu_pattern, mcu_output)
        if mcu_match:
            result["client_loss"] = int(mcu_match.group(3))
        
        # MPU server output pattern: similar format
        mpu_pattern = r'(\d+)/\s*(\d+)\s*\(\s*(\d+)%\)'
        mpu_match = re.search(mpu_pattern, mpu_output)  
        if mpu_match:
            result["server_loss"] = int(mpu_match.group(3))
            
        return result


def save_diagnostic_report(report: DiagnosticReport, output_dir: Path) -> Dict[str, Path]:
    """Save diagnostic report to JSON and text files."""
    output_dir.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    
    json_file = output_dir / f"mpu_rx_diagnostic_report_{timestamp}.json"
    txt_file = output_dir / f"mpu_rx_diagnostic_report_{timestamp}.txt"
    
    # Save JSON report
    with open(json_file, 'w', encoding='utf-8') as f:
        json.dump(report.__dict__, f, indent=2, default=str)
    
    # Save text report
    with open(txt_file, 'w', encoding='utf-8') as f:
        f.write("MPU RX PERFORMANCE DIAGNOSTIC REPORT\n")
        f.write("=" * 50 + "\n\n")
        f.write(f"Timestamp: {report.timestamp}\n")
        f.write(f"Configuration: {json.dumps(report.config, indent=2)}\n\n")
        
        f.write("EXECUTIVE SUMMARY\n")
        f.write("-" * 20 + "\n")
        if report.analysis.get("recommendations"):
            for i, rec in enumerate(report.analysis["recommendations"], 1):
                f.write(f"{i}. {rec}\n")
        f.write("\n")
        
        f.write("PHASE RESULTS\n")
        f.write("-" * 20 + "\n")
        for phase_name, phase_data in report.phases.items():
            f.write(f"\n{phase_name.upper()}:\n")
            if isinstance(phase_data.get("results"), list):
                f.write(f"  Commands executed: {len(phase_data['results'])}\n")
                f.write(f"  Duration: {phase_data.get('start_time', 'unknown')} - {phase_data.get('end_time', 'unknown')}\n")
        
        f.write(f"\nOptimization Tests: {len(report.optimization_results.get('tests', []))}\n")
        f.write("\nFull details available in JSON report.\n")
    
    return {"json": json_file, "txt": txt_file}


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(
        description="MPU RX Performance Diagnostic Test Suite - Automated analysis of 85% packet loss issue"
    )
    parser.add_argument("--mcu-port", default="COM8", help="MCU serial port (default: COM8)")
    parser.add_argument("--mpu-port", default="COM9", help="MPU serial port (default: COM9)")
    parser.add_argument("--baudrate", type=int, default=115200, help="Serial baudrate (default: 115200)")
    parser.add_argument("--mcu-ip", default="192.168.0.200", help="MCU IP address (default: 192.168.0.200)")
    parser.add_argument("--mpu-ip", default="192.168.0.5", help="MPU IP address (default: 192.168.0.5)")
    parser.add_argument("--test-duration", type=int, default=30, help="iperf test duration in seconds (default: 30)")
    parser.add_argument("--skip-phases", help="Comma-separated list of phases to skip (1-5)")
    parser.add_argument("--output-dir", default=".", help="Output directory for reports (default: current)")
    
    args = parser.parse_args()
    
    # Parse skip phases
    if args.skip_phases:
        try:
            args.skip_phases = {int(x.strip()) for x in args.skip_phases.split(",") if x.strip().isdigit()}
        except ValueError:
            args.skip_phases = set()
    else:
        args.skip_phases = set()
    
    return args


def main() -> int:
    """Main entry point."""
    args = parse_args()
    
    try:
        # Initialize diagnostic runner
        runner = MPURXDiagnosticRunner(
            mcu_port=args.mcu_port,
            mpu_port=args.mpu_port,
            baudrate=args.baudrate,
            mcu_ip=args.mcu_ip,
            mpu_ip=args.mpu_ip,
            test_duration=args.test_duration,
            skip_phases=args.skip_phases
        )
        
        # Run diagnostic
        report = runner.run_diagnostic()
        
        # Save report
        output_paths = save_diagnostic_report(report, Path(args.output_dir))
        
        print(f"\n📁 Report files saved:")
        print(f"   JSON: {output_paths['json']}")
        print(f"   TXT:  {output_paths['txt']}")
        
        return 0
        
    except serial.SerialException as e:
        print(f"❌ Serial connection error: {e}")
        return 2
    except Exception as e:
        print(f"❌ Diagnostic failed: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())