/*
 * NDM-TCP: Neural Differential Manifolds for TCP Congestion Control
 * Linux Kernel Module Implementation (Fixed & Optimized)
 * * FIXES:
 * 1. Reduced struct size to fit ICSK_CA_PRIV_SIZE (64 bytes).
 * 2. Fixed XMM clobber error by using proper kernel FPU wrappers.
 * 3. Compacted rtt_history to 8 slots to save space.
 Licnesed GPL V2.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/tcp.h>
#include <linux/inet_diag.h>
#include <net/tcp.h>
#include <linux/slab.h>

#ifdef CONFIG_X86_64
#include <asm/fpu/api.h>
#include <asm/cpufeature.h>
#endif

#define NDM_TCP_VERSION "1.2.1-stable"

/* Constants - Adjusted for 64-byte constraint */
#define ENTROPY_WINDOW_SIZE 8 
#define HIDDEN_SIZE 4
#define INPUT_SIZE 6
#define SCALE_SHIFT 10
#define ENTROPY_THRESHOLD 716 
#define BASE_PLASTICITY 307
#define PLASTICITY_DECAY 1018 
#define MIN_RTT_INIT 0xFFFFFFFF

/* Neural Network Weights */
static const s16 L1_WEIGHTS[HIDDEN_SIZE * INPUT_SIZE] __aligned(32) = {
    -1000, -983, -966, -949, -932, -915,
    -963, -946, -929, -912, -895, -878,
    -926, -909, -892, -875, -858, -841,
    -889, -872, -855, -838, -821, -804
};

#define RECURRENT_WEIGHT 500
static const s16 OUT_WEIGHTS[HIDDEN_SIZE] = { -1000, -987, -974, -961 };

/* Entropy LUT for N=8 */
static const u16 ENTROPY_LUT[9] = { 0, 375, 500, 525, 500, 430, 310, 150, 0 };

static const s16 TANH_LUT[65] = {
    -1018, -1016, -1012, -1005, -993, -973, -941, -894, 
    -826, -736, -626, -502, -373, -249, -135, -34, 
    68, 168, 281, 404, 529, 649, 755, 841, 
    906, 950, 979, 996, 1007, 1013, 1017, 1019, 1020,
    1021, 1022, 1022, 1023, 1023, 1023, 1024, 1024,
    1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024,
    1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024,
    1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024
};

static const u16 SIGMOID_LUT[65] = {
    47, 58, 71, 87, 106, 129, 156, 187, 222, 261, 303, 348, 395, 443, 492, 540, 
    587, 632, 675, 715, 752, 786, 816, 843, 867, 887, 905, 920, 933, 944, 953, 961,
    967, 973, 977, 981, 984, 987, 989, 991, 993, 994, 995, 996, 997, 997, 998, 998,
    999, 999, 999, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000
};

/* * Optimized Structure: Total size 64 bytes 
 * Fits within ICSK_CA_PRIV_SIZE
 */
struct ndm_tcp {
    u32 min_rtt_us;           /* 4 */
    u32 prior_cwnd;           /* 8 */
    u32 ssthresh;             /* 12 */
    u32 cached_cwnd_delta;     /* 16 */
    
    u16 plasticity;           /* 18 */
    u16 shannon_entropy;      /* 20 */
    u16 packets_acked;        /* 22 */
    u8 history_index;         /* 23 */
    u8 history_count;         /* 24 */
    
    u8 flags;                 /* 25 */
    u8 nn_skip_counter;       /* 26 */
    s16 hidden_state[HIDDEN_SIZE]; /* 34 (4*2) */
    
    u16 rtt_history[ENTROPY_WINDOW_SIZE]; /* 50 (8*2) */
    
    u8 padding[14];           /* 64 - TOTAL */
} __aligned(8);

#define FLAG_HAS_DATA   (1 << 0)
#define FLAG_SLOW_START (1 << 1)
#define FLAG_CONGESTION (1 << 2)
#define FLAG_LOSS       (1 << 3)

static inline void set_flag(struct ndm_tcp *ca, u8 flag) { ca->flags |= flag; }
static inline void clear_flag(struct ndm_tcp *ca, u8 flag) { ca->flags &= ~flag; }
static inline bool check_flag(const struct ndm_tcp *ca, u8 flag) { return (ca->flags & flag); }

