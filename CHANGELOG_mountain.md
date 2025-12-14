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
