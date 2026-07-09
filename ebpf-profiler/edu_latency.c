/* SPDX-License-Identifier: MIT */
/*
 * edu_latency.c — Userspace loader and display for the EDU eBPF profiler
 *
 * Loads edu_latency.bpf.o into the kernel via libbpf, attaches the
 * kprobes, then polls the histogram maps every N seconds and prints
 * a formatted latency histogram to stdout.
 *
 * Build (inside the guest):
 *   make    (see Makefile)
 *
 * Run (as root, with edu.ko loaded):
 *   sudo ./edu_latency [-i <interval_s>] [-n <iterations>]
 *
 * While it's running, trigger the device in another terminal:
 *   sudo ./edu_test && sudo ./edu_uring_test
 *
 * Expected output:
 *   Attaching 7 kprobes... OK
 *   Sampling every 3s. Ctrl-C to stop.
 *
 *   ════════════════════════ Snapshot #1 (t=3s) ════════════════════════
 *
 *   [1] ISR execution time (8 events):
 *         [0µs,  1µs)   ████████████████   5
 *         [1µs,  2µs)   ██████             2
 *         [2µs,  4µs)   ███                1
 *   P50=1µs  P95=2µs  max=3µs
 *
 *   [2] DMA ioctl round-trip (8 events):
 *         [64µs,128µs)  ████████████████   5
 *         [128µs,256µs) ████████           3
 *   P50=100µs  P95=220µs  max=250µs
 *
 *   [3] Interrupt → wakeup latency (8 events):
 *         [4µs,  8µs)   ████████████████   6
 *         [8µs, 16µs)   ████               2
 *   P50=5µs  P95=12µs  max=15µs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>

/* libbpf — install with: apt install libbpf-dev */
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

/* Generated skeleton from edu_latency.bpf.o (make generates this) */
#include "edu_latency.skel.h"

#define HIST_MAX_SLOTS  20
#define BAR_WIDTH       40

/* ANSI colours */
#define CYAN   "\033[36m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define BOLD   "\033[1m"
#define RESET  "\033[0m"

static volatile int keep_running = 1;
static void sig_handler(int sig) { (void)sig; keep_running = 0; }

/* ─── Histogram print ─────────────────────────────────────────────────────── */

/*
 * print_hist - read one histogram map and display it as a bar chart.
 *
 * Buckets are log2 ranges: bucket k covers [2^(k-1), 2^k) µs.
 * We also compute approximate percentiles by cumulative count.
 */
static void print_hist(const char *title, int map_fd, uint64_t total_events)
{
    uint64_t buckets[HIST_MAX_SLOTS] = {0};
    uint64_t max_count = 0, cum = 0;

    /* Read all buckets from BPF map */
    for (uint32_t i = 0; i < HIST_MAX_SLOTS; i++) {
        bpf_map_lookup_elem(map_fd, &i, &buckets[i]);
        if (buckets[i] > max_count)
            max_count = buckets[i];
    }

    printf("\n" BOLD "%s" RESET " (%lu events):\n", title, total_events);

    if (total_events == 0) {
        printf("  (no events recorded — run ./edu_test in another terminal)\n");
        return;
    }

    uint64_t p50 = 0, p95 = 0, p99 = 0, last_val = 0;

    for (uint32_t i = 0; i < HIST_MAX_SLOTS; i++) {
        if (buckets[i] == 0) continue;

        uint64_t lo = (i == 0) ? 0 : (1ULL << (i - 1));
        uint64_t hi = 1ULL << i;

        /* Print bucket label */
        printf("  [%5luµs, %5luµs)  ", lo, hi);

        /* Print bar */
        int bar_len = max_count > 0
                      ? (int)((double)buckets[i] / max_count * BAR_WIDTH)
                      : 0;
        printf(CYAN);
        for (int b = 0; b < bar_len; b++) printf("█");
        printf(RESET);
        for (int b = bar_len; b < BAR_WIDTH; b++) printf(" ");
        printf("  %lu\n", buckets[i]);

        /* Track percentiles */
        cum += buckets[i];
        last_val = (lo + hi) / 2;
        if (!p50 && cum * 2 >= total_events)  p50 = last_val;
        if (!p95 && cum * 20 >= total_events * 19) p95 = last_val;
        if (!p99 && cum * 100 >= total_events * 99) p99 = last_val;
    }

    printf("  " GREEN "P50=%luµs  P95=%luµs  P99=%luµs" RESET "\n",
           p50, p95 ? p95 : last_val, p99 ? p99 : last_val);
}