static inline s16 fast_tanh(s32 x) {
    s32 idx;
    if (unlikely(x <= -3072)) return -1024;
    if (unlikely(x >= 3072)) return 1024;
    idx = (x + 3072) >> 6;
    return TANH_LUT[idx & 63];
}

static inline u32 fast_sigmoid(s32 x) {
    s32 idx;
    if (unlikely(x <= -3072)) return 0;
    if (unlikely(x >= 3072)) return 1024;
    idx = (x + 3072) >> 6;
    return SIGMOID_LUT[idx & 63];
}

static u32 calculate_entropy_fast(struct ndm_tcp *ca) {
    u32 histogram[8] = {0};
    u16 min_val = 0xFFFF, max_val = 0;
    u16 i;
    u32 entropy = 0;
    
    for (i = 0; i < ca->history_count; i++) {
        if (ca->rtt_history[i] < min_val) min_val = ca->rtt_history[i];
        if (ca->rtt_history[i] > max_val) max_val = ca->rtt_history[i];
    }
    
    u16 range = max_val - min_val;
    if (unlikely(range == 0)) return 0;
    
    u32 scale = 458752 / range; /* (7 << 16) / range */
    
    for (i = 0; i < ca->history_count; i++) {
        u32 bin = ((ca->rtt_history[i] - min_val) * scale) >> 16;
        if (bin > 7) bin = 7;
        histogram[bin]++;
    }
    
    for (i = 0; i < 8; i++) {
        if (histogram[i]) entropy += ENTROPY_LUT[histogram[i]];
    }
    return entropy;
}

#ifdef CONFIG_X86_64
static void ndm_forward_pass_avx(struct ndm_tcp *ca, s32 *input_vec, s32 *hidden_accum) {
    s16 inputs_s16[8] __aligned(16);
    int i;

    for(i=0; i<6; i++) inputs_s16[i] = (s16)input_vec[i];
    inputs_s16[6] = 0; inputs_s16[7] = 0;

    kernel_fpu_begin();

    /* * Rewritten to avoid XMM clobber lists which conflict with -mno-sse.
     * We use "v" constraints if possible, or just standard registers 
     * while inside kernel_fpu_begin.
     */
    asm volatile(
        "vmovdqa %1, %%xmm0 \n\t"
        "vmovdqa %2, %%xmm1 \n\t"
        "vpmaddwd %%xmm0, %%xmm1, %%xmm1 \n\t"
        "vphaddd %%xmm1, %%xmm1, %%xmm1 \n\t"
        "vphaddd %%xmm1, %%xmm1, %%xmm1 \n\t"
        "vmovd %%xmm1, %0 \n\t"
        : "=m" (hidden_accum[0])
        : "m" (inputs_s16[0]), "m" (L1_WEIGHTS[0])
    );

    for (i = 1; i < HIDDEN_SIZE; i++) {
        const s16 *w = &L1_WEIGHTS[i * INPUT_SIZE];
        s32 sum = (inputs_s16[0] * w[0]) + (inputs_s16[1] * w[1]) + 
                  (inputs_s16[2] * w[2]) + (inputs_s16[3] * w[3]) + 
                  (inputs_s16[4] * w[4]) + (inputs_s16[5] * w[5]);
        hidden_accum[i] = sum;
    }

    kernel_fpu_end();
}
#endif

