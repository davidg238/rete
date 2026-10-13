/* Copyright (c) 2026 Ekorau LLC */
/* The remote console — printk teed to UDP, one datagram per line. See log_udp.h. */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
/* printk_hook_fn_t / __printk_hook_install / __printk_get_hook live HERE, not in
 * printk.h — chaining needs the getter, and the getter is only in this header. */
#include <zephyr/sys/printk-hooks.h>
#include <string.h>
#include <errno.h>

#include "log_udp.h"

#ifdef CONFIG_NET_SOCKETS

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
/* The sender's idle poll. */
#define POLL_MS    50
/* rete#415, measured on c6b over Thread (2026-10-13): a dump's lines leave ~4 ms apart
 * and the stack runs out of net buffers after ~80 of them; sendto then fails with
 * ENOMEM until frames drain. That is pressure, not a dead route, so it is retried
 * after BUSY_MS; any other error (no route) after DEAD_MS. The paced writer stops
 * waiting only after GIVE_UP_MS with no progress, which a DEAD_MS back-off alone
 * never reaches. */
#define BUSY_MS    10
#define DEAD_MS    500
#define GIVE_UP_MS 2000

static char ring[RING_BYTES];
static uint32_t r_head, r_tail;
static uint32_t dropped_chars, sent_lines, send_errors;
static struct k_spinlock ring_lock;
static printk_hook_fn_t prev_hook;

/* rete#415: where the lines go. HOLD until the first target (the boot waits in the
   ring, as it waited for a route), ON once one is set, OFF after an empty one (lines
   are discarded: nobody asked for them). `gen` moves on every new target, so the
   sender re-opens on the next pass. */
enum { TEE_HOLD, TEE_ON, TEE_OFF };
static volatile int tee_state = TEE_HOLD;
static volatile uint32_t tee_gen;
static struct sockaddr_in6 tee_dst;       /* written under ring_lock */
static char tee_addr[INET6_ADDRSTRLEN];   /* for the boot and target lines */
static int tee_port;
static volatile int sender_pending;       /* a refused line is held for retry */
K_SEM_DEFINE(tee_wake, 0, 1);

/* rete#415: the target survives deep sleep and a restart, so a waking node tees from
   its first line rather than holding the boot until its first check-in replays
   goal/console (a profile window opened from boot can dump before then). RTC_NOINIT
   memory on espressif, as main.c's g_rtc; elsewhere plain .bss, which a boot zeroes,
   and the check fails, as it does on a power-on's garbage. `on` 0 is an off kept. */
#define TEE_KEEP_MAGIC 0x7EE15415u
struct tee_keep { uint32_t check; uint16_t port; uint8_t on; char addr[INET6_ADDRSTRLEN]; };
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#include <esp_attr.h>   /* RTC_NOINIT_ATTR */
static RTC_NOINIT_ATTR struct tee_keep tee_kept;
#else
static struct tee_keep tee_kept;
#endif

static uint32_t tee_keep_sum(const struct tee_keep *k)
{
	uint32_t h = 2166136261u ^ TEE_KEEP_MAGIC;
	size_t i;

	h = (h ^ k->port) * 16777619u;
	h = (h ^ k->on) * 16777619u;
	for (i = 0; i < sizeof k->addr; i++) {
		h = (h ^ (uint8_t)k->addr[i]) * 16777619u;
	}
	return h;
}

static void tee_keep_put(const char *addr, int port, int on)
{
	memset(&tee_kept, 0, sizeof tee_kept);
	strncpy(tee_kept.addr, addr, sizeof tee_kept.addr - 1);
	tee_kept.port = (uint16_t)port;
	tee_kept.on = (uint8_t)on;
	tee_kept.check = tee_keep_sum(&tee_kept);
}

static int tee_from_keep;

static int tee_keep_valid(void)
{
	return tee_kept.check == tee_keep_sum(&tee_kept) &&
	       tee_kept.addr[sizeof tee_kept.addr - 1] == '\0';
}

