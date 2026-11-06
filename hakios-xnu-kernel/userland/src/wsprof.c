/*
 * wsprof.c: where the window server's drawing thread is between two frames (part of libhakios.gl.dylib; boot-arg
 * ws_prof=US with touch_lat=1: a look every US microseconds, 1000 if 1).
 *
 * Why: to see what the window server (backboardd) does between the end of a frame (its glFlush, "tl: g ... ended") and
 * the beginning of the next. Found (logs/134143-run.log, ws_prof=500): 67-99% of the time in its run loop's mach_msg,
 * waiting for its own clock's next tick; up to 12% in IOConnectCallMethod, the swap handed to the display driver.
 * SwapEnd ("tl: s") comes at once after the glFlush, under 0.1 ms. (A gap of 25-60 ms seen before was the two counters
 * taken for one: "tl: g N" counts eagl.c's frames, "tl: s N" and "tl: v N" the display driver's swaps, and they drift
 * apart by a frame or two -- match them by time, as tools/frames.py does.)
 *
 * eagl.c calls wsprof_frame() at the end of every frame, on the drawing thread: that thread is the one looked at.
 * A thread of this file stops it every US microseconds for a moment, takes its pc, lr and the return addresses of
 * its frame-pointer chain, and lets it go (nothing that takes a lock while it is stopped). Every 5 seconds the looks
 * taken between two frames that were less than 100 ms apart (the motion going on) are put together by their top
 * frames, with names from dladdr (the exported ones; the shared cache keeps no others: "<image>" then), and written:
 * "ws: between frames, N looks (M ms): P% NAME < NAME < ..." (the leaf first). The looks taken in frames of 10 ms
 * or more (from the first draw to the glFlush) likewise: "ws: in frames of 10 ms or more, ...".
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void wsprof_start(unsigned every_us);
void wsprof_frame(uint64_t began_us, uint64_t ended_us);

#define WS_LOOKS        16384
#define WS_DEPTH        8
#define WS_FRAMES       1024
#define WS_SHOWN        5
#define WS_REPORT_US    5000000
#define WS_SLOW_US      10000

static struct ws_look {
	uint64_t at;
	uint64_t stack[WS_DEPTH];
	uint8_t depth;
} ws_looks[WS_LOOKS];
static volatile uint64_t ws_look_next;
static struct { volatile uint64_t began, ended; } ws_frames[WS_FRAMES];
static volatile uint64_t ws_frame_next;
static thread_t ws_target = MACH_PORT_NULL;
static uint64_t ws_stack_low, ws_stack_high;
static unsigned ws_every_us;
static mach_timebase_info_data_t ws_timebase;

static uint64_t
ws_now_us(void)
{
	return mach_absolute_time() * ws_timebase.numer / ws_timebase.denom / 1000;
}

static void
ws_look(void)
{
	struct ws_look *look = &ws_looks[ws_look_next % WS_LOOKS];
	arm_thread_state64_t state;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	unsigned depth = 0;
	uint64_t at = ws_now_us();

	if (thread_suspend(ws_target) != KERN_SUCCESS) return;
	if (thread_get_state(ws_target, ARM_THREAD_STATE64, (thread_state_t)&state, &count) == KERN_SUCCESS) {
		uint64_t fp = arm_thread_state64_get_fp(state), sp = arm_thread_state64_get_sp(state);

		look->stack[depth++] = arm_thread_state64_get_pc(state);
		look->stack[depth++] = arm_thread_state64_get_lr(state);
		while (depth < WS_DEPTH && fp >= sp && fp >= ws_stack_low && fp + 16 <= ws_stack_high && (fp & 15) == 0) {
			const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;

			if (frame[1] == 0) break;
			look->stack[depth++] = frame[1];
			if (frame[0] <= fp) break;
			fp = frame[0];
		}
	}
	thread_resume(ws_target);
	look->at = at;
	look->depth = (uint8_t)depth;
	ws_look_next++;
}

/* a name for an address: the exported symbol's, else the image's */
static void
ws_name(uint64_t address, char *name, size_t size)
{
	Dl_info info;
	const char *image;

	if (dladdr((const void *)(uintptr_t)address, &info) == 0 || info.dli_fname == NULL) {
		snprintf(name, size, "%#llx", (unsigned long long)address);
		return;
	}
	image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
	if (info.dli_sname != NULL && strcmp(info.dli_sname, "<redacted>") != 0) {
		snprintf(name, size, "%s", info.dli_sname);
	} else {
		snprintf(name, size, "<%s>", image);
	}
}

struct ws_stack {
	char text[400];
	unsigned looks;
};

static int
ws_stack_order(const void *a, const void *b)
{
	const struct ws_stack *x = a, *y = b;

	return x->looks < y->looks ? 1 : x->looks > y->looks ? -1 : 0;
}