static void ndm_forward_pass_opt(struct ndm_tcp *ca, u32 rtt_us, u32 *cwnd_delta) {
    s32 input_vec[INPUT_SIZE];
    s32 hidden_accum[HIDDEN_SIZE];
    int i;
    
    u64 scaled_rtt = (u64)rtt_us << SCALE_SHIFT;
    do_div(scaled_rtt, max(1U, ca->min_rtt_us));
    input_vec[0] = (s32)min(scaled_rtt, (u64)2048) - 1024;
    input_vec[1] = (s32)ca->shannon_entropy;
    input_vec[2] = check_flag(ca, FLAG_SLOW_START) ? 1024 : -1024;
    input_vec[3] = check_flag(ca, FLAG_CONGESTION) ? -1024 : 1024;
    input_vec[4] = (s32)ca->plasticity - 512;
    input_vec[5] = check_flag(ca, FLAG_LOSS) ? -1024 : 1024;
    
#ifdef CONFIG_X86_64
    if (boot_cpu_has(X86_FEATURE_AVX) && irq_fpu_usable()) {
        ndm_forward_pass_avx(ca, input_vec, hidden_accum);
    } else {
#endif
        for (i = 0; i < HIDDEN_SIZE; i++) {
            const s16 *w = &L1_WEIGHTS[i * INPUT_SIZE];
            hidden_accum[i] = (input_vec[0] * w[0]) + (input_vec[1] * w[1]) + 
                              (input_vec[2] * w[2]) + (input_vec[3] * w[3]) + 
                              (input_vec[4] * w[4]) + (input_vec[5] * w[5]);
        }
#ifdef CONFIG_X86_64
    }
#endif

    for (i = 0; i < HIDDEN_SIZE; i++) {
        s32 val = (hidden_accum[i] + ((s32)ca->hidden_state[i] * RECURRENT_WEIGHT)) >> SCALE_SHIFT;
        ca->hidden_state[i] = fast_tanh(val);
    }
    
    s32 output_sum = 0;
    for (i = 0; i < HIDDEN_SIZE; i++) {
        output_sum += (ca->hidden_state[i] * OUT_WEIGHTS[i]);
    }
    output_sum >>= SCALE_SHIFT;
    
    *cwnd_delta = (ca->shannon_entropy > ENTROPY_THRESHOLD) ? 
                   fast_sigmoid(output_sum >> 1) : fast_sigmoid(output_sum);
}

static void ndm_tcp_init(struct sock *sk) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct ndm_tcp *ca = inet_csk_ca(sk);
    
    memset(ca, 0, sizeof(struct ndm_tcp));
    ca->min_rtt_us = MIN_RTT_INIT;
    ca->ssthresh = tp->snd_ssthresh;
    ca->prior_cwnd = tp->snd_cwnd;
    ca->plasticity = BASE_PLASTICITY;
    ca->flags = FLAG_SLOW_START;
    ca->cached_cwnd_delta = 512;
    
    tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
}

static void ndm_tcp_cong_avoid(struct sock *sk, u32 ack, u32 acked) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct ndm_tcp *ca = inet_csk_ca(sk);
    u32 rtt_us, cwnd_delta;
    
    if (unlikely(!acked)) return;
    
    ca->packets_acked += acked;
    rtt_us = max(tp->srtt_us >> 3, 1U);
    if (rtt_us < ca->min_rtt_us) ca->min_rtt_us = rtt_us;
    
    ca->rtt_history[ca->history_index] = (u16)min(rtt_us / 1000, 65535U);
    ca->history_index = (ca->history_index + 1) % ENTROPY_WINDOW_SIZE;
    if (ca->history_count < ENTROPY_WINDOW_SIZE) ca->history_count++;

    if (unlikely(ca->packets_acked >= 16)) {
        ca->shannon_entropy = (u16)calculate_entropy_fast(ca);
        ca->packets_acked = 0;
        set_flag(ca, FLAG_HAS_DATA);
        if (ca->shannon_entropy < ENTROPY_THRESHOLD) set_flag(ca, FLAG_CONGESTION);
        else clear_flag(ca, FLAG_CONGESTION);
        clear_flag(ca, FLAG_LOSS);
    }
    
    if (tp->snd_cwnd < ca->ssthresh) set_flag(ca, FLAG_SLOW_START);
    else clear_flag(ca, FLAG_SLOW_START);
        
    if (ca->shannon_entropy < 500 && ca->plasticity > 800 && ca->nn_skip_counter < 8) {
        cwnd_delta = ca->cached_cwnd_delta;
        ca->nn_skip_counter++;
    } else {
        ndm_forward_pass_opt(ca, rtt_us, &cwnd_delta);
        ca->cached_cwnd_delta = cwnd_delta;
        ca->nn_skip_counter = 0;
    }
    
    if (check_flag(ca, FLAG_SLOW_START)) {
        tcp_slow_start(tp, check_flag(ca, FLAG_CONGESTION) ? (acked >> 1) : acked);
    } else if (check_flag(ca, FLAG_HAS_DATA)) {
        u32 shift = check_flag(ca, FLAG_CONGESTION) ? 11 : 10;
        tcp_cong_avoid_ai(tp, tp->snd_cwnd, max(1U, (acked * cwnd_delta) >> shift));
    } else {
        tcp_reno_cong_avoid(sk, ack, acked);
    }
    
    ca->plasticity = (u16)(((u32)ca->plasticity * PLASTICITY_DECAY) >> 10);
    if (ca->plasticity < 100) ca->plasticity = 100;
}

