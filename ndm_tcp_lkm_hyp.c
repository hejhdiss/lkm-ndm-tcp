/*
 * NDM-TCP v3.0: Neural Differential Manifolds (Hyper-Embedded)
 * Optimization Level: HYPER (Zero-Branch, Shift-Add Inference)
 * Plasticity: 1-600 Integer Scaled (0.1 to 6.0)
 * Feature: Hyper-Fast Bit-Stream Entropy Damping
 */

#include <linux/module.h>
#include <linux/tcp.h>
#include <net/tcp.h>

#define NDM_VER "3.0.4-hyper-entropy"
#define MAX_P 600
#define BASE  100
#define ENTROPY_MASK 0xFF /* 8-sample window */

/* Struct compressed for cache alignment (16 bytes) */
struct ndm_tcp {
    u32 min_rtt;       
    u32 last_ack;      
    u16 plasticity;    
    u8  entropy_hist;  /* Bit-history of RTT increases */
    u8  entropy_val;   /* Calculated Shannon approximation (0-64) */
    u16 rtt_var;       
    u16 pad;
};

/* * HYPER ENTROPY: 
 * Calculates jitter/uncertainty without math.h or log functions.
 * Uses population count of RTT direction changes in an 8-bit window.
 */
static __always_inline void update_entropy(struct ndm_tcp *ca, u32 current_rtt) {
    /* Push 1 if RTT increased, 0 if decreased/stable */
    u8 trend = (current_rtt > (ca->rtt_var)) ? 1 : 0;
    ca->entropy_hist = (ca->entropy_hist << 1) | trend;
    ca->rtt_var = (u16)current_rtt;

    /* Entropy = Bit transitions. High transitions = High jitter/Uncertainty.
     * hweight8 (Hamming Weight) is a single CPU instruction on modern x86. */
    ca->entropy_val = hweight8(ca->entropy_hist ^ (ca->entropy_hist >> 1));
}

/* * HYPER INFERENCE: Branchless Manifold Gradient
 * Entropy acts as a 'Noise Floor'—high entropy reduces the growth gradient.
 */
static __always_inline s32 hyper_manifold(struct sock *sk, struct ndm_tcp *ca) {
    struct tcp_sock *tp = tcp_sk(sk);
    u32 rtt = tp->srtt_us >> 3;
    s32 grad;
    
    /* Calculate base growth: (CWND * P) / 100 */
    grad = (s32)((tp->snd_cwnd * ca->plasticity) / BASE);
    
    /* ENTROPY DAMPING: Subtract noise from the growth gradient.
     * High entropy_val (max 8) scales to a penalty. */
    grad -= (s32)(ca->entropy_val << 2); 

    /* Apply RTT penalty only if RTT increased (Branchless) */
    u32 diff = rtt - ca->min_rtt;
    s32 mask = -(s32)(rtt > ca->min_rtt); 
    grad += (mask & -(s32)((diff * ca->plasticity) / BASE));

    return grad;
}

static void ndm_tcp_init(struct sock *sk) {
    struct ndm_tcp *ca = inet_csk_ca(sk);
    memset(ca, 0, sizeof(*ca));
    ca->min_rtt = 0xFFFFFFFF;
    ca->plasticity = 100; 
}

static void ndm_tcp_set_state(struct sock *sk, u8 new_state) {
    struct ndm_tcp *ca = inet_csk_ca(sk);
    if (new_state == TCP_CA_Loss) {
        u32 p = ca->plasticity + 50;
        ca->plasticity = (u16)((p > MAX_P) ? MAX_P : p);
    }
}

static void ndm_tcp_cong_avoid(struct sock *sk, u32 ack, u32 acked) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct ndm_tcp *ca = inet_csk_ca(sk);
    u32 rtt = tp->srtt_us >> 3;

    if (!tcp_is_cwnd_limited(sk)) return;

    /* Update Entropy & Min_RTT */
    update_entropy(ca, rtt);
    if (rtt < ca->min_rtt || ca->min_rtt == 0) ca->min_rtt = rtt;

    if (tp->snd_cwnd <= tp->snd_ssthresh) {
        tcp_slow_start(tp, acked);
    } else {
        s32 g = hyper_manifold(sk, ca);
        if (g > 0) tp->snd_cwnd++;
        else tcp_cong_avoid_ai(tp, tp->snd_cwnd, 1);
    }
}

static u32 ndm_tcp_ssthresh(struct sock *sk) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct ndm_tcp *ca = inet_csk_ca(sk);
    
    if (ca->plasticity > 100)
        ca->plasticity = (ca->plasticity * 3) >> 2;
        
    return max(2U, tp->snd_cwnd >> 1);
}

static u32 ndm_tcp_undo(struct sock *sk) { return tcp_sk(sk)->snd_cwnd; }

static struct tcp_congestion_ops ndm_tcp_ops __read_mostly = {
    .init       = ndm_tcp_init,
    .ssthresh   = ndm_tcp_ssthresh,
    .cong_avoid = ndm_tcp_cong_avoid,
    .set_state  = ndm_tcp_set_state,
    .undo_cwnd  = ndm_tcp_undo,
    .owner      = THIS_MODULE,
    .name       = "ndm_tcp",
};

static int __init ndm_tcp_register(void) {
    BUILD_BUG_ON(sizeof(struct ndm_tcp) > ICSK_CA_PRIV_SIZE);
    return tcp_register_congestion_control(&ndm_tcp_ops);
}

static void __exit ndm_tcp_unregister(void) {
    tcp_unregister_congestion_control(&ndm_tcp_ops);
}

module_init(ndm_tcp_register);
module_exit(ndm_tcp_unregister);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NDM-TCP Hyper: Entropy-Stabilized Manifolds");