/* Zero all entries in a histogram map */
static void clear_hist(int map_fd)
{
    uint64_t zero = 0;
    for (uint32_t i = 0; i < HIST_MAX_SLOTS; i++)
        bpf_map_update_elem(map_fd, &i, &zero, BPF_ANY);
}

/* ─── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int interval_s = 3;
    int max_iters  = 0; /* 0 = run forever */
    int opt;

    while ((opt = getopt(argc, argv, "i:n:h")) != -1) {
        switch (opt) {
        case 'i': interval_s = atoi(optarg); break;
        case 'n': max_iters  = atoi(optarg); break;
        case 'h':
            printf("Usage: %s [-i interval_s] [-n iterations]\n", argv[0]);
            printf("  -i  sampling interval in seconds (default: 3)\n");
            printf("  -n  stop after N snapshots (default: run forever)\n");
            return 0;
        }
    }

    /* libbpf verbosity — set to 0 for quiet operation */
    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

    /* Load and verify the BPF object */
    struct edu_latency_bpf *skel = edu_latency_bpf__open_and_load();
    if (!skel) {
        fprintf(stderr, "Failed to load BPF skeleton: %s\n"
                "Make sure edu.ko is loaded and you are running as root.\n"
                "Also check: bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h\n",
                strerror(errno));
        return 1;
    }

    /* Attach all kprobes */
    int err = edu_latency_bpf__attach(skel);
    if (err) {
        fprintf(stderr, "Failed to attach BPF programs: %s\n"
                "If 'edu_isr' not found, ensure edu.ko is insmod'd.\n",
                strerror(-err));
        edu_latency_bpf__destroy(skel);
        return 1;
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf(BOLD "EDU Driver eBPF Latency Profiler" RESET "\n");
    printf("Probing: edu_isr, edu_ioctl, __wake_up_common, finish_wait\n");
    printf("Sampling every %ds. " YELLOW "Ctrl-C" RESET " to stop.\n\n", interval_s);
    printf("Tip: In another terminal, run:  sudo ./edu_test && sudo ./edu_uring_test\n");

    int iter = 0;

    while (keep_running && (max_iters == 0 || iter < max_iters)) {
        sleep(interval_s);
        iter++;

        /* Read counter map */
        uint64_t counters[3] = {0, 0, 0};
        for (uint32_t i = 0; i < 3; i++) {
            bpf_map_lookup_elem(
                bpf_map__fd(skel->maps.counters), &i, &counters[i]);
        }

        printf("\n" BOLD "══════════════════════ Snapshot #%d (t=%ds) ══════════════════════" RESET "\n",
               iter, iter * interval_s);

        print_hist("[1] ISR execution time",
                   bpf_map__fd(skel->maps.isr_hist), counters[0]);

        print_hist("[2] DMA ioctl round-trip latency",
                   bpf_map__fd(skel->maps.ioctl_hist), counters[1]);

        print_hist("[3] Interrupt → thread wakeup latency",
                   bpf_map__fd(skel->maps.wakeup_hist), counters[2]);

        /* Reset for next interval */
        clear_hist(bpf_map__fd(skel->maps.isr_hist));
        clear_hist(bpf_map__fd(skel->maps.ioctl_hist));
        clear_hist(bpf_map__fd(skel->maps.wakeup_hist));
        uint64_t zero = 0;
        for (uint32_t i = 0; i < 3; i++)
            bpf_map_update_elem(bpf_map__fd(skel->maps.counters), &i, &zero, BPF_ANY);
    }

    printf("\n" GREEN "Profiler stopped." RESET "\n");
    edu_latency_bpf__destroy(skel);
    return 0;
}
