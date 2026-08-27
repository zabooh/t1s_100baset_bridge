# iperf test matrix - T1S bridge setup

Full test of every directed node-to-node connection between the four nodes on
the bench: PC, Bridge, Follower A, Follower B.

Generated with `iperf_matrix_test.py` (run it, then hand the resulting log
file to Claude to fill in this table - see the script's docstring for the
exact trust rule used to pick which side's numbers count).

- **UDP Max**: highest bitrate that still arrived with acceptable loss
  (ascending search, see script for the exact rate steps and threshold),
  reported as measured throughput + loss %.
- **TCP**: single-run throughput (TCP self-tunes its own rate).

| Direction | UDP Max | TCP |
|---|---|---|
| PC -> Bridge | 70.94 Mbit/s, 0% loss | 22.80 Mbit/s |
| PC -> Follower A | 7.92 Mbit/s, 1% loss | 5.44 Mbit/s |
| PC -> Follower B | 8.00 Mbit/s, 0% loss | 5.38 Mbit/s |
| | | |
| Bridge -> PC | 69.70 Mbit/s, 0% loss | 11.60 Mbit/s |
| Bridge -> Follower A | 9.43 Mbit/s, 0% loss | 5.85 Mbit/s |
| Bridge -> Follower B | 9.43 Mbit/s, 0% loss | 5.85 Mbit/s |
| | | |
| Follower A -> PC | 9.42 Mbit/s, 0% loss | 3.88 Mbit/s |
| Follower A -> Bridge | 9.41 Mbit/s, 0% loss | 5.83 Mbit/s |
| Follower A -> Follower B | 9.43 Mbit/s, 0% loss | 5.83 Mbit/s |
| | | |
| Follower B -> PC | 9.44 Mbit/s, 0% loss | 3.79 Mbit/s |
| Follower B -> Bridge | 9.42 Mbit/s, 0% loss | 5.83 Mbit/s |
| Follower B -> Follower A | 9.43 Mbit/s, 0% loss | 5.84 Mbit/s |

## Setup at time of test

- PC: `192.168.0.100`
- Bridge: `192.168.0.210` (eth1/100BASE-T side)
- Follower A: `192.168.0.202` (COM10)
- Follower B: `192.168.0.201` (COM23)

## Notes / anomalies

- Source log: `iperf_matrix_results.log`, run started 2026-08-28T00:48:25 (the
  third run in that file - the first two ran into script bugs that have since
  been fixed: missing `iperfi` interface pinning on the Bridge as a source,
  and stuck iperf sessions from earlier failed attempts blocking the next
  test with "All instances busy"). This third run completed cleanly end to
  end with no failed tests.
- **Resolved anomaly - `PC -> Follower A/B` UDP originally capped at only
  2 Mbit/s** (loss climbing to 3-4% already at the 5 Mbit/s step), while every
  other direction crossing the same T1S segment (`Bridge <-> Follower`,
  `Follower <-> Follower`, `Follower -> PC`) reached ~9.4 Mbit/s cleanly, and
  TCP PC->Follower reached ~5 Mbit/s cleanly on this same path - so the T1S
  link itself was never the bottleneck.
  **Confirmed root cause (packet-level capture):** at a nominal 5 Mbit/s
  target, the real Windows `iperf.exe` client did not send evenly spaced
  packets (~2.4 ms apart, as the rate would suggest) - it sent bursts of 4-7
  packets within under a millisecond, then an uneven pause, repeatedly
  (`frame.time_relative`, ms: `0.00/0.36/0.54/0.68/0.72/0.96`, then a gap to
  `4.99`, then another 4-packet burst within 90 us at `24.67-24.76`, etc.).
  The long-run average matched the requested rate, but the instantaneous
  rate spiked far above it - a known iperf 1.x/2.x-on-Windows limitation:
  Windows' coarse default timer/sleep resolution (~15.6 ms) makes iperf's
  internal pacing loop fall behind and catch up in bursts. A single such run
  showed the server-side reality plainly: `75/1277 (5.9%)` lost at a 5 Mbit/s
  target. TCP was unaffected because it self-paces via ACK feedback instead
  of a sleep-timer loop, and the embedded (Bridge/Follower) iperf clients
  apparently pace evenly enough not to trigger this. Not a bridge/firmware
  bug - a PC-tooling limitation.
  **Fix:** `iperf_matrix_test.py` now calls `winmm.timeBeginPeriod(1)` for
  its whole run, forcing 1 ms Windows system timer resolution (applies
  machine-wide while held, so it smooths the `iperf.exe` subprocess too).
  Retested with the same setup: packets now land evenly ~2-3 ms apart, 0%
  loss at 5 Mbit/s, and the UDP Max search reaches the real ceiling (~8 Mbit/s
  measured, limited by the coarse 8/10 Mbit/s step size rather than a hard
  wall - consistent with the ~9.4 Mbit/s seen elsewhere). The table above
  already reflects the retested numbers.
- All `<direction> -> PC` TCP numbers use `-B 192.168.0.100` for the PC's
  iperf client/server (see FALLSTRICKE.md - this PC has more than one NIC in
  the 192.168.0.0/24 range and picks the wrong one without it).
- UDP Max search: ascending steps 1/2/5/8/10/20/50/80 Mbit/s, 3 s each, stops
  at the first step with >2% loss and reports the last clean step.
