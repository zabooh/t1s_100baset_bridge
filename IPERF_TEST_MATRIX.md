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
| PC -> Follower A | 2.00 Mbit/s, 0% loss | 5.16 Mbit/s |
| PC -> Follower B | 2.00 Mbit/s, 0% loss | 5.04 Mbit/s |
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
- **`PC -> Follower A/B` UDP caps at only 2 Mbit/s (loss climbs to 3-4% at the
  5 Mbit/s step), while every other direction that crosses the same T1S
  segment (`Bridge <-> Follower`, `Follower <-> Follower`, `Follower -> PC`)
  comfortably reaches ~9.4 Mbit/s, close to the 10BASE-T1S physical limit.**
  Only the PC as the UDP *source* is affected - PC as destination reaches the
  same ~9.4 Mbit/s as everything else. Likely explanation: the real PC-side
  `iperf.exe` client paces its sends less evenly (burstier) than the embedded
  iperf clients at the same nominal bitrate, overloading the bridge's T1S-side
  forwarding sooner. Not investigated further - worth a closer look if PC-to-
  Follower UDP throughput matters for real use.
- All `<direction> -> PC` TCP numbers use `-B 192.168.0.100` for the PC's
  iperf client/server (see FALLSTRICKE.md - this PC has more than one NIC in
  the 192.168.0.0/24 range and picks the wrong one without it).
- UDP Max search: ascending steps 1/2/5/8/10/20/50/80 Mbit/s, 3 s each, stops
  at the first step with >2% loss and reports the last clean step.
