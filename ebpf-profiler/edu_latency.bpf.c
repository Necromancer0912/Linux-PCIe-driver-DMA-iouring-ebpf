// SPDX-License-Identifier: GPL-2.0
/*
 * edu_latency.bpf.c — BPF kernel-side program (CO-RE, libbpf style)
 *
 * This is the kernel half of the latency profiler. It runs inside the
 * Linux kernel using the eBPF VM — sandboxed, verifier-checked, safe.
 *
 * Compiled to BPF bytecode by clang, then loaded by edu_latency.c
 * (the userspace half) via libbpf. The userspace half reads the maps
 * and prints the histograms.
 *
 * Build (inside the guest):
 *   make   (see Makefile in this directory)
 *
 * Architecture:
 *
 *   edu_isr() entry ──kprobe──► record timestamp (per-CPU map)
 *   edu_isr() exit  ──kretprobe► compute delta → update isr_hist[]
 *
 *   edu_ioctl() entry ──kprobe──► record timestamp (per-thread map, cmd==DMA_TEST)
 *   edu_ioctl() exit  ──kretprobe► compute delta → update ioctl_hist[]
 *
 *   __wake_up_common() ──kprobe──► record wakeup timestamp (per-CPU)
 *   finish_wait()      ──kprobe──► compute delta → update wakeup_hist[]
 *
 * CO-RE = "Compile Once, Run Everywhere"
 *   Uses BTF (BPF Type Format) from vmlinux.h to access kernel types
 *   without recompiling for every kernel version.
 *   Generate vmlinux.h with:
 *     bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

/* EDU_IOC_DMA_TEST = _IO('E', 2) = (0 << 30) | ('E' << 8) | 2 = 0x4502 */
#define EDU_IOC_DMA_TEST_NR  0x4502UL

/* Histogram bucket count: 2^0 µs to 2^19 µs (0 to ~500 seconds) */
#define HIST_MAX_SLOTS  20

/* ─── BPF Maps ───────────────────────────────────────────────────────────── */

/* Per-CPU scratch storage for in-progress timing */
struct {
    __uint(type,        BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key,         __u32);
    __type(value,       __u64);
} isr_start_ns SEC(".maps");

struct {
    __uint(type,        BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key,         __u32);   /* tid */
    __type(value,       __u64);
} ioctl_start_ns SEC(".maps");

struct {
    __uint(type,        BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key,         __u32);
    __type(value,       __u64);
} wakeup_start_ns SEC(".maps");

/*
 * Histogram maps: key = log2(latency_us) bucket, value = count.
 * We use a simple array rather than BPF_MAP_TYPE_PERCPU_ARRAY to keep
 * the userspace printing logic simple. For high-frequency production use,
 * switch to PERCPU to avoid spinlock contention.
 */
struct {
    __uint(type,        BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, HIST_MAX_SLOTS);
    __type(key,         __u32);
    __type(value,       __u64);
} isr_hist SEC(".maps");

struct {
    __uint(type,        BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, HIST_MAX_SLOTS);
    __type(key,         __u32);
    __type(value,       __u64);
} ioctl_hist SEC(".maps");

struct {
    __uint(type,        BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, HIST_MAX_SLOTS);
    __type(key,         __u32);
    __type(value,       __u64);
} wakeup_hist SEC(".maps");

/* Total event counters (exported to userspace for context) */
struct {
    __uint(type,        BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 3);   /* [0]=isr, [1]=ioctl, [2]=wakeup */
    __type(key,         __u32);
    __type(value,       __u64);
} counters SEC(".maps");

/* ─── Helpers ─────────────────────────────────────────────────────────────── */

/* log2 floor for histogram bucketing. Returns 0 for val==0. */
static __always_inline __u32 log2_floor(__u64 val)
{
    __u32 n = 0;
    if (val >= (1ULL << 10)) { n += 10; val >>= 10; }
    if (val >= (1ULL <<  5)) { n +=  5; val >>=  5; }
    if (val >= (1ULL <<  2)) { n +=  2; val >>=  2; }
    if (val >= (1ULL <<  1)) { n +=  1; }
    return n;
}

