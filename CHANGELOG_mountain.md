# Mountain Kernel: Network tweaked kernel for you.

Mountain Kernel is a developer-friendly, lightly modified kernel.
It applies minimal yet critical logic refinements to the core network stack. Instead of heavy structural changes, it focuses on practical micro-optimizations in receive scheduling and congestion control to deliver a more consistent, low-jitter experience.

I recommend this kernel to heavy web users, gamers, and VoIP call users.

![logo](./logo/logo.png)

## Is this for me?
If you care about ping spikes in games, or hate it when YouTube buffers under load, Mountain Kernel is built for you. The goal is a “set it and forget it” upgrade: fewer stalls, more consistent responsiveness, and predictable behavior under real-world traffic.

## Overview
- Lower CPU usage for network receive processing (Threaded NAPI; move RX processing from `NET_RX_SOFTIRQ` to per-device kthreads)
- ~96–98% reduced loopback ping spikes vs Vanilla (max RTT reduced to **0.103 ms** in my loopback test)
- ~2x faster average loopback RTT vs Vanilla (~48.9% decreased average delay)

---

# CHANGELOG

## Changes in ECMP Routing (Different from standard Mountain Release)
This is a large change, so I documented a benchmark for this patch
(base commit 559e608 + patch vs linux-kbuild-6.12.57+deb13 generic).

- More consistent response in routing/selection behavior in my tests (lower LAN RTT variance)
  - **Loopback Ping Test (benchmark for the ECMP patch itself)**
    - 228% lower standard deviation
    - Prevented ping spikes (Max **4.65 ms -> 0.059 ms**)
    - Count >= 0.5 ms is unlikely to happen
    - Experiment Link:
      `https://gg582.github.io/tutorials/2025-12-08-%EB%B9%84%EC%A0%84%EA%B3%B5%EC%9E%90%EB%8F%84-%EC%89%BD%EA%B2%8C-%EB%94%B0%EB%9D%BC%ED%95%98%EB%8A%94-%EB%A6%AC%EB%88%85%EC%8A%A4-%EC%BB%A4%EB%84%90-%ED%95%B4%ED%82%B9-%EC%A7%80%ED%84%B0%EB%A5%BC-%EC%9E%A1%EC%95%84%EB%B3%B4%EC%9E%90/` (Korean)

  - *Then why can the standard Mountain release look “slower” on max spike than the ECMP-patched benchmark?*
    - The ECMP-patched benchmark and the standard Mountain release are not the same patch set.
    - In my measurements, Mountain focuses more on improving average latency under typical workloads; the average ping delay can be **~10–15% faster**, while max-spike behavior may vary depending on system/load conditions.

## Changes in `tcp_bbr.c`
- Dynamically adjusts BBR’s pacing behavior by detecting bandwidth fluctuations.
  The primary objective of these changes is to reduce unnecessary packet loss and latency by reacting more quickly to drops in network bandwidth.

### Detailed Changes
- Finalized the logic within the `bbr_update_bw` function to detect bandwidth reductions by comparing the previous bandwidth with the current bandwidth.
- Defined relevant parameters (`BW_DELTA_ALPHA`, `BW_DELTA_CEILING`, `BW_DELTA_FLOOR`) to allow sensitivity adjustment of this feature.
- Activates the `reduce_cwnd` flag when the pacing gain falls below a specific threshold (`BW_DELTA_FLOOR`).
- Modified the `bbr_set_cwnd` function to check this flag; if set, the congestion window is temporarily reduced to alleviate network load.

## `net/core/dev.c`: RX processing (softirq -> kthread)
- Modified `net/core/dev.c` to enable threaded NAPI for all network devices.
- In `register_netdevice()`, called `netif_set_threaded()` to move network device receive processing from `NET_RX_SOFTIRQ` to a dedicated kthread.
- This change aims to reduce CPU spikes by offloading receive processing to a kernel thread, which can improve responsiveness on busy systems and under mixed workloads.

## Dynamic NAPI Budget Adjustment
- Modified `__napi_poll()` in `net/core/dev.c` to dynamically adjust the NAPI budget (`n->weight`).
  - If the last poll consumed the full budget, the budget for the next poll is doubled (up to a max of `NAPI_POLL_WEIGHT * 4`) to handle high traffic more efficiently.
  - The budget is reduced only if the actual work done is less than half of the current budget (weight), preventing aggressive downscaling under moderate load.
