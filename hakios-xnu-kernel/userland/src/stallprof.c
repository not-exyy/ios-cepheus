/*
 * (hakios) boot-arg sb_prof=US, with touch_lat=1: what SpringBoard and the rest of the system did while the screen
 * stood still. A thread of ours looks at every thread of SpringBoard every US microseconds (4000 is a good value):
 * the main thread's stack always, any other thread's stack only when it is running or has had processor time since
 * the last look; and every 25th look (proc_pidinfo of them all takes 40 ms here), the processor time of every process. The looks of the last two seconds are
 * kept. inject_sb.c's run loop observer says when the screen stood still (stallprof_window): one turn of the main
 * run loop that took 50 ms or more before its commit ("turn"), or 120 ms or more between two commits that changed
 * something, with such commits within 100 ms before ("gap"). For each, a little later, the log gets
 *
 *   stall: gap 274 ms at 2011733454..2011981162: 68 looks; SpringBoard 31% (main 2%), its threads: NAME 25% ...
 *   stall:   T NAME N/LOOKS cpu MS ms: leaf function < caller < ...    (the most frequent stacks of a thread)
 *   stall:   processes: backboardd[13] 40% kernel_task[0] 12% ...     (100% is one core's whole time)
 *
 * Functions are named by dladdr, or by the closest Objective-C method below the address in the same image
 * (-[Class selector]), whichever starts closer: the shared cache keeps no local symbols. tools/frames.py ignores
 * the lines. Looking costs about 1 ms of every 20 ("stall: cost:" lines say).
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define SP_DEPTH        28
#define SP_RECS         8192            /* thread looks kept (a few per look at all of them) */
#define SP_SNAPS        64              /* process looks kept */
#define SP_PROC_EVERY   25              /* (proc_pidinfo of every process takes 40 ms here: every 25th look) */
#define SP_PROCS        512
#define SP_THREADS      256
#define SP_WINDOWS      32

#define PROC_ALL_PIDS           1
#define PROC_PIDTASKINFO        4
struct sp_taskinfo {
	uint64_t virtual_size, resident_size, total_user, total_system, threads_user, threads_system;
	int32_t policy, faults, pageins, cow_faults, messages_sent, messages_received, syscalls_mach, syscalls_unix,
	    csw, threadnum, numrunning, priority;
};
int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);

void stallprof_start(void);
void stallprof_window(uint64_t from_us, uint64_t to_us, const char *why);
void stallprof_name(uintptr_t address, char *name, size_t size);

struct sp_rec {
	uint64_t at;                    /* us */
	uint64_t tid;
	uint32_t cpu_us;                /* processor time since the look before */
	uint8_t state, depth;
	uint64_t stack[SP_DEPTH];
};
static struct sp_rec sp_recs[SP_RECS];
static unsigned sp_rec_next;            /* only the sampler thread writes and reads them */

struct sp_snap {
	uint64_t at;
	unsigned n;
	int pid[SP_PROCS];
	uint64_t time[SP_PROCS];        /* mach_absolute_time's units */
};
static struct sp_snap sp_snaps[SP_SNAPS];
static unsigned sp_snap_next;

static struct sp_thread {
	uint64_t tid;
	uint64_t cpu_us;
	uint64_t stack_low, stack_high;
	uint64_t seen;                  /* the last look that found it (us) */
	char name[32];
} sp_threads[SP_THREADS];

static struct {
	uint64_t from, to;
	char why[8];
} sp_windows[SP_WINDOWS];
static volatile unsigned sp_window_put, sp_window_got;
static pthread_mutex_t sp_window_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sp_name_lock = PTHREAD_MUTEX_INITIALIZER;        /* the names' tables: the sampler's and layerdiff.c's */

static unsigned sp_period_us;
static uint64_t sp_main_tid;
static mach_timebase_info_data_t sp_timebase;

static uint64_t
sp_now_us(void)
{
	return mach_absolute_time() * sp_timebase.numer / sp_timebase.denom / 1000;
}

static void
sp_say(const char *format, ...)
{
	char line[1400];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
		line[n - 1] = '\n';
	}
	write(2, line, (size_t)n);
}