/* one more look into a table of stacks put together by their top frames */
static void
ws_count(struct ws_stack *stacks, unsigned *n, unsigned room, const struct ws_look *look)
{
	char text[400] = "", one[96];
	unsigned k;

	for (unsigned d = 0; d < look->depth && d < 5; d++) {
		ws_name(look->stack[d], one, sizeof(one));
		/* (the same name twice in a row: once) */
		if (d > 0 && strstr(text, one) == text + strlen(text) - strlen(one)) continue;
		if (d > 0) strlcat(text, " < ", sizeof(text));
		strlcat(text, one, sizeof(text));
	}
	for (k = 0; k < *n && strcmp(stacks[k].text, text) != 0; k++) {
	}
	if (k == *n) {
		if (*n == room) return;
		strlcpy(stacks[k].text, text, sizeof(stacks[k].text));
		stacks[(*n)++].looks = 0;
	}
	stacks[k].looks++;
}

static void
ws_write(const char *what, struct ws_stack *stacks, unsigned n, unsigned total, unsigned other)
{
	qsort(stacks, n, sizeof(stacks[0]), ws_stack_order);
	dprintf(2, "ws: %s, %u looks (%u ms; %u looks %s):", what, total, total * ws_every_us / 1000, other,
	    what[0] == 'b' ? "drawing" : "in the other frames");
	for (unsigned k = 0; k < n && k < WS_SHOWN; k++) {
		dprintf(2, "%s %u%% %s", k ? ";" : "", stacks[k].looks * 100 / total, stacks[k].text);
	}
	dprintf(2, "\n");
}

/*
 * The looks since `from` between two frames of the motion, put together; and those in frames of WS_SLOW_US or more
 * (what such a frame's drawing thread does: "ws: in frames of 10 ms or more, ...").
 */
static void
ws_report(uint64_t from_look, uint64_t to_look)
{
	static struct ws_stack stacks[256], slow[256];
	unsigned n = 0, total = 0, drawing = 0, slow_n = 0, slow_total = 0;
	uint64_t frames_to = ws_frame_next, frames_from = frames_to > WS_FRAMES ? frames_to - WS_FRAMES + 1 : 0;

	if (to_look - from_look > WS_LOOKS) from_look = to_look - WS_LOOKS;
	for (uint64_t i = from_look; i < to_look; i++) {
		const struct ws_look *look = &ws_looks[i % WS_LOOKS];
		uint64_t before = 0, after = 0, length = 0;
		bool in_frame = false;

		/* the frame that ended last before the look, and the one that began first after it */
		for (uint64_t f = frames_from; f < frames_to; f++) {
			uint64_t began = ws_frames[f % WS_FRAMES].began, ended = ws_frames[f % WS_FRAMES].ended;

			if (began <= look->at && look->at < ended) {
				in_frame = true;
				length = ended - began;
				break;
			}
			if (ended <= look->at) before = ended;
			if (began > look->at) {
				after = began;
				break;
			}
		}
		if (in_frame) {
			drawing++;
			if (length >= WS_SLOW_US) {
				slow_total++;
				ws_count(slow, &slow_n, sizeof(slow) / sizeof(slow[0]), look);
			}
			continue;
		}
		if (before == 0 || after == 0 || after - before >= 100000) continue;
		total++;
		ws_count(stacks, &n, sizeof(stacks) / sizeof(stacks[0]), look);
	}
	if (total != 0) ws_write("between frames", stacks, n, total, drawing);
	if (slow_total != 0) ws_write("in frames of 10 ms or more", slow, slow_n, slow_total, drawing - slow_total);
}

static void *
ws_main(__unused void *argument)
{
	uint64_t reported = ws_now_us(), from = 0;

	pthread_setname_np("com.hakios.wsprof");
	for (;;) {
		ws_look();
		usleep(ws_every_us);
		if (ws_now_us() - reported >= WS_REPORT_US) {
			uint64_t to = ws_look_next;

			ws_report(from, to);
			from = to;
			reported = ws_now_us();
		}
	}
	return NULL;
}

void
wsprof_start(unsigned every_us)
{
	ws_every_us = every_us >= 100 ? every_us : 1000;
	mach_timebase_info(&ws_timebase);
}

/* on the drawing thread, at the end of each of its frames */
void
wsprof_frame(uint64_t began_us, uint64_t ended_us)
{
	uint64_t i;

	if (ws_every_us == 0) return;
	if (ws_target == MACH_PORT_NULL) {
		pthread_t self = pthread_self(), thread;

		ws_stack_high = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(self);
		ws_stack_low = ws_stack_high - pthread_get_stacksize_np(self);
		ws_target = mach_thread_self();
		if (pthread_create(&thread, NULL, ws_main, NULL) == 0) {
			pthread_detach(thread);
			dprintf(2, "ws: the drawing thread looked at every %u us\n", ws_every_us);
		}
	}
	i = ws_frame_next;
	ws_frames[i % WS_FRAMES].began = began_us;
	ws_frames[i % WS_FRAMES].ended = ended_us;
	ws_frame_next = i + 1;
}