- This allows the system to adapt to changing network loads, improving throughput stability and potentially reducing CPU overhead.


---

# BBRv3 Tweaks: Pacing and CWND Response to Bandwidth Drops

## Overview

This work evaluates a **tweaked variant of BBRv3** that adjusts how pacing rate and congestion window (CWND) respond to **observed bandwidth drops**.
The goal is **not to increase peak throughput**, but to **reduce tail latency and jitter under WAN and high-load conditions**, particularly in environments where transient queue buildup is common.

Both the baseline and the modified kernel use **BBRv3**.
The difference lies purely in **parameterization and response timing**, not in algorithm generation.

---

## Motivation

In real WAN environments (e.g., ECMP paths, mixed traffic, CPU contention), we observed:

* Stable average RTT
* Periodic **large RTT spikes (tail latency)**
* Increased RTT variance (jitter)

These effects were consistent with **temporary queue buildup that persists longer than necessary**, even when no packet loss or ECN marks are present.

The tweak aims to make BBRv3 **less tolerant of sustained RTT inflation when delivery rate drops**, encouraging earlier backoff and faster queue drainage.

---

## Summary of Changes

### 1. Pacing Reduction on Bandwidth Drop

When a new bandwidth sample (`sample_bw`) is **lower than the previously estimated bandwidth**, the modified logic:

* Computes a normalized bandwidth delta relative to the estimated maximum bandwidth
* Applies an additional multiplicative factor (`pacing_gain_extra`) to the pacing gain
* Caps the reduction using configurable ceiling/floor values

This causes the sender to **reduce its pacing rate earlier and more smoothly** when bandwidth appears to contract.

---

### 2. Immediate CWND Reduction on Significant Pacing Drop

If the pacing reduction crosses a defined threshold:

* A one-time **immediate CWND reduction** is triggered
* CWND is reduced by the amount of newly ACKed data in the next update cycle

This prevents the sender from continuing to hold excess in-flight data during transient congestion, shortening queue residence time.

---

### 3. Scope of the Change

* No change to:

  * Startup behavior
  * Long-term bandwidth estimation
  * BBR state machine (STARTUP / DRAIN / PROBE_BW / PROBE_RTT)
* No new congestion signals are introduced
* The tweak only affects **how aggressively the sender reacts to bandwidth regression**

---

## Test Methodology (Current Sample)

The current evaluation uses:

* ICMP ping (local and WAN)
* 1000 packets per run
* 0.1s interval
* **100% CPU load during tests**

WAN target: public ICMP endpoint
Local target: same-LAN host

Multiple runs were performed for both baseline and modified kernels.

---

## Observed Results (This Sample)

### WAN (ICMP)

Across repeated runs, the modified kernel consistently showed:

* **Lower maximum RTT**
* **Lower RTT mdev (jitter)**
* Average RTT remained effectively unchanged

This indicates a reduction in **tail latency and variance**, without improving or harming baseline latency.

A single ICMP "loss" (1/1000) was initially observed in some modified runs; however:

* The loss disappears when increasing ping timeout (e.g., `ping -W 5`)
* This strongly suggests a **local scheduling or softirq delay artifact under CPU saturation**, not a true network drop

As such, ICMP loss is not considered a meaningful signal in this setup.
Furthermore, when testing with `-W 5` options, vanilla showed (3/1000) packet loss.
This gently supports what I have explained above.

---

### Local (ICMP)

Local RTT differences were minimal:

* Average RTT unchanged
* Maximum RTT slightly reduced
* RTT mdev slightly increased

Given the very small RTT scale (tens of microseconds), this is attributed to **measurement noise and increased control-loop sensitivity**, and is not considered a functional regression.

---

## Interpretation

The results are consistent with the intended behavior:

* The modified BBRv3 reacts **earlier to bandwidth contraction**
* Excess queue buildup is reduced more quickly
* Tail latency and jitter improve, while steady-state latency remains unchanged

This suggests the tweak primarily affects **queue residence time**, not throughput capacity.

---

## Limitations

* No controlled remote iperf endpoint was available
* WAN evaluation is limited to ICMP latency characteristics
* Results should be interpreted as **directional and environment-specific**

This work does **not** claim universal performance improvement or upstream readiness.

---

## Status

* Suitable for personal or experimental kernels
* Maintained locally
* Intended as a reference implementation and empirical note

Further validation (controlled WAN throughput, mixed congestion control fairness) would be required before proposing upstream changes.

---
