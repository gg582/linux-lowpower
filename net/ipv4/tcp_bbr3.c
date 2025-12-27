/* BBR (Bottleneck Bandwidth and RTT) congestion control
 *
 * BBR is a model-based congestion control algorithm that aims for low queues,
 * low loss, and (bounded) Reno/CUBIC coexistence. To maintain a model of the
 * network path, it uses measurements of bandwidth and RTT, as well as (if they
 * occur) packet loss and/or shallow-threshold ECN signals. Note that although
 * it can use ECN or loss signals explicitly, it does not require either; it
 * can bound its in-flight data based on its estimate of the BDP.
 *
 * The model has both higher and lower bounds for the operating range:
 * lo: bw_lo, inflight_lo: conservative short-term lower bound
 * hi: bw_hi, inflight_hi: robust long-term upper bound
 * The bandwidth-probing time scale is (a) extended dynamically based on
 * estimated BDP to improve coexistence with Reno/CUBIC; (b) bounded by
 * an interactive wall-clock time-scale to be more scalable and responsive
 * than Reno and CUBIC.
 *
 * Here is a state transition diagram for BBR:
 *
 * |
 * V
 * +---> STARTUP  ----+
 * |        |         |
 * |        V         |
 * |      DRAIN   ----+
 * |        |         |
 * |        V         |
 * +---> PROBE_BW ----+
 * |      ^    |      |
 * |      |    |      |
 * |      +----+      |
 * |                  |
 * +---- PROBE_RTT <--+
 *
 * A BBR flow starts in STARTUP, and ramps up its sending rate quickly.
 * When it estimates the pipe is full, it enters DRAIN to drain the queue.
 * In steady state a BBR flow only uses PROBE_BW and PROBE_RTT.
 * A long-lived BBR flow spends the vast majority of its time remaining
 * (repeatedly) in PROBE_BW, fully probing and utilizing the pipe's bandwidth
 * in a fair manner, with a small, bounded queue. *If* a flow has been
 * continuously sending for the entire min_rtt window, and hasn't seen an RTT
 * sample that matches or decreases its min_rtt estimate for 10 seconds, then
 * it briefly enters PROBE_RTT to cut inflight to a minimum value to re-probe
 * the path's two-way propagation delay (min_rtt). When exiting PROBE_RTT, if
 * we estimated that we reached the full bw of the pipe then we enter PROBE_BW;
 * otherwise we enter STARTUP to try to fill the pipe.
 *
 * BBR is described in detail in:
 * "BBR: Congestion-Based Congestion Control",
 * Neal Cardwell, Yuchung Cheng, C. Stephen Gunn, Soheil Hassas Yeganeh,
 * Van Jacobson. ACM Queue, Vol. 14 No. 5, September-October 2016.
 *
 * Custom TWEAK:
 * - Pacing reduction on bandwidth drop (BW_DELTA_ALPHA/CEILING/FLOOR).
 * - Immediate CWND reduction when pacing drops significantly.
 */

#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/module.h>
#include <net/tcp.h>
#include <linux/inet_diag.h>
#include <linux/inet.h>
#include <linux/random.h>
#include <linux/win_minmax.h>
#include <trace/events/tcp.h>
#include "tcp_dctcp.h"

#ifndef TCP_ECN_OK
#define TCP_ECN_OK 1
#endif
#ifndef TCP_ECN_LOW
#define TCP_ECN_LOW 2
#endif
#ifndef TCP_CONG_WANTS_CE_EVENTS
#define TCP_CONG_WANTS_CE_EVENTS 0
#endif

#define BBR_VERSION       3

#define BW_DELTA_ALPHA        (BBR_UNIT / 2)
#define BW_DELTA_CEILING      (BBR_UNIT / 4)
#define BW_DELTA_FLOOR        (BBR_UNIT * 3 / 4)

#define BW_SCALE 24
#define BW_UNIT (1 << BW_SCALE)
#define BBR_SCALE 8
#define BBR_UNIT (1 << BBR_SCALE)

enum bbr_mode {
	BBR_STARTUP,
	BBR_DRAIN,
	BBR_PROBE_BW,
	BBR_PROBE_RTT,
};

