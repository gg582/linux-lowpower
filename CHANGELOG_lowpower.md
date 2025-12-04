- Refactored `addr_list_lock` in `struct net_device` to explicitly use spinlocks.
  - Reverted the `addr_list_lock` member in `include/linux/netdevice.h` to `spinlock_t`.
  - Changed its initialization in `net/core/dev.c` (in `register_netdevice()`) to `spin_lock_init()` and added `netdev_set_addr_lockdep_class()`.
  - Updated `netif_addr_lock()` and `netif_addr_lock_bh()` in `include/linux/netdevice.h` to use `spin_lock_nested()` and `spin_lock_bh()` respectively, with `CONFIG_LOCKDEP` handling.
  - Removed incorrect documentation about `addr_list_lock` becoming a mutex from `CHANGELOG_lowpower.md` and `PLANS.md`.

- Restructured `struct dst_entry` layout in `include/net/dst.h`.
  - Reverted internal member rearrangements to match the `origin/master` layout.
  - Moved EMA-related fields (`ema_load`, `ema_time_delta`, `last_update_jiffies`, `ema_k_factor`, `power_cost_weight`) to the very end of `struct dst_entry` to minimize ABI disruption.
  - Confirmed correct placement of `rt_uncached` and related members.

- Refined `update_dst_ems_metrics()` implementation in `net/core/dev.c`.
  - Rewrote the calculation of `rate_change`/`diff` to use `u64` and explicit absolute difference, preventing signed/unsigned mixing issues.

- Improved sysctl registration for `ema_k_factor` in `net/ipv4/sysctl_net_ipv4.c`.
  - Added `minmax` clamping (0-1024) to `lowpower_ema_k_factor` sysctl by changing its handler to `proc_dointvec_minmax` and defining `zero` and `one_zero_two_four` limits.

- Enhanced EMA field concurrency control.
  - Applied `READ_ONCE()` and `WRITE_ONCE()` wrappers to all accesses of EMA fields (`ema_load`, `ema_time_delta`, `last_update_jiffies`, `ema_k_factor`, `power_cost_weight`) in `net/ipv6/route.c`, `net/ipv4/route.c`, `net/ipv4/fib_semantics.c`, `net/core/dev.c`, `net/sctp/outqueue.c`, and `net/xfrm/xfrm_policy.c` to ensure atomic operations and prevent reordering.

- Aligned weight usage philosophy for power-aware traffic consolidation.
  - Modified IPv6 routing (`net/ipv6/route.c`) to prefer busier paths by changing `m -= weight;` to `m += weight;`.
  - Removed SCTP-specific weight usage (`net/sctp/outqueue.c`) for simplification, as directed.

- Ensured consistent EMA initialization across protocols.
  - Modified IPv6 (`net/ipv6/route.c`) and XFRM (`net/xfrm/xfrm_policy.c`) EMA initialization to fetch `ema_k_factor` and `power_cost_weight` from `net->ipv4.sysctl_lowpower_ema_k_factor` and `net->ipv4.sysctl_lowpower_power_cost_weight`, aligning with IPv4's sysctl-driven configuration.

- Reviewed `this_cpu_ptr` and `rcu_dereference` usage in `get_dst_entry_from_fib6_nh` (`net/ipv6/route.c`).
  - Confirmed its usage is appropriate within an RCU read-side critical section, requiring no adjustments.