static u32 ndm_tcp_ssthresh(struct sock *sk) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct ndm_tcp *ca = inet_csk_ca(sk);
    set_flag(ca, FLAG_LOSS | FLAG_CONGESTION);
    ca->plasticity = min(1024U, ca->plasticity + 100U);
    ca->ssthresh = max(tp->snd_cwnd / ((ca->shannon_entropy > ENTROPY_THRESHOLD) ? 3 : 2), 2U);
    ca->prior_cwnd = tp->snd_cwnd;
    return ca->ssthresh;
}

static u32 ndm_tcp_undo_cwnd(struct sock *sk) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct ndm_tcp *ca = inet_csk_ca(sk);
    tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
    if (tp->snd_cwnd < ca->ssthresh) set_flag(ca, FLAG_SLOW_START);
    return tp->snd_cwnd;
}

static void ndm_tcp_cwnd_event(struct sock *sk, enum tcp_ca_event ev) {
    struct ndm_tcp *ca = inet_csk_ca(sk);
    if (ev == CA_EVENT_LOSS) set_flag(ca, FLAG_CONGESTION | FLAG_LOSS);
    else if (ev == CA_EVENT_CWND_RESTART) ca->plasticity = BASE_PLASTICITY;
}

static size_t ndm_tcp_get_info(struct sock *sk, u32 ext, int *attr, union tcp_cc_info *info) {
    const struct ndm_tcp *ca = inet_csk_ca(sk);
    if (ext & (1 << (INET_DIAG_VEGASINFO - 1))) {
        info->vegas.tcpv_enabled = 1;
        info->vegas.tcpv_rttcnt = ca->history_count;
        info->vegas.tcpv_rtt = ca->min_rtt_us / 1000;
        info->vegas.tcpv_minrtt = ca->shannon_entropy;
        *attr = INET_DIAG_VEGASINFO;
        return sizeof(struct tcpvegas_info);
    }
    return 0;
}

static void ndm_tcp_set_state(struct sock *sk, u8 new_state) {
    struct ndm_tcp *ca = inet_csk_ca(sk);
    if (new_state == TCP_CA_Loss) {
        set_flag(ca, FLAG_CONGESTION | FLAG_LOSS);
        ca->plasticity = min(1024, ca->plasticity + 150);
    }
}

static struct tcp_congestion_ops ndm_tcp_ops __read_mostly = {
    .init       = ndm_tcp_init,
    .ssthresh   = ndm_tcp_ssthresh,
    .cong_avoid = ndm_tcp_cong_avoid,
    .undo_cwnd  = ndm_tcp_undo_cwnd,
    .cwnd_event = ndm_tcp_cwnd_event,
    .get_info   = ndm_tcp_get_info,
    .set_state  = ndm_tcp_set_state,
    .owner      = THIS_MODULE,
    .name       = "ndm_tcp",
};

static int __init ndm_tcp_register(void) {
    /* Critical check for private storage size */
    BUILD_BUG_ON(sizeof(struct ndm_tcp) > ICSK_CA_PRIV_SIZE);
    pr_info("NDM-TCP v%s: Registered\n", NDM_TCP_VERSION);
    return tcp_register_congestion_control(&ndm_tcp_ops);
}

static void __exit ndm_tcp_unregister(void) {
    tcp_unregister_congestion_control(&ndm_tcp_ops);
}

module_init(ndm_tcp_register);
module_exit(ndm_tcp_unregister);

MODULE_AUTHOR("NDM-TCP Development Team");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Neural Differential Manifolds TCP CC (AVX Optimized)");

MODULE_VERSION(NDM_TCP_VERSION);