struct bbr {
	u32	min_rtt_us;
	u32	min_rtt_stamp;
	u32	probe_rtt_done_stamp;
	u32	probe_rtt_min_us;
	u32	probe_rtt_min_stamp;
	u32 	next_rtt_delivered;
	u64	cycle_mstamp;
	u32 	mode:2,
	prev_ca_state:3,
	round_start:1,
	ce_state:1,
	bw_probe_up_rounds:5,
	try_fast_path:1,
	idle_restart:1,
	probe_rtt_round_done:1,
	init_cwnd:7,
	unused_1:10;
	u32	pacing_gain:10,
	cwnd_gain:10,
	full_bw_reached:1,
	full_bw_cnt:2,
	cycle_idx:2,
	has_seen_rtt:1,
	reduce_cwnd:1,
	unused_2:5;
	u32	prior_cwnd;
	u32	full_bw;
	u64	ack_epoch_mstamp;
	u16	extra_acked[2];
	u32	ack_epoch_acked:20,
	extra_acked_win_rtts:5,
	extra_acked_win_idx:1,
	full_bw_now:1,
	startup_ecn_rounds:2,
	loss_in_cycle:1,
	ecn_in_cycle:1,
	unused_3:1;
	u32	loss_round_delivered;
	u32	undo_bw_lo, undo_inflight_lo, undo_inflight_hi;
	u32	bw_latest, bw_lo, bw_hi[2];
	u32	inflight_latest, inflight_lo, inflight_hi;
	u32	bw_probe_up_cnt, bw_probe_up_acks, probe_wait_us, prior_rcv_nxt;
	u32	ecn_eligible:1,
	ecn_alpha:9,
	bw_probe_samples:1,
	prev_probe_too_high:1,
	stopped_risky_probe:1,
	rounds_since_probe:8,
	loss_round_start:1,
	loss_in_round:1,
	ecn_in_round:1,
	ack_phase:3,
	loss_events_in_round:4,
	initialized:1;
	u32	alpha_last_delivered, alpha_last_delivered_ce;
	u32	pacing_gain_extra;
};

struct bbr_context { u32 sample_bw; };

static const u32 bbr_min_rtt_win_sec = 10;
static const u32 bbr_probe_rtt_mode_ms = 200;
static const int bbr_pacing_margin_percent = 1;
static const int bbr_startup_pacing_gain = BBR_UNIT * 277 / 100 + 1;
static const int bbr_startup_cwnd_gain = BBR_UNIT * 2;
static const int bbr_drain_gain = BBR_UNIT * 1000 / 2885;
static const int bbr_cwnd_gain  = BBR_UNIT * 2;
static const int bbr_pacing_gain_cycle[] = { BBR_UNIT * 5 / 4, BBR_UNIT * 91 / 100, BBR_UNIT, BBR_UNIT };
static const u32 bbr_cwnd_min_target = 4;
static const int bbr_extra_acked_gain = BBR_UNIT;
static const u32 bbr_extra_acked_max_us = 100 * 1000;

static u32 bbr_max_bw(const struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);
	return max(bbr->bw_hi[0], bbr->bw_hi[1]);
}

static u32 bbr_bw(const struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);
	return min(bbr_max_bw(sk), bbr->bw_lo);
}

static bool bbr_can_use_ecn(const struct sock *sk)
{
	return (tcp_sk(sk)->ecn_flags & TCP_ECN_OK) && (tcp_sk(sk)->ecn_flags & TCP_ECN_LOW);
}

static void bbr_exit_probe_rtt(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);
	bbr->mode = bbr->full_bw_reached ? BBR_PROBE_BW : BBR_STARTUP;
}

static void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	if (!(bbr->probe_rtt_done_stamp && after(tcp_jiffies32, bbr->probe_rtt_done_stamp)))
		return;
	bbr->probe_rtt_min_stamp = tcp_jiffies32;
	tcp_snd_cwnd_set(tp, max(tcp_snd_cwnd(tp), bbr->prior_cwnd));
	bbr_exit_probe_rtt(sk);
}

static void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	if (bbr->prev_ca_state < TCP_CA_Recovery && bbr->mode != BBR_PROBE_RTT)
		bbr->prior_cwnd = tcp_snd_cwnd(tp);
	else
		bbr->prior_cwnd = max(bbr->prior_cwnd, tcp_snd_cwnd(tp));
}

static void bbr_tweak_pacing_reduction(struct sock *sk, u32 sample_bw, u32 old_bw)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 max_bw_val = bbr_max_bw(sk);

	if (!max_bw_val || sample_bw >= old_bw) {
		bbr->pacing_gain_extra = BBR_UNIT;
		return;
	}

	u64 delta = ((u64)(old_bw - sample_bw) * BBR_UNIT) / max_bw_val;
	u64 reduction = (delta * BW_DELTA_ALPHA) >> BBR_SCALE;

	if (reduction > BW_DELTA_CEILING)
		reduction = BW_DELTA_CEILING;

	bbr->pacing_gain_extra = BBR_UNIT - (u32)reduction;
	bbr->reduce_cwnd = 0;

	if (bbr->pacing_gain_extra < BW_DELTA_FLOOR) {
		bbr->reduce_cwnd = 1;
		if (bbr->pacing_gain_extra < BBR_UNIT / 8)
			bbr->pacing_gain_extra = BBR_UNIT / 8;
	}
}