static struct sp_thread *
sp_thread_of(uint64_t tid, bool make)
{
	for (unsigned i = (unsigned)(tid * 0x9e3779b97f4a7c15ull >> 56), probes = 0; probes < SP_THREADS; i = (i + 1) % SP_THREADS, probes++) {
		if (sp_threads[i].tid == tid) return &sp_threads[i];
		if (sp_threads[i].tid == 0) {
			if (!make) return NULL;
			memset(&sp_threads[i], 0, sizeof(sp_threads[i]));
			sp_threads[i].tid = tid;
			return &sp_threads[i];
		}
	}
	return NULL;
}

/* threads come and go (the dispatch queues' workers): forget those not seen for 3 s, when the table fills up */
static void
sp_forget_threads(uint64_t at)
{
	static struct sp_thread live[SP_THREADS];
	unsigned n = 0, used = 0;

	for (unsigned i = 0; i < SP_THREADS; i++) used += sp_threads[i].tid != 0;
	if (used < SP_THREADS * 3 / 4) return;
	for (unsigned i = 0; i < SP_THREADS; i++) {
		if (sp_threads[i].tid != 0 && sp_threads[i].seen + 3000000 > at) live[n++] = sp_threads[i];
	}
	memset(sp_threads, 0, sizeof(sp_threads));
	for (unsigned i = 0; i < n; i++) *sp_thread_of(live[i].tid, true) = live[i];
}

static void
sp_look_at_processes(uint64_t at)
{
	static int pids[SP_PROCS];
	struct sp_snap *snap = &sp_snaps[sp_snap_next % SP_SNAPS];
	int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)), n = bytes > 0 ? bytes / (int)sizeof(int) : 0;

	snap->n = 0;
	for (int i = 0; i < n && snap->n < SP_PROCS; i++) {
		struct sp_taskinfo info;

		if (proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &info, sizeof(info)) != (int)sizeof(info)) continue;
		snap->pid[snap->n] = pids[i];
		snap->time[snap->n] = info.total_user + info.total_system;
		snap->n++;
	}
	snap->at = at;
	sp_snap_next++;
}

static void
sp_look(void)
{
	thread_act_array_t threads;
	mach_msg_type_number_t n = 0;
	thread_t self = mach_thread_self();
	uint64_t at = sp_now_us();

	mach_port_deallocate(mach_task_self(), self);
	if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS) return;
	for (mach_msg_type_number_t t = 0; t < n; t++) {
		thread_basic_info_data_t basic;
		thread_identifier_info_data_t ident;
		mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT, icount = THREAD_IDENTIFIER_INFO_COUNT;
		struct sp_thread *known;
		uint64_t cpu, delta;

		if (threads[t] == self || thread_info(threads[t], THREAD_BASIC_INFO, (thread_info_t)&basic, &count) != KERN_SUCCESS ||
		    thread_info(threads[t], THREAD_IDENTIFIER_INFO, (thread_info_t)&ident, &icount) != KERN_SUCCESS ||
		    (known = sp_thread_of(ident.thread_id, true)) == NULL) {
			mach_port_deallocate(mach_task_self(), threads[t]);
			continue;
		}
		known->seen = at;
		cpu = (uint64_t)basic.user_time.seconds * 1000000 + (uint64_t)basic.user_time.microseconds +
		      (uint64_t)basic.system_time.seconds * 1000000 + (uint64_t)basic.system_time.microseconds;
		delta = known->cpu_us != 0 && cpu >= known->cpu_us ? cpu - known->cpu_us : 0;
		known->cpu_us = cpu;
		if (known->stack_high == 0) {
			/* (before the thread is stopped: pthread_from_mach_thread_np takes the thread list's lock) */
			thread_extended_info_data_t ext;
			mach_msg_type_number_t ecount = THREAD_EXTENDED_INFO_COUNT;
			pthread_t pt = pthread_from_mach_thread_np(threads[t]);

			if (pt != NULL) {
				known->stack_high = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(pt);
				known->stack_low = known->stack_high - pthread_get_stacksize_np(pt);
			} else {
				known->stack_high = 1;
			}
			if (thread_info(threads[t], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &ecount) == KERN_SUCCESS) {
				strlcpy(known->name, ext.pth_name, sizeof(known->name));
			}
		}
		if (ident.thread_id == sp_main_tid || basic.run_state == TH_STATE_RUNNING || delta > 0) {
			struct sp_rec *rec = &sp_recs[sp_rec_next % SP_RECS];
			arm_thread_state64_t state;
			mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
			unsigned depth = 0;

			/* while it is stopped nothing that may take a lock: no malloc, no dladdr, no objc */
			if (thread_suspend(threads[t]) == KERN_SUCCESS) {
				if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&state, &state_count) == KERN_SUCCESS) {
					uint64_t fp = arm_thread_state64_get_fp(state), sp = arm_thread_state64_get_sp(state);

					rec->stack[depth++] = arm_thread_state64_get_pc(state);
					rec->stack[depth++] = arm_thread_state64_get_lr(state);
					while (depth < SP_DEPTH && known->stack_high > 1 && fp >= sp && fp >= known->stack_low &&
					       fp + 16 <= known->stack_high && (fp & 15) == 0) {
						const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;

						if (frame[1] == 0) break;
						rec->stack[depth++] = frame[1];
						if (frame[0] <= fp) break;
						fp = frame[0];
					}
				}
				thread_resume(threads[t]);
			}
			rec->at = at;
			rec->tid = ident.thread_id;
			rec->cpu_us = (uint32_t)delta;
			rec->state = (uint8_t)basic.run_state;
			rec->depth = (uint8_t)depth;
			sp_rec_next++;
		}
		mach_port_deallocate(mach_task_self(), threads[t]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, n * sizeof(thread_t));
	sp_forget_threads(at);
}