/* Called from EVERY context printk is called from — thread, workqueue, ISR. It
 * must never block and never allocate. A spinlock rather than a lock-free ring
 * because printk has multiple concurrent producers and a torn index would
 * corrupt the log we are here to trust. */
static int ring_put(char c)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	uint32_t next = (r_head + 1u) % RING_BYTES;
	int put = 0;

	if (tee_state == TEE_OFF) {
		put = 1;                  /* nobody asked: discard, uncounted */
	} else if (next == r_tail) {
		dropped_chars++;          /* full: drop, count, never block */
	} else {
		ring[r_head] = c;
		r_head = next;
		put = 1;
	}
	k_spin_unlock(&ring_lock, key);
	return put;
}

static uint32_t ring_used(void)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	uint32_t u = (r_head + RING_BYTES - r_tail) % RING_BYTES;
	k_spin_unlock(&ring_lock, key);
	return u;
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

/* rete#415 F6: a profile dump is ~10 KB written in one burst, and the ring is 4 KB,
 * so an unpaced dump lost its tail, `end` line included, which is the line custos
 * waits for. The writer is the VM thread (a window's close, or deepSleep:), so it may
 * wait. It waits while the sender is moving the tail; a sender that has not moved
 * for GIVE_UP_MS (no route) is not waited on until it moves again, so a dead route
 * costs one stall per dump, not one per line. */
void log_udp_tee_paced(const char *bytes, size_t n)
{
	static int gave_up;
	static uint32_t gave_up_tail;
	uint32_t tail = r_tail;
	int still = 0;

	while (tee_state == TEE_ON && !gave_up && RING_BYTES - 1 - ring_used() < n) {
		k_msleep(2);
		if (r_tail != tail) {
			tail = r_tail;
			still = 0;
		} else if ((still += 2) >= GIVE_UP_MS) {
			gave_up = 1;
		}
	}
	if (gave_up && r_tail != gave_up_tail) {
		gave_up = 0;              /* the sender moves again: pace the next line again */
	}
	if (gave_up) {
		gave_up_tail = r_tail;
	}
	log_udp_tee(bytes, n);
}

