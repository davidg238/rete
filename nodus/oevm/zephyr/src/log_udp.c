/* Copyright (c) 2026 Ekorau LLC */
/* The remote console — printk teed to UDP, one datagram per line. See log_udp.h. */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
/* printk_hook_fn_t / __printk_hook_install / __printk_get_hook live HERE, not in
 * printk.h — chaining needs the getter, and the getter is only in this header. */
#include <zephyr/sys/printk-hooks.h>
#include <string.h>

#include "log_udp.h"

#ifdef OEVM_LOG_UDP6

#include <zephyr/net/socket.h>

#ifndef OEVM_LOG_UDP_PORT
#define OEVM_LOG_UDP_PORT 54330
#endif

/* 4 KB holds the whole Zephyr-side boot chatter with room to spare, which matters
 * because the hook is installed long before the mesh is up: everything printed
 * between boot and attach sits here and goes out in one burst once the socket
 * works. Overflow drops the NEWEST character and counts it — see the report. */
#define RING_BYTES 4096
#define LINE_MAX   256

static char ring[RING_BYTES];
static uint32_t r_head, r_tail;
static uint32_t dropped_chars, sent_lines, send_errors;
static struct k_spinlock ring_lock;
static printk_hook_fn_t prev_hook;

/* Called from EVERY context printk is called from — thread, workqueue, ISR. It
 * must never block and never allocate. A spinlock rather than a lock-free ring
 * because printk has multiple concurrent producers and a torn index would
 * corrupt the log we are here to trust. */
static int ring_put(char c)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	uint32_t next = (r_head + 1u) % RING_BYTES;
	int put = 0;

	if (next == r_tail) {
		dropped_chars++;          /* full: drop, count, never block */
	} else {
		ring[r_head] = c;
		r_head = next;
		put = 1;
	}
	k_spin_unlock(&ring_lock, key);
	return put;
}

static int tee_char_out(int c)
{
	(void)ring_put((char)c);
	/* Chain, so the UART console and the #56 collector are untouched. */
	return prev_hook ? prev_hook(c) : c;
}

/* The VM's console leaf calls this beside its fwrite (platform_zephyr.c). nsl's
 * `out` does not go through printk — it goes through libc stdout — so without
 * this the remote console would carry every C-side line and NONE of the node's,
 * which is to say none of the bell lines. Found by reading the first remote log
 * and noticing what was not in it. */
void log_udp_tee(const char *bytes, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		(void)ring_put(bytes[i]);
	}
}

static int ring_get(char *out)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	int got = 0;

	if (r_tail != r_head) {
		*out = ring[r_tail];
		r_tail = (r_tail + 1u) % RING_BYTES;
		got = 1;
	}
	k_spin_unlock(&ring_lock, key);
	return got;
}

static int log_udp_socket(struct sockaddr_in6 *dst)
{
	int s;

	memset(dst, 0, sizeof *dst);
	dst->sin6_family = AF_INET6;
	dst->sin6_port = htons(OEVM_LOG_UDP_PORT);
	if (zsock_inet_pton(AF_INET6, OEVM_LOG_UDP6, &dst->sin6_addr) != 1) {
		return -1;              /* a bad literal is a build error, not a runtime one */
	}
	s = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	return s;
}

/* One datagram per line. UDP because a console that can BLOCK is a console that
 * can hang the thing it is observing: a dropped line costs a line, while a
 * blocked send costs the bell. Loss is acceptable here in a way it is not on the
 * space wire — and the receiver stamps arrival, so ordering survives loss. */
static void log_udp_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	struct sockaddr_in6 dst;
	char line[LINE_MAX];
	int len = 0;
	int pending = 0;          /* `line` is assembled and NOT yet accepted */
	int sock = -1;
	uint32_t reported_drops = 0;

	while (1) {
		if (sock < 0) {
			sock = log_udp_socket(&dst);
			if (sock < 0) {
				k_msleep(2000);   /* no route yet, or a bad literal */
				continue;
			}
		}

		/* HOLD A REFUSED LINE, do not discard it. The first version counted the
		 * error and moved on, which threw away everything printed before the
		 * mesh attached — the Zephyr banner, `oevm: build-id`, the node's own
		 * ready line. Those are the most valuable lines in a remote log and the
		 * only ones guaranteed to be produced before there is a route. The ring
		 * keeps filling behind this and drops the NEWEST characters if it
		 * overflows, which is the right end to lose: a boot is bounded and a
		 * running node's chatter is not. Witnessed 2026-08-04, first flash. */
		if (pending) {
			if (zsock_sendto(sock, line, len, 0,
					 (struct sockaddr *)&dst, sizeof dst) < 0) {
				send_errors++;
				k_msleep(500);
				continue;
			}
			sent_lines++;
			pending = 0;
			len = 0;
		}

		char ch;
		while (!pending && ring_get(&ch)) {
			if (ch == '\r') {
				continue;
			}
			if (ch == '\n' || len == LINE_MAX - 1) {
				if (len > 0) {
					if (zsock_sendto(sock, line, len, 0,
							 (struct sockaddr *)&dst, sizeof dst) < 0) {
						send_errors++;
						pending = 1;   /* retry this exact line */
					} else {
						sent_lines++;
						len = 0;
					}
				} else {
					len = 0;
				}
			} else {
				line[len++] = ch;
			}
		}
		if (pending) {
			k_msleep(500);
			continue;
		}

		/* SAY WHAT WAS LOST. A logging path that drops silently is the same
		 * failure as a health check that only proves itself (#147): it reports
		 * success while the evidence goes missing. Reported through the ring,
		 * so it also reaches the local console. */
		if (dropped_chars != reported_drops) {
			reported_drops = dropped_chars;
			printk("logudp: dropped %u chars, %u send errors, %u lines sent\n",
			       dropped_chars, send_errors, sent_lines);
		}

		k_msleep(50);
	}
}

K_THREAD_DEFINE(log_udp_tid, 2048, log_udp_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(10), 0, 0);

/* Installed at POST_KERNEL, which runs BEFORE bg_thread_main prints the boot
 * banner — and the banner is what logread.split_runs() uses to open a run. A
 * remote log that starts after it parses as zero runs, so the reader would work
 * on the local store and quietly fail on this one. */
static int log_udp_hook_init(void)
{
	prev_hook = __printk_get_hook();
	__printk_hook_install(tee_char_out);
	return 0;
}
SYS_INIT(log_udp_hook_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

void log_udp_start(void)
{
	printk("logudp: teeing console to [%s]:%d\n", OEVM_LOG_UDP6, OEVM_LOG_UDP_PORT);
}

#else  /* !OEVM_LOG_UDP6 */

void log_udp_start(void)
{
	/* No remote console in this image. Say it
	 * out loud. An image built without the address looks identical to one
	 * whose gateway is unreachable, and this bench has now been bitten twice by a
	 * build-time omission wearing the costume of a network fault. */
	printk("logudp: not built in (no OEVM_LOG_UDP6) — console is UART only\n");
}

/* The no-op the VM's console leaf calls in an image without a remote console.
   Defined HERE rather than as a weak stub beside the call site, for the reason
   recorded in platform_zephyr.c. */
void log_udp_tee(const char *bytes, size_t n)
{
	(void)bytes; (void)n;
}

#endif /* OEVM_LOG_UDP6 */