/* ---- names of addresses --------------------------------------------------------------------------- */

struct sp_method {
	uintptr_t start;
	const char *cls, *sel;
	bool meta;
};
static struct {
	const void *base;
	struct sp_method *methods;
	unsigned n;
} sp_images[96];
static unsigned sp_image_count;

static int
sp_method_order(const void *a, const void *b)
{
	uintptr_t x = ((const struct sp_method *)a)->start, y = ((const struct sp_method *)b)->start;

	return x < y ? -1 : x > y;
}

static void
sp_add_methods(struct sp_method **v, unsigned *n, unsigned *size, Class cls, const char *name, bool meta)
{
	unsigned count = 0;
	Method *list = class_copyMethodList(cls, &count);

	for (unsigned m = 0; m < count; m++) {
		if (*n == *size) {
			*size = *size ? *size * 2 : 1024;
			*v = realloc(*v, *size * sizeof(**v));
		}
		(*v)[(*n)++] = (struct sp_method){ (uintptr_t)method_getImplementation(list[m]), name, sel_getName(method_getName(list[m])), meta };
	}
	free(list);
}

static unsigned
sp_image_methods(const Dl_info *where)
{
	unsigned i, count = 0, n = 0, size = 0;
	const char **names;
	struct sp_method *v = NULL;

	for (i = 0; i < sp_image_count; i++) {
		if (sp_images[i].base == where->dli_fbase) return i;
	}
	if (sp_image_count == sizeof(sp_images) / sizeof(sp_images[0])) return ~0u;
	names = objc_copyClassNamesForImage(where->dli_fname, &count);
	for (unsigned c = 0; c < count; c++) {
		Class cls = objc_getClass(names[c]);

		if (cls == Nil) continue;
		sp_add_methods(&v, &n, &size, cls, class_getName(cls), false);
		sp_add_methods(&v, &n, &size, object_getClass((id)cls), class_getName(cls), true);
	}
	free(names);
	if (n > 0) qsort(v, n, sizeof(*v), sp_method_order);
	sp_images[sp_image_count].base = where->dli_fbase;
	sp_images[sp_image_count].methods = v;
	sp_images[sp_image_count].n = n;
	return sp_image_count++;
}