/* A send refused for want of buffers, which drain on their own. */
static int busy(int e)
{
	return e == ENOMEM || e == ENOBUFS || e == EAGAIN;
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

static int set_target(const char *addr, int port)
{
	struct sockaddr_in6 dst;
	k_spinlock_key_t key;

	memset(&dst, 0, sizeof dst);
	dst.sin6_family = AF_INET6;
	dst.sin6_port = htons(port);
	if (zsock_inet_pton(AF_INET6, addr, &dst.sin6_addr) != 1) {
		return 0;
	}
	key = k_spin_lock(&ring_lock);
	tee_dst = dst;
	tee_gen++;
	tee_state = TEE_ON;
	k_spin_unlock(&ring_lock, key);
	strncpy(tee_addr, addr, sizeof tee_addr - 1);
	tee_port = port;
	k_sem_give(&tee_wake);
	return 1;
}

int log_udp_set_target(const char *addr6, size_t len, int port)
{
	char a[INET6_ADDRSTRLEN];

	if (len == 0) {
		k_spinlock_key_t key = k_spin_lock(&ring_lock);
		if (tee_state != TEE_OFF) {
			tee_state = TEE_OFF;
			r_tail = r_head;      /* what was queued for the old target is nobody's */
		}
		k_spin_unlock(&ring_lock, key);
		tee_keep_put("", 0, 0);
		printk("logudp: off (goal/console)\n");   /* reaches the UART only */
		return 1;
	}
	if (len >= sizeof a || port < 1 || port > 65535) {
		return 0;
	}
	memcpy(a, addr6, len);
	a[len] = '\0';
	if (tee_state == TEE_ON && port == tee_port && strcmp(a, tee_addr) == 0) {
		return 1;                 /* the replayed cell: no new socket, no new line */
	}
	if (!set_target(a, port)) {
		return 0;
	}
	tee_keep_put(a, port, 1);
	printk("logudp: teeing console to [%s]:%d (goal/console)\n", a, port);
	return 1;
}

void log_udp_drain(int max_ms)
{
	int waited = 0;

	while (tee_state == TEE_ON && (ring_used() > 0 || sender_pending) && waited < max_ms) {
		k_msleep(10);
		waited += 10;
	}
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
	uint32_t gen = 0;
	uint32_t reported_drops = 0;

	while (1) {
		if (tee_state != TEE_ON) {
			(void)k_sem_take(&tee_wake, K_MSEC(200));
			continue;
		}
		if (gen != tee_gen) {     /* a new target: re-open and aim there */
			k_spinlock_key_t key = k_spin_lock(&ring_lock);
			dst = tee_dst;
			gen = tee_gen;
			k_spin_unlock(&ring_lock, key);
			if (sock >= 0) {
				zsock_close(sock);
				sock = -1;
			}
		}
		if (sock < 0) {
			sock = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
			if (sock < 0) {
				k_msleep(2000);   /* no stack yet */
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
				k_msleep(busy(errno) ? BUSY_MS : DEAD_MS);
				continue;
			}
			sent_lines++;
			pending = 0;
			sender_pending = 0;
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
						sender_pending = 1;
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
			k_msleep(busy(errno) ? BUSY_MS : DEAD_MS);
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

		k_msleep(POLL_MS);
	}
}

K_THREAD_DEFINE(log_udp_tid, 2048, log_udp_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(10), 0, 0);

/* Installed at POST_KERNEL, which runs BEFORE bg_thread_main prints the boot
 * banner — and the banner is what custos's run split (GET /runs) uses to open a run. A
 * remote log that starts after it parses as zero runs, so the reader would work
 * on the local store and quietly fail on this one. */
static int log_udp_hook_init(void)
{
	prev_hook = __printk_get_hook();
	__printk_hook_install(tee_char_out);
	if (tee_keep_valid()) {
		if (tee_kept.on) {
			(void)set_target(tee_kept.addr, tee_kept.port);
		} else {
			tee_state = TEE_OFF;
		}
		tee_from_keep = 1;
		return 0;
	}
#ifdef OEVM_LOG_UDP6
	(void)set_target(OEVM_LOG_UDP6, OEVM_LOG_UDP_PORT);
#endif
	return 0;
}
SYS_INIT(log_udp_hook_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

void log_udp_start(void)
{
	if (tee_state == TEE_ON) {
		printk("logudp: teeing console to [%s]:%d (%s)\n", tee_addr, tee_port,
		       tee_from_keep ? "kept from the last goal/console" : "build-time; goal/console overrides");
	} else if (tee_state == TEE_OFF) {
		printk("logudp: off (kept from the last goal/console)\n");
	} else {
		printk("logudp: holding the console until goal/console names a target\n");
	}
}

#else  /* !CONFIG_NET_SOCKETS */

void log_udp_start(void)
{
	/* No network in this image, so no remote console. Say it out loud: an image
	 * that cannot tee looks identical to one whose target is unreachable, and this
	 * bench has now been bitten twice by a build-time omission wearing the costume
	 * of a network fault. */
	printk("logudp: no sockets in this image — console is UART only\n");
}

/* The no-ops the VM's console leaves call in an image without a network. Defined
   HERE rather than as weak stubs beside the call sites, for the reason recorded in
   platform_zephyr.c. */
void log_udp_tee(const char *bytes, size_t n)
{
	(void)bytes; (void)n;
}
void log_udp_tee_paced(const char *bytes, size_t n)
{
	(void)bytes; (void)n;
}
int log_udp_set_target(const char *addr6, size_t len, int port)
{
	(void)addr6; (void)len; (void)port;
	return 0;
}
void log_udp_drain(int max_ms)
{
	(void)max_ms;
}

#endif /* CONFIG_NET_SOCKETS */
