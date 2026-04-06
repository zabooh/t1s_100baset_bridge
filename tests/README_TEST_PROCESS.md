# PTP Test Process

## Hardware Setup

| Board | Serial Port | IP Address | Role |
|-------|-------------|------------|------|
| ATSAME54P20A #1 | COM10 | 192.168.0.20 | Grandmaster (GM) |
| ATSAME54P20A #2 | COM8  | 192.168.0.30 | Follower (FOL) |

Both boards connected via 10BASE-T1S (LAN865x). Serial: 115200 baud 8N1.

---

## Regression Test — New Firmware Version

Run after every firmware build to verify basic PTP functionality:

```powershell
cd C:\work\ptp\AN1847\t1s_100baset_bridge\tests
python ptp_test_agent.py --gm-port COM10 --fol-port COM8
```

Expected result: **6/6 PASS**

```
[PASS] Step 0: Reset
[PASS] Step 1: IP Configuration
[PASS] Step 2: Network Connectivity (GM->FOL, FOL->GM)
[PASS] Step 3: PTP Start
[PASS] Step 4: Convergence to FINE state
[PASS] Step 5: Offset Validation
Overall Result: PASS (6/6 tests passed)
```

The log is saved automatically as `ptp_test_YYYYMMDD_HHMMSS.log`.

---

## Known-Good Baseline — Tag v1.0.1

Tag `v1.0.1` marks the firmware + test version that is confirmed passing.

To fall back to the known-good state:

```powershell
git checkout v1.0.1
python tests/ptp_test_agent.py --gm-port COM10 --fol-port COM8
```

This is useful to distinguish whether a failure is caused by a firmware regression
or by a test/environment problem.

---

## On-Off Resilience Test

Investigates Follower behaviour when the Grandmaster stops and restarts:

```powershell
python ptp_onoff_test.py --gm-port COM10 --fol-port COM8
```

Default scenario: GM runs 10 s → stops 5 s → restarts.  
Options:

| Option | Default | Description |
|--------|---------|-------------|
| `--gm-on-time S` | 10 | Seconds GM runs before stop |
| `--gm-off-time S` | 5 | Seconds GM is off (blackout) |
| `--cycles N` | 1 | Number of stop/restart cycles |
| `--samples N` | 10 | Offset samples per phase |
| `--convergence-timeout S` | 30 | Max seconds to wait for FINE |

The test measures:
- **Baseline offset** while GM is running (mean/stdev)
- **Blackout behaviour**: does FOL emit state transitions? Does offset drift?
- **Re-convergence time**: how long does FOL take to reach FINE after GM restart?
- **Post-restart offset**: is accuracy restored within ±100 ns?

### Known Issue (as of v1.0.1)

After a GM restart the Follower detects the sequence-ID mismatch, calls `GM_RESET`,
and enters `MATCHFREQ` — but does **not** re-converge to FINE within 30 s.
Root cause: `PTP_FOL_Init` after `GM_RESET` does not fully reset the frequency
estimator. This is a known firmware bug; once fixed, `ptp_onoff_test.py` will be
promoted to a second regression test.

---

## Test Files

| File | Purpose |
|------|---------|
| `ptp_test_agent.py` | Regression test (Steps 0–5, must pass on every firmware release) |
| `ptp_onoff_test.py` | On-off resilience investigation |

---

## Workflow: Release a New Firmware Version

1. Flash firmware to both boards.
2. Run regression test → must be 6/6 PASS.
3. Commit test results / firmware changes.
4. Set a new tag:

```powershell
git add tests/ptp_test_agent.py
git commit -m "Description of changes"
git tag vX.Y.Z
git push origin <branch>
git push origin vX.Y.Z
```
