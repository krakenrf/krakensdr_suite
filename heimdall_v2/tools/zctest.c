/* zctest: byte-exact sample-loss test for librtlsdr's async path with
 * heimdall's geometry (rtlsdr_read_async(dev, cb, ctx, 32, 32768), one
 * SCHED_RR reader thread per dongle). The RTL2832 runs in counter test mode
 * (rtlsdr_set_testmode): every byte is the previous + 1 (mod 256).
 *
 * Per buffer: length check, counter continuity across the buffer boundary and
 * inside it (1 discontinuity = clean gap, >1 = corrupted data), repeated-byte
 * pairs (the counter never repeats: poison / zero / stale fill shows up here).
 * With -p every buffer is overwritten with a poison byte after the check, so
 * a buffer the next completion did not (fully) refill is caught.
 * A lost whole transfer (32768 = 128 x 256 bytes) is invisible to the 8-bit
 * counter, so arrival timing is checked too: residual r_k = t_k - k*T, its
 * minimum per 1 s window; a step of more than T/2 between consecutive windows
 * = a transfer lost (+T) or delivered twice (-T).
 * -r N restarts every stream every N s (cancel + read_async again), -R also
 * closes and reopens the dongle (heimdall's restart path).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "rtl-sdr.h"

#define MAXDEV 8
#define FS 2400000
#define STARTUP_BUFS 20
#define POISON 0xA5

static double stall_ms = 0; /* -S: stall dev 0's callback once, 5 s in (detector self-test) */
static int buf_num = 32, buf_len = 32768, prio = 20, poison = 0, reopen = 0;
static double duration = 60, restart_s = 0;
static volatile int stop_all = 0;