/* the start of the function at `address` (0: none known); its name into `name` */
static uintptr_t
sp_name(uintptr_t address, char *name, size_t size)
{
	Dl_info where = { 0 };
	const char *image = "?", *slash;
	uintptr_t start = 0;
	unsigned i;

	if (dladdr((void *)address, &where) == 0 || where.dli_fname == NULL) {
		snprintf(name, size, "%#lx", (unsigned long)address);
		return address;
	}
	image = (slash = strrchr(where.dli_fname, '/')) != NULL ? slash + 1 : where.dli_fname;
	if (where.dli_saddr != NULL) start = (uintptr_t)where.dli_saddr;
	if ((i = sp_image_methods(&where)) != ~0u && sp_images[i].n > 0) {
		struct sp_method *v = sp_images[i].methods;
		unsigned lo = 0, hi = sp_images[i].n;

		while (hi - lo > 1) {
			unsigned mid = (lo + hi) / 2;

			if (v[mid].start <= address) lo = mid; else hi = mid;
		}
		if (v[lo].start <= address && v[lo].start >= start) {
			snprintf(name, size, "%s`%c[%s %s]+%#lx", image, v[lo].meta ? '+' : '-', v[lo].cls, v[lo].sel,
				 (unsigned long)(address - v[lo].start));
			return v[lo].start;
		}
	}
	if (start != 0 && where.dli_sname != NULL && strcmp(where.dli_sname, "<redacted>") == 0) {
		/* (a function the cache has no name for: where it starts in its image) */
		snprintf(name, size, "%s`@%#lx+%#lx", image, (unsigned long)(start - (uintptr_t)where.dli_fbase),
			 (unsigned long)(address - start));
		return start;
	}
	if (start != 0) {
		snprintf(name, size, "%s`%s+%#lx", image, where.dli_sname, (unsigned long)(address - start));
		return start;
	}
	snprintf(name, size, "%s+%#lx", image, (unsigned long)(address - (uintptr_t)where.dli_fbase));
	return address;
}

/* the name of a function at an address, for others (layerdiff.c) */
void
stallprof_name(uintptr_t address, char *name, size_t size)
{
	pthread_mutex_lock(&sp_name_lock);
	sp_name(address, name, size);
	pthread_mutex_unlock(&sp_name_lock);
}

/* ---- what a window of time had ---------------------------------------------------------------------- */

struct sp_stack {
	uint64_t tid;
	unsigned count, cpu_us, depth;
	uintptr_t key[SP_DEPTH];        /* the functions' starts */
	uint64_t example[SP_DEPTH];     /* one look's addresses */
};

static int
sp_stack_order(const void *a, const void *b)
{
	const struct sp_stack *x = a, *y = b;

	if (x->tid != y->tid) return x->tid < y->tid ? -1 : 1;
	return (int)y->count - (int)x->count;
}

static struct sp_stack sp_stacks[2048];

/* the main thread's looks, by function: in how many of them it was on the stack (inclusive), and on top (self) */
static struct sp_func {
	uintptr_t start, example;
	unsigned in, self;
} sp_funcs[4096];
static unsigned sp_nfuncs;

static struct sp_func *
sp_func_of(uintptr_t start, uintptr_t example)
{
	for (unsigned i = (unsigned)((start >> 2) * 0x9e3779b97f4a7c15ull >> 52) % 4096, probes = 0; probes < 4096; i = (i + 1) % 4096, probes++) {
		if (sp_funcs[i].start == start) return &sp_funcs[i];
		if (sp_funcs[i].start == 0) {
			sp_funcs[i] = (struct sp_func){ start, example, 0, 0 };
			sp_nfuncs++;
			return &sp_funcs[i];
		}
	}
	return NULL;
}

static int
sp_func_order(const void *a, const void *b)
{
	return (int)((const struct sp_func *)b)->in - (int)((const struct sp_func *)a)->in;
}