static __always_inline void hist_increment(void *map, __u64 value_us)
{
    __u32 bucket = log2_floor(value_us + 1); /* +1 avoids log2(0) */
    if (bucket >= HIST_MAX_SLOTS)
        bucket = HIST_MAX_SLOTS - 1;
    __u64 *cnt = bpf_map_lookup_elem(map, &bucket);
    if (cnt)
        __sync_fetch_and_add(cnt, 1);
}

static __always_inline void counter_inc(void *map, __u32 idx)
{
    __u64 *c = bpf_map_lookup_elem(map, &idx);
    if (c) __sync_fetch_and_add(c, 1);
}

/* ─── edu_isr kprobe / kretprobe ─────────────────────────────────────────── */

SEC("kprobe/edu_isr")
int BPF_KPROBE(trace_edu_isr_entry)
{
    __u32 key = 0;
    __u64 ts  = bpf_ktime_get_ns();
    bpf_map_update_elem(&isr_start_ns, &key, &ts, BPF_ANY);
    return 0;
}

SEC("kretprobe/edu_isr")
int BPF_KRETPROBE(trace_edu_isr_exit)
{
    __u32 key  = 0;
    __u64 *tsp = bpf_map_lookup_elem(&isr_start_ns, &key);
    if (!tsp)
        return 0;

    __u64 duration_ns = bpf_ktime_get_ns() - *tsp;
    __u64 duration_us = duration_ns / 1000;

    hist_increment(&isr_hist, duration_us);
    counter_inc(&counters, 0);
    return 0;
}

/* ─── edu_ioctl kprobe / kretprobe ──────────────────────────────────────── */

SEC("kprobe/edu_ioctl")
int BPF_KPROBE(trace_edu_ioctl_entry, struct file *file,
               unsigned int cmd, unsigned long arg)
{
    /* Only trace the DMA test ioctl (filter by command number) */
    if (cmd != EDU_IOC_DMA_TEST_NR)
        return 0;

    __u32 tid = bpf_get_current_pid_tgid() & 0xFFFFFFFF;
    __u64 ts  = bpf_ktime_get_ns();
    bpf_map_update_elem(&ioctl_start_ns, &tid, &ts, BPF_ANY);
    return 0;
}

SEC("kretprobe/edu_ioctl")
int BPF_KRETPROBE(trace_edu_ioctl_exit)
{
    __u32 tid  = bpf_get_current_pid_tgid() & 0xFFFFFFFF;
    __u64 *tsp = bpf_map_lookup_elem(&ioctl_start_ns, &tid);
    if (!tsp)
        return 0;

    __u64 duration_us = (bpf_ktime_get_ns() - *tsp) / 1000;
    hist_increment(&ioctl_hist, duration_us);
    counter_inc(&counters, 1);
    bpf_map_delete_elem(&ioctl_start_ns, &tid);
    return 0;
}

/* ─── Interrupt-to-wakeup latency ────────────────────────────────────────── *
 * Measure from wake_up() (called in ISR) to finish_wait() (called when
 * the sleeping thread resumes after being woken).
 */

SEC("kprobe/__wake_up_common")
int BPF_KPROBE(trace_wakeup_entry)
{
    __u32 key = 0;
    __u64 ts  = bpf_ktime_get_ns();
    bpf_map_update_elem(&wakeup_start_ns, &key, &ts, BPF_ANY);
    return 0;
}

SEC("kprobe/finish_wait")
int BPF_KPROBE(trace_finish_wait)
{
    __u32 key  = 0;
    __u64 *tsp = bpf_map_lookup_elem(&wakeup_start_ns, &key);
    if (!tsp)
        return 0;

    __u64 delta_ns = bpf_ktime_get_ns() - *tsp;
    /* Filter: only record if < 1ms (avoids noise from unrelated wake_up calls) */
    if (delta_ns < 1000000) {
        hist_increment(&wakeup_hist, delta_ns / 1000);
        counter_inc(&counters, 2);
    }
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