static double now_s(void) {
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef struct {
	int id; char serial[32]; rtlsdr_dev_t *dev; pthread_t th;
	volatile int running;          /* inside read_async */
	/* per-run state */
	int uninit; uint8_t next; uint64_t run_bufs; double t0;
	double win_start, win_min, prev_win_min; int have_prev;
	/* totals */
	uint64_t bufs, bytes, short_bufs, gaps, gap_bytes, corrupt_bufs, repeat_pairs, poison_pairs;
	uint64_t startup_gaps, startup_gap_bytes, startup_corrupt;
	uint64_t timing_plus, timing_minus; double max_step, min_step;
	double cb_max, cb_sum; uint64_t runs, run_errors, open_errors;
	double thread_cpu;
	int got_policy, got_prio;
	uint8_t *sink; int sink_idx;
	char first_corrupt[256];
} dstate;

static dstate D[MAXDEV];
static int ndev = 0;

static void check_window(dstate *d, double t, double r, int final) {
	if (!final && t - d->win_start < 1.0) { if (r < d->win_min) d->win_min = r; return; }
	if (d->have_prev) {
		double step = d->win_min - d->prev_win_min;
		double T = (double)buf_len / (2.0 * FS);
		if (step > d->max_step) d->max_step = step;
		if (step < d->min_step) d->min_step = step;
		if (step > T / 2) { d->timing_plus++; fprintf(stderr, "dev %s: timing step %+.3f ms at run buf %llu (transfer lost?)\n", d->serial, step * 1e3, (unsigned long long)d->run_bufs); }
		if (step < -T / 2) { d->timing_minus++; fprintf(stderr, "dev %s: timing step %+.3f ms at run buf %llu (duplicate?)\n", d->serial, step * 1e3, (unsigned long long)d->run_bufs); }
	}
	d->prev_win_min = d->win_min; d->have_prev = 1;
	d->win_start = t; d->win_min = r;
}

static void cb(unsigned char *buf, uint32_t len, void *ctx) {
	dstate *d = ctx;
	double t = now_s();
	if (len != (uint32_t)buf_len) d->short_bufs++;
	int startup = d->run_bufs < STARTUP_BUFS;
	if (d->uninit) { d->next = buf[0]; d->uninit = 0; d->t0 = t; d->win_start = t; d->win_min = 0; d->have_prev = 0; }
	uint32_t ndisc = 0, lost = 0, rep = 0, poi = 0, first_at = 0;
	uint8_t exp = d->next;
	for (uint32_t i = 0; i < len; i++) {
		uint8_t b = buf[i];
		if (b != exp) {
			if (!ndisc) first_at = i;
			ndisc++;
			lost += (uint8_t)(b - exp);
		}
		exp = b + 1;
	}
	for (uint32_t i = 0; i + 1 < len; i++) {
		if (buf[i] == buf[i + 1]) { rep++; if (buf[i] == POISON) poi++; }
	}
	d->next = exp;
	if (ndisc == 1) {
		if (startup) { d->startup_gaps++; d->startup_gap_bytes += lost; if (getenv("ZC_SHOW_STARTUP")) fprintf(stderr, "dev %s: startup gap of %u bytes (mod 256) at byte %u of run buf %llu (run %llu)\n", d->serial, lost, first_at, (unsigned long long)d->run_bufs, (unsigned long long)d->runs); }
		else { d->gaps++; d->gap_bytes += lost; fprintf(stderr, "dev %s: gap of %u bytes (mod 256) at byte %u of run buf %llu\n", d->serial, lost, first_at, (unsigned long long)d->run_bufs); }
	} else if (ndisc > 1) {
		if (startup) d->startup_corrupt++;
		else {
			d->corrupt_bufs++;
			if (!d->first_corrupt[0])
				snprintf(d->first_corrupt, sizeof d->first_corrupt, "run buf %llu: %u discontinuities, first at %u, %u repeat pairs (%u poison)",
					(unsigned long long)d->run_bufs, ndisc, first_at, rep, poi);
		}
	}
	d->repeat_pairs += rep; d->poison_pairs += poi;
	/* heimdall-like copy out of the transfer buffer */
	memcpy(d->sink + (size_t)d->sink_idx * buf_len, buf, len);
	d->sink_idx = (d->sink_idx + 1) % 64;
	if (poison) memset(buf, POISON, len);
	/* timing */
	double T = (double)buf_len / (2.0 * FS);
	double r = (t - d->t0) - (double)d->run_bufs * T;
	check_window(d, t, r, 0);
	if (stall_ms > 0 && d->id == 0 && d->run_bufs == 732) usleep((useconds_t)(stall_ms * 1000));
	d->run_bufs++; d->bufs++; d->bytes += len;
	double dt = now_s() - t;
	d->cb_sum += dt; if (dt > d->cb_max) d->cb_max = dt;
}

static int open_dev(dstate *d) {
	int idx = rtlsdr_get_index_by_serial(d->serial);
	if (idx < 0) { fprintf(stderr, "serial %s not found\n", d->serial); return -1; }
	if (rtlsdr_open(&d->dev, idx) < 0) { fprintf(stderr, "open %s failed\n", d->serial); d->dev = NULL; return -1; }
	rtlsdr_set_sample_rate(d->dev, FS);
	rtlsdr_set_center_freq(d->dev, 100000000);
	rtlsdr_set_testmode(d->dev, 1);
	rtlsdr_reset_buffer(d->dev);
	return 0;
}

static void *reader(void *arg) {
	dstate *d = arg;
	if (prio > 0) {
		struct sched_param sp = { .sched_priority = prio };
		int e = pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
		if (e) fprintf(stderr, "dev %s: SCHED_RR %d failed: %s\n", d->serial, prio, strerror(e));
	}
	{ struct sched_param sp; int pol; pthread_getschedparam(pthread_self(), &pol, &sp); d->got_policy = pol; d->got_prio = sp.sched_priority; }
	while (!stop_all) {
		if (!d->dev) {
			if (open_dev(d) < 0) { d->open_errors++; usleep(200000); continue; }
		}
		d->uninit = 1; d->run_bufs = 0;
		d->running = 1;
		int r = rtlsdr_read_async(d->dev, cb, d, buf_num, buf_len);
		d->running = 0;
		check_window(d, now_s(), d->win_min, 1); /* close last window */
		d->runs++;
		if (r < 0) { d->run_errors++; fprintf(stderr, "dev %s: read_async returned %d\n", d->serial, r); usleep(200000); }
		if (reopen || stop_all) { rtlsdr_close(d->dev); d->dev = NULL; }
	}
	struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	d->thread_cpu = ts.tv_sec + ts.tv_nsec * 1e-9;
	return NULL;
}

static void on_sig(int s) { (void)s; stop_all = 1; }

int main(int argc, char **argv) {
	int opt; char *serials = NULL;
	while ((opt = getopt(argc, argv, "s:t:b:l:P:r:RpS:")) != -1) {
		switch (opt) {
		case 's': serials = strdup(optarg); break;
		case 't': duration = atof(optarg); break;
		case 'b': buf_num = atoi(optarg); break;
		case 'l': buf_len = atoi(optarg); break;
		case 'P': prio = atoi(optarg); break;
		case 'r': restart_s = atof(optarg); break;
		case 'R': reopen = 1; break;
		case 'p': poison = 1; break;
		case 'S': stall_ms = atof(optarg); break;
		default: fprintf(stderr, "usage: zctest -s 1000,1001 [-t s] [-b 32] [-l 32768] [-P 20] [-r s] [-R] [-p]\n"); return 2;
		}
	}
	if (!serials) return 2;
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	for (char *tok = strtok(serials, ","); tok && ndev < MAXDEV; tok = strtok(NULL, ",")) {
		dstate *d = &D[ndev]; memset(d, 0, sizeof *d);
		d->id = ndev; snprintf(d->serial, sizeof d->serial, "%s", tok);
		d->sink = malloc((size_t)64 * buf_len); memset(d->sink, 0, (size_t)64 * buf_len);
		d->min_step = 1e9; d->max_step = -1e9;
		if (open_dev(d) < 0) return 1;
		ndev++;
	}
	printf("zctest: %d dongles, %d x %d, prio %d, poison %d, restart %.1f s (%s), %.0f s\n",
		ndev, buf_num, buf_len, prio, poison, restart_s, reopen ? "close+reopen" : "same handle", duration);
	fflush(stdout);
	struct timespec pt0; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &pt0);
	double start = now_s();
	for (int i = 0; i < ndev; i++) pthread_create(&D[i].th, NULL, reader, &D[i]);
	double next_restart = start + restart_s;
	while (!stop_all && now_s() - start < duration) {
		usleep(50000);
		if (restart_s > 0 && now_s() >= next_restart) {
			for (int i = 0; i < ndev; i++) if (D[i].dev && D[i].running) rtlsdr_cancel_async(D[i].dev);
			next_restart += restart_s;
		}
	}
	stop_all = 1;
	for (int k = 0; k < 400; k++) { /* re-send the cancel until every reader is out */
		int any = 0;
		for (int i = 0; i < ndev; i++) if (D[i].running) { any = 1; rtlsdr_cancel_async(D[i].dev); }
		if (!any) break;
		usleep(10000);
	}
	for (int i = 0; i < ndev; i++) pthread_join(D[i].th, NULL);
	double el = now_s() - start;
	struct timespec pt1; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &pt1);
	double pcpu = (pt1.tv_sec - pt0.tv_sec) + (pt1.tv_nsec - pt0.tv_nsec) * 1e-9;
	uint64_t tot_gaps = 0, tot_corrupt = 0, tot_tp = 0, tot_tm = 0, tot_short = 0, tot_poi = 0;
	for (int i = 0; i < ndev; i++) {
		dstate *d = &D[i];
		double rate = d->bytes / el / 2;
		printf("dev %s: policy %s prio %d | runs %llu (errors %llu, open errors %llu) | bufs %llu short %llu | gaps %llu (%llu B mod256) corrupt %llu repeat-pairs %llu poison-pairs %llu | startup: gaps %llu (%llu B) corrupt %llu | timing steps +T %llu -T %llu (window step min %.3f max %.3f ms) | cb mean %.1f us max %.1f us | thread cpu %.2f s | %.0f S/s avg\n",
			d->serial, d->got_policy == SCHED_RR ? "RR" : d->got_policy == SCHED_FIFO ? "FIFO" : "OTHER", d->got_prio,
			(unsigned long long)d->runs, (unsigned long long)d->run_errors, (unsigned long long)d->open_errors,
			(unsigned long long)d->bufs, (unsigned long long)d->short_bufs,
			(unsigned long long)d->gaps, (unsigned long long)d->gap_bytes, (unsigned long long)d->corrupt_bufs,
			(unsigned long long)d->repeat_pairs, (unsigned long long)d->poison_pairs,
			(unsigned long long)d->startup_gaps, (unsigned long long)d->startup_gap_bytes, (unsigned long long)d->startup_corrupt,
			(unsigned long long)d->timing_plus, (unsigned long long)d->timing_minus, d->min_step * 1e3, d->max_step * 1e3,
			d->bufs ? d->cb_sum / d->bufs * 1e6 : 0, d->cb_max * 1e6, d->thread_cpu, rate);
		if (d->first_corrupt[0]) printf("dev %s: first corrupt buffer: %s\n", d->serial, d->first_corrupt);
		tot_gaps += d->gaps; tot_corrupt += d->corrupt_bufs; tot_tp += d->timing_plus; tot_tm += d->timing_minus; tot_short += d->short_bufs; tot_poi += d->poison_pairs;
	}
	printf("TOTAL: elapsed %.1f s, process cpu %.2f s, gaps %llu, corrupt %llu, short %llu, poison-pairs %llu, timing +T %llu -T %llu\n", el, pcpu,
		(unsigned long long)tot_gaps, (unsigned long long)tot_corrupt, (unsigned long long)tot_short, (unsigned long long)tot_poi,
		(unsigned long long)tot_tp, (unsigned long long)tot_tm);
	return 0;
}