static void
sp_report(uint64_t from, uint64_t to, const char *why)
{
	static struct { uint64_t tid; unsigned looks, cpu_us; } per[SP_THREADS];
	unsigned threads = 0, stacks = 0, looks = 0, all_cpu = 0, main_running = 0;
	uint64_t last_at = 0, began = sp_now_us();
	bool around = strcmp(why, "pre") == 0 || strcmp(why, "post") == 0;
	char line[1400];
	int at;

	memset(sp_funcs, 0, sizeof(sp_funcs));
	sp_nfuncs = 0;

	for (unsigned k = sp_rec_next > SP_RECS ? sp_rec_next - SP_RECS : 0; k < sp_rec_next; k++) {
		struct sp_rec *rec = &sp_recs[k % SP_RECS];
		unsigned p, s;
		uintptr_t key[SP_DEPTH];

		if (rec->at < from || rec->at > to) continue;
		if (rec->at != last_at) looks++, last_at = rec->at;
		/* (around a gap only what ran: the code that moved things before it and after) */
		if (around && rec->state != TH_STATE_RUNNING && rec->cpu_us == 0) continue;
		for (p = 0; p < threads && per[p].tid != rec->tid; p++) {}
		if (p == threads) {
			if (threads == SP_THREADS) continue;
			per[threads++] = (typeof(per[0])){ rec->tid, 0, 0 };
		}
		per[p].looks++;
		per[p].cpu_us += rec->cpu_us;
		all_cpu += rec->cpu_us;
		for (unsigned d = 0; d < rec->depth; d++) {
			char ignored[8];

			key[d] = d == 0 ? sp_name((uintptr_t)rec->stack[d], ignored, sizeof(ignored)) :
				 sp_name((uintptr_t)rec->stack[d] - 1, ignored, sizeof(ignored));
		}
		if (rec->tid == sp_main_tid && (rec->state == TH_STATE_RUNNING || rec->cpu_us > 0)) {
			main_running++;
			for (unsigned d = 0; d < rec->depth; d++) {
				struct sp_func *f = sp_func_of(key[d], (uintptr_t)rec->stack[d] - (d > 0));
				bool again = false;

				for (unsigned e = 0; e < d; e++) again |= key[e] == key[d];
				if (f != NULL && !again) f->in++;
				if (f != NULL && d == 0) f->self++;
			}
		}
		for (s = 0; s < stacks; s++) {
			if (sp_stacks[s].tid == rec->tid && sp_stacks[s].depth == rec->depth &&
			    memcmp(sp_stacks[s].key, key, rec->depth * sizeof(key[0])) == 0) break;
		}
		if (s == stacks) {
			if (stacks == sizeof(sp_stacks) / sizeof(sp_stacks[0])) continue;
			sp_stacks[s].tid = rec->tid;
			sp_stacks[s].count = sp_stacks[s].cpu_us = 0;
			sp_stacks[s].depth = rec->depth;
			memcpy(sp_stacks[s].key, key, rec->depth * sizeof(key[0]));
			memcpy(sp_stacks[s].example, rec->stack, rec->depth * sizeof(rec->stack[0]));
			stacks++;
		}
		sp_stacks[s].count++;
		sp_stacks[s].cpu_us += rec->cpu_us;
	}
	/* the threads, the busiest first */
	for (unsigned a = 0; a < threads; a++) {
		for (unsigned b = a + 1; b < threads; b++) {
			if (per[b].cpu_us > per[a].cpu_us) {
				typeof(per[0]) swap = per[a];
				per[a] = per[b], per[b] = swap;
			}
		}
	}
	{
		unsigned main_cpu = 0;
		uint64_t span = to > from ? to - from : 1;

		for (unsigned p = 0; p < threads; p++) {
			if (per[p].tid == sp_main_tid) main_cpu = per[p].cpu_us;
		}
		at = snprintf(line, sizeof(line), "stall: %s %llu ms at %llu..%llu: %u looks; SpringBoard %llu%% (main %llu%%), its threads:",
			      why, (to - from) / 1000, from, to, looks, all_cpu * 100ull / span, main_cpu * 100ull / span);
		for (unsigned p = 0; p < threads && p < 8 && at < (int)sizeof(line) - 60; p++) {
			struct sp_thread *t = sp_thread_of(per[p].tid, false);

			at += snprintf(line + at, sizeof(line) - (size_t)at, " %s#%llx %llu%%",
				       t && t->name[0] ? t->name : per[p].tid == sp_main_tid ? "main" : "-",
				       per[p].tid & 0xffff, per[p].cpu_us * 100ull / span);
		}
		sp_say("%s\n", line);
	}
	/* the main thread's time by function, the most first: a line of where it was (inclusive), one of what it did (self) */
	if (main_running > 0) {
		unsigned n = 0;

		for (unsigned i = 0; i < 4096; i++) {
			if (sp_funcs[i].start != 0) sp_funcs[n++] = sp_funcs[i];
		}
		qsort(sp_funcs, n, sizeof(sp_funcs[0]), sp_func_order);
		for (unsigned part = 0; part < 3; part++) {
			at = snprintf(line, sizeof(line), "stall:   main running %u looks, in%s:", main_running, part ? " (more)" : "");
			for (unsigned i = part * 14; i < n && i < part * 14 + 14 && at < (int)sizeof(line) - 160; i++) {
				char name[140];

				if (sp_funcs[i].in * 20 < main_running) break;          /* under 5% */
				sp_name(sp_funcs[i].example, name, sizeof(name));
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %u%% %s;", sp_funcs[i].in * 100 / main_running, name);
			}
			sp_say("%s\n", line);
		}
		for (unsigned a = 0; a < n; a++) {
			for (unsigned b = a + 1; b < n; b++) {
				if (sp_funcs[b].self > sp_funcs[a].self) {
					struct sp_func swap = sp_funcs[a];
					sp_funcs[a] = sp_funcs[b], sp_funcs[b] = swap;
				}
			}
		}
		at = snprintf(line, sizeof(line), "stall:   main running, on top:");
		for (unsigned i = 0; i < n && i < 12 && sp_funcs[i].self > 0 && at < (int)sizeof(line) - 160; i++) {
			char name[140];

			sp_name(sp_funcs[i].example, name, sizeof(name));
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u%% %s;", sp_funcs[i].self * 100 / main_running, name);
		}
		sp_say("%s\n", line);
	}
	/* the main thread's stacks and those of any thread that had 3% or more, the most frequent first */
	qsort(sp_stacks, stacks, sizeof(sp_stacks[0]), sp_stack_order);
	for (unsigned s = 0, shown = 0; s < stacks; s++) {
		struct sp_thread *t = sp_thread_of(sp_stacks[s].tid, false);
		unsigned p;

		if (s == 0 || sp_stacks[s].tid != sp_stacks[s - 1].tid) shown = 0;
		for (p = 0; p < threads && per[p].tid != sp_stacks[s].tid; p++) {}
		if (p == threads || ++shown > (around ? 8u : 4u)) continue;
		if (sp_stacks[s].tid != sp_main_tid && per[p].cpu_us * 100ull < 3ull * (to - from)) continue;
		at = snprintf(line, sizeof(line), "stall:   %s#%llx %u/%u cpu %u ms:",
			      t && t->name[0] ? t->name : sp_stacks[s].tid == sp_main_tid ? "main" : "-",
			      sp_stacks[s].tid & 0xffff, sp_stacks[s].count, per[p].looks, sp_stacks[s].cpu_us / 1000);
		for (unsigned d = 0; d < sp_stacks[s].depth && at < (int)sizeof(line) - 100; d++) {
			char name[200];

			sp_name((uintptr_t)sp_stacks[s].example[d] - (d > 0), name, sizeof(name));
			at += snprintf(line + at, sizeof(line) - (size_t)at, "%s %s", d ? " <" : "", name);
		}
		sp_say("%s\n", line);
	}
	/* every process's time over the window: the looks just before it and just after */
	{
		struct sp_snap *first = NULL, *last = NULL;

		for (unsigned k = sp_snap_next > SP_SNAPS ? sp_snap_next - SP_SNAPS : 0; k < sp_snap_next; k++) {
			struct sp_snap *snap = &sp_snaps[k % SP_SNAPS];

			if (snap->at <= from) first = snap;
			if (snap->at >= to && last == NULL) last = snap;
		}
		if (first != NULL && last != NULL && last->at > first->at) {
			static struct { int pid; uint64_t time; } busy[SP_PROCS];
			unsigned nbusy = 0;
			uint64_t span = (last->at - first->at) * 1000, all = 0;

			for (unsigned i = 0; i < last->n; i++) {
				uint64_t before = 0;

				for (unsigned j = 0; j < first->n; j++) {
					if (first->pid[j] == last->pid[i]) before = first->time[j];
				}
				if (last->time[i] > before) {
					uint64_t ns = (last->time[i] - before) * sp_timebase.numer / sp_timebase.denom;

					busy[nbusy].pid = last->pid[i];
					busy[nbusy++].time = ns;
					all += ns;
				}
			}
			for (unsigned a = 0; a < nbusy; a++) {
				for (unsigned b = a + 1; b < nbusy; b++) {
					if (busy[b].time > busy[a].time) {
						typeof(busy[0]) swap = busy[a];
						busy[a] = busy[b], busy[b] = swap;
					}
				}
			}
			at = snprintf(line, sizeof(line), "stall:   processes over %llu ms: %llu%% in all:",
				      (last->at - first->at) / 1000, all * 100 / span);
			for (unsigned b = 0; b < nbusy && b < 12 && at < (int)sizeof(line) - 60; b++) {
				char pname[64] = "?";

				proc_name(busy[b].pid, pname, sizeof(pname));
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %s[%d] %llu%%", busy[b].pid == 0 ? "kernel_task" : pname,
					       busy[b].pid, busy[b].time * 100 / span);
			}
			sp_say("%s\n", line);
		}
	}
	sp_say("stall:   (reported in %llu ms)\n", (sp_now_us() - began) / 1000);
}