/* Adjusted bbr_tso_segs to match u32 (*min_tso_segs)(struct sock *sk) interface */
__bpf_kfunc static u32 bbr_tso_segs(struct sock *sk)
{
  struct tcp_sock *tp = tcp_sk(sk);
	u32 mss_now = tp->mss_cache;
	u32 bytes = READ_ONCE(sk->sk_pacing_rate) >> READ_ONCE(sk->sk_pacing_shift);

	return max_t(u32, bytes / mss_now, sock_net(sk)->ipv4.sysctl_tcp_min_tso_segs);
}

__bpf_kfunc static void bbr_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	if (event == CA_EVENT_TX_START) {
		if (!tp->app_limited) return;
		bbr->idle_restart = 1;
		if (bbr->mode == BBR_PROBE_BW) {
			u32 bw = bbr_bw(sk);
			u64 rate = (u64)bw * tp->mss_cache * BBR_UNIT;
			rate >>= BBR_SCALE;
			rate *= USEC_PER_SEC / 100 * (100 - bbr_pacing_margin_percent);
			WRITE_ONCE(sk->sk_pacing_rate, rate >> BW_SCALE);
		} else if (bbr->mode == BBR_PROBE_RTT) {
			bbr_check_probe_rtt_done(sk);
		}
	} else if ((event == CA_EVENT_ECN_IS_CE || event == CA_EVENT_ECN_NO_CE) && bbr_can_use_ecn(sk)) {
		u32 state = bbr->ce_state;
		dctcp_ece_ack_update(sk, event, &bbr->prior_rcv_nxt, &state);
		bbr->ce_state = state;
	}
}

static u32 bbr_bdp(struct sock *sk, u32 bw, int gain)
{
	struct bbr *bbr = inet_csk_ca(sk);
	if (unlikely(bbr->min_rtt_us == ~0U))
		return bbr->init_cwnd;
	u64 w = (u64)bw * bbr->min_rtt_us;
	return (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;
}

static void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs, u32 acked, u32 bw, int gain, u32 cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u32 target, extra_ack = 0;

	if (!acked) goto done;
	if (bbr->reduce_cwnd) {
		cwnd = max_t(s32, cwnd - acked, 1);
		bbr->reduce_cwnd = 0;
	}

	if (bbr_extra_acked_gain) {
		extra_ack = (u64)(bbr_extra_acked_gain * max(bbr->extra_acked[0], bbr->extra_acked[1])) >> BBR_SCALE;
		extra_ack = min(extra_ack, (u32)(((u64)bbr_bw(sk) * bbr_extra_acked_max_us) / BW_UNIT));
	}

	target = bbr_bdp(sk, bw, gain) + extra_ack;

	/* Internal budget calculation */
	u32 mss = tp->mss_cache;
	u32 bytes = READ_ONCE(sk->sk_pacing_rate) >> READ_ONCE(sk->sk_pacing_shift);
	u32 tso = 3 * max_t(u32, bytes / mss, sock_net(sk)->ipv4.sysctl_tcp_min_tso_segs);

	target = max_t(u32, max_t(u32, target, tso), bbr_cwnd_min_target);

	if (bbr->full_bw_reached) {
		cwnd += acked;
		if (cwnd >= target) cwnd = target;
	} else if (cwnd < target || cwnd < 2 * bbr->init_cwnd) {
		cwnd += acked;
	}
	cwnd = max_t(u32, cwnd, bbr_cwnd_min_target);
	done:
	tcp_snd_cwnd_set(tp, min(cwnd, tp->snd_cwnd_clamp));
}

__bpf_kfunc static void bbr_main(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	struct bbr_context ctx = { .sample_bw = 0 };
	u32 old_bw = bbr_bw(sk);

	bbr->round_start = 0;
	if (rs->interval_us > 0 && !before(rs->prior_delivered, bbr->next_rtt_delivered)) {
		bbr->next_rtt_delivered = tp->delivered;
		bbr->round_start = 1;
	}

	if (rs->interval_us > 0)
		ctx.sample_bw = DIV_ROUND_UP_ULL((u64)rs->delivered * BW_UNIT, rs->interval_us);

	if (rs->interval_us > 0 && rs->acked_sacked > 0) {
		if (!rs->is_app_limited || ctx.sample_bw >= bbr_max_bw(sk))
			bbr->bw_hi[1] = max(bbr->bw_hi[1], ctx.sample_bw);

		bbr_tweak_pacing_reduction(sk, ctx.sample_bw, old_bw);

		if (bbr->round_start) {
			bbr->bw_hi[0] = bbr->bw_hi[1];
			bbr->bw_hi[1] = 0;
		}
	}

	switch (bbr->mode) {
		case BBR_STARTUP: bbr->pacing_gain = bbr_startup_pacing_gain; bbr->cwnd_gain = bbr_startup_cwnd_gain; break;
		case BBR_DRAIN: bbr->pacing_gain = bbr_drain_gain; bbr->cwnd_gain = bbr_startup_cwnd_gain; break;
		case BBR_PROBE_BW: bbr->pacing_gain = bbr_pacing_gain_cycle[bbr->cycle_idx]; bbr->cwnd_gain = bbr_cwnd_gain; break;
		case BBR_PROBE_RTT: bbr->pacing_gain = BBR_UNIT; bbr->cwnd_gain = BBR_UNIT; break;
	}

	u32 bw_val = bbr_bw(sk);
	u32 final_pacing_gain = (u64)bbr->pacing_gain * bbr->pacing_gain_extra >> BBR_SCALE;
	u64 rate = (u64)bw_val * tp->mss_cache * final_pacing_gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - bbr_pacing_margin_percent);

	if (bbr->full_bw_reached || (rate >> BW_SCALE) > READ_ONCE(sk->sk_pacing_rate))
		WRITE_ONCE(sk->sk_pacing_rate, rate >> BW_SCALE);

	bbr_set_cwnd(sk, rs, rs->acked_sacked, bw_val, bbr->cwnd_gain, tcp_snd_cwnd(tp));
}

__bpf_kfunc static void bbr_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	bbr->initialized = 1;
	bbr->init_cwnd = min(0x7FU, tcp_snd_cwnd(tp));
	bbr->min_rtt_us = tcp_min_rtt(tp);
	bbr->min_rtt_stamp = tcp_jiffies32;
	bbr->pacing_gain_extra = BBR_UNIT;
	bbr->reduce_cwnd = 0;
	bbr->mode = BBR_STARTUP;
	bbr->bw_lo = ~0U;
	cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

__bpf_kfunc static u32 bpf_bbr_undo_cwnd(struct sock *sk)
{
	return tcp_snd_cwnd(tcp_sk(sk));
}

__bpf_kfunc static u32 bbr_sndbuf_expand(struct sock *sk) { return 3; }
__bpf_kfunc static u32 bbr_ssthresh(struct sock *sk) { bbr_save_cwnd(sk); return tcp_sk(sk)->snd_ssthresh; }

static struct tcp_congestion_ops tcp_bbr3_cong_ops __read_mostly = {
	.flags		= TCP_CONG_NON_RESTRICTED | TCP_CONG_WANTS_CE_EVENTS,
	.name		= "bbr3",
	.owner		= THIS_MODULE,
	.init		= bbr_init,
	.cong_control	= bbr_main,
	.sndbuf_expand	= bbr_sndbuf_expand,
	.undo_cwnd      = bpf_bbr_undo_cwnd,
	.cwnd_event	= bbr_cwnd_event,
	.ssthresh	= bbr_ssthresh,
	.min_tso_segs	= bbr_tso_segs,
};

static int __init bbr3_register(void)
{
	return tcp_register_congestion_control(&tcp_bbr3_cong_ops);
}

static void __exit bbr3_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_bbr3_cong_ops);
}

module_init(bbr3_register);
module_exit(bbr3_unregister);

MODULE_AUTHOR("Van Jacobson <vanj@google.com>");
MODULE_AUTHOR("Neal Cardwell <ncardwell@google.com>");
MODULE_AUTHOR("Yuchung Cheng <ycheng@google.com>");
MODULE_AUTHOR("Soheil Hassas Yeganeh <soheil@google.com>");
MODULE_AUTHOR("Priyaranjan Jha <priyarjha@google.com>");
MODULE_AUTHOR("Yousuk Seung <ysseung@google.com>");
MODULE_AUTHOR("Kevin Yang <yyd@google.com>");
MODULE_AUTHOR("Arjun Roy <arjunroy@google.com>");
MODULE_AUTHOR("David Morley <morleyd@google.com>");

MODULE_AUTHOR("Tweak by Lee Yunjin <gzblues61@daum.net>");
MODULE_LICENSE("Dual BSD/GPL");