static void *
sp_main(__unused void *argument)
{
	unsigned looks = 0;

	uint64_t cost_threads = 0, cost_procs = 0, cost_since = sp_now_us();
	unsigned cost_looks = 0;

	pthread_setname_np("hakios.stallprof");
	for (;;) {
		uint64_t t0, t1, t2;

		usleep(sp_period_us);
		t0 = sp_now_us();
		sp_look();
		t1 = sp_now_us();
		if (looks++ % SP_PROC_EVERY == 0) sp_look_at_processes(t1);
		t2 = sp_now_us();
		cost_threads += t1 - t0, cost_procs += t2 - t1, cost_looks++;
		/* what looking costs (it is SpringBoard's time too) */
		if (t2 - cost_since >= 20000000) {
			sp_say("stall: cost: %u looks in %llu ms (one every %llu us), threads %llu us, processes %llu us each\n",
			       cost_looks, (t2 - cost_since) / 1000, (t2 - cost_since) / cost_looks, cost_threads / cost_looks,
			       cost_procs * SP_PROC_EVERY / cost_looks);
			cost_threads = cost_procs = 0, cost_looks = 0, cost_since = t2;
		}
		while (sp_window_got != sp_window_put) {
			unsigned w = sp_window_got % SP_WINDOWS;

			/* (the looks just after its end too: wait for them) */
			if (sp_now_us() < sp_windows[w].to + 3 * sp_period_us + 15000) break;
			pthread_mutex_lock(&sp_name_lock);
			sp_report(sp_windows[w].from, sp_windows[w].to, sp_windows[w].why);
			pthread_mutex_unlock(&sp_name_lock);
			sp_window_got++;
		}
	}
	return NULL;
}

void
stallprof_window(uint64_t from_us, uint64_t to_us, const char *why)
{
	if (sp_period_us == 0) return;
	pthread_mutex_lock(&sp_window_lock);
	if (sp_window_put - sp_window_got < SP_WINDOWS) {
		unsigned w = sp_window_put % SP_WINDOWS;

		sp_windows[w].from = from_us;
		sp_windows[w].to = to_us;
		strlcpy(sp_windows[w].why, why, sizeof(sp_windows[w].why));
		sp_window_put++;
	}
	/* ... and for a gap, what moved things just before it and what started them again */
	if (strcmp(why, "gap") == 0 && sp_window_put - sp_window_got + 2 <= SP_WINDOWS) {
		unsigned w = sp_window_put % SP_WINDOWS;

		sp_windows[w].from = from_us - 200000, sp_windows[w].to = from_us;
		strlcpy(sp_windows[w].why, "pre", sizeof(sp_windows[w].why));
		w = ++sp_window_put % SP_WINDOWS;
		sp_windows[w].from = to_us, sp_windows[w].to = to_us + 150000;
		strlcpy(sp_windows[w].why, "post", sizeof(sp_windows[w].why));
		sp_window_put++;
	}
	pthread_mutex_unlock(&sp_window_lock);
}

/* (on the main thread: it is the one called "main") */
void
stallprof_start(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *at;
	pthread_t sampler;
	pthread_attr_t attr;
	struct sched_param param = { .sched_priority = 60 };

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (at = strstr(args, "sb_prof=")) == NULL ||
	    (at != args && at[-1] != ' ')) return;
	sp_period_us = (unsigned)atoi(at + 8);
	if (sp_period_us < 1000) sp_period_us = sp_period_us ? 1000 : 0;
	if (sp_period_us == 0) return;
	mach_timebase_info(&sp_timebase);
	{
		thread_identifier_info_data_t ident;
		mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
		thread_t me = mach_thread_self();

		if (thread_info(me, THREAD_IDENTIFIER_INFO, (thread_info_t)&ident, &count) == KERN_SUCCESS) sp_main_tid = ident.thread_id;
		mach_port_deallocate(mach_task_self(), me);
	}
	pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	pthread_attr_setschedparam(&attr, &param);
	pthread_create(&sampler, &attr, sp_main, NULL);
	sp_say("stall: every thread of SpringBoard looked at every %u us\n", sp_period_us);
}
