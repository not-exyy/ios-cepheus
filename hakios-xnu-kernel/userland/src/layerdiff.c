/*
 * (hakios) boot-arg sb_diff=1, with touch_lat=1: what each turn of SpringBoard's main run loop changed on the screen,
 * looked at just before CoreAnimation's commit (inject_sb.c calls layerdiff_turn): every layer of the visible windows
 * (not under a hidden one) is compared with what it was at the turn before, and a turn that changed something gives
 *
 *   ld: US N changed: OWNER.what old>new; ...; +KEY(CLASS keyPath P, 0.35 s, begins +0.250 s) ...
 *
 * OWNER is the class of the layer's view (or of the layer, and its name); "what" is pos, size, origin (of the
 * bounds: a scroll view's offset), alpha, hidden, xform (the transform), contents, new (a layer not seen before);
 * +KEY an animation added to it, with when it begins against now -- a delayed one shows up as begins +X.
 * Costs a few milliseconds a turn: a diagnostic only.
 */
#include <dlfcn.h>
#include <execinfo.h>
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
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

#define LD_SEND(type, object, name)        ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))
#define LD_SEND1(type, object, name, argument_type, argument) \
	((type (*)(id, SEL, argument_type))objc_msgSend)((id)(object), sel_registerName(name), argument)
#define LD_SLOTS        16384

void layerdiff_start(void);
void stallprof_name(uintptr_t address, char *name, size_t size);
void layerdiff_turn(uint64_t now_us);
extern double CACurrentMediaTime(void);

typedef struct { double x, y; } ld_point;
typedef struct { double x, y, w, h; } ld_rect;
typedef struct { double m[16]; } ld_transform;

static struct ld_layer {
	const void *layer;
	unsigned seen;                  /* the turn that last found it */
	ld_point pos;
	ld_rect bounds;
	float alpha;
	bool hidden;
	uint64_t xform;
	const void *contents;
	long animations;
} ld_table[LD_SLOTS];
static unsigned ld_turn, ld_count;
static bool ld_on;
static char ld_line[1400];
static int ld_at, ld_changes;

static struct ld_layer *
ld_slot(const void *layer)
{
	for (unsigned i = (unsigned)(((uintptr_t)layer >> 4) * 0x9e3779b97f4a7c15ull >> 50) % LD_SLOTS, probes = 0; probes < LD_SLOTS;
	     i = (i + 1) % LD_SLOTS, probes++) {
		if (ld_table[i].layer == layer || ld_table[i].layer == NULL) return &ld_table[i];
	}
	return NULL;
}

static void
ld_add(const char *format, ...)
{
	va_list ap;

	if (ld_at > (int)sizeof(ld_line) - 120) return;
	va_start(ap, format);
	ld_at += vsnprintf(ld_line + ld_at, sizeof(ld_line) - (size_t)ld_at, format, ap);
	va_end(ap);
}

static void
ld_owner(id layer, char *out, size_t size)
{
	id delegate = LD_SEND(id, layer, "delegate");
	id name = LD_SEND(id, layer, "name");
	const char *text = name ? LD_SEND(const char *, name, "UTF8String") : NULL;

	snprintf(out, size, "%s%s%s", delegate ? object_getClassName(delegate) : object_getClassName(layer), text ? "/" : "",
		 text ? text : "");
}

static void
ld_change(id layer, const char *format, ...)
{
	char owner[96];
	va_list ap;

	ld_changes++;
	if (ld_at > (int)sizeof(ld_line) - 160) return;
	ld_owner(layer, owner, sizeof(owner));
	ld_at += snprintf(ld_line + ld_at, sizeof(ld_line) - (size_t)ld_at, " %s.", owner);
	va_start(ap, format);
	ld_at += vsnprintf(ld_line + ld_at, sizeof(ld_line) - (size_t)ld_at, format, ap);
	va_end(ap);
	ld_at += snprintf(ld_line + ld_at, sizeof(ld_line) - (size_t)ld_at, ";");
}

static void
ld_animations(id layer, long before)
{
	id keys = LD_SEND(id, layer, "animationKeys");
	long n = keys ? LD_SEND(long, keys, "count") : 0;
	double now = CACurrentMediaTime();

	for (long i = before > 0 && before < n ? before : 0; i < n; i++) {
		id key = LD_SEND1(id, keys, "objectAtIndex:", long, i);
		id animation = LD_SEND1(id, layer, "animationForKey:", id, key);
		id path = animation && class_respondsToSelector(object_getClass(animation), sel_registerName("keyPath")) ?
			  LD_SEND(id, animation, "keyPath") : NULL;
		double begin = animation ? LD_SEND(double, animation, "beginTime") : 0;

		ld_change(layer, "+%s(%s %s, %.3f s, begins %+.3f s)", key ? LD_SEND(const char *, key, "UTF8String") : "?",
			  animation ? object_getClassName(animation) : "?", path ? LD_SEND(const char *, path, "UTF8String") : "-",
			  animation ? LD_SEND(double, animation, "duration") : 0, begin > 0 ? begin - now : 0);
	}
}

static void
ld_walk(id layer, int depth)
{
	struct ld_layer *s, was;
	id sublayers;
	ld_transform t;
	uint64_t hash = 0;
	bool fresh;

	if (layer == NULL || depth > 60 || (s = ld_slot((const void *)layer)) == NULL) return;
	fresh = s->layer == NULL;
	if (fresh) {
		if (ld_count > LD_SLOTS * 3 / 4) return;
		ld_count++;
	}
	was = *s;
	s->layer = (const void *)layer;
	s->seen = ld_turn;
	s->pos = LD_SEND(ld_point, layer, "position");
	s->bounds = LD_SEND(ld_rect, layer, "bounds");
	s->alpha = LD_SEND(float, layer, "opacity");
	s->hidden = LD_SEND(BOOL, layer, "isHidden");
	t = LD_SEND(ld_transform, layer, "transform");
	for (int i = 0; i < 16; i++) {
		uint64_t bits;

		memcpy(&bits, &t.m[i], sizeof(bits));
		hash = (hash ^ bits) * 0x100000001b3ull;
	}
	s->xform = hash;
	s->contents = (const void *)LD_SEND(id, layer, "contents");
	{
		id keys = LD_SEND(id, layer, "animationKeys");

		s->animations = keys ? LD_SEND(long, keys, "count") : 0;
	}
	if (ld_turn > 1) {
		if (fresh || was.seen + 1 < ld_turn) {
			ld_change(layer, "new %gx%g at %g,%g alpha %g%s", s->bounds.w, s->bounds.h, s->pos.x, s->pos.y, (double)s->alpha,
				  s->hidden ? " hidden" : "");
		} else {
			if (was.pos.x != s->pos.x || was.pos.y != s->pos.y)
				ld_change(layer, "pos %.1f,%.1f>%.1f,%.1f", was.pos.x, was.pos.y, s->pos.x, s->pos.y);
			if (was.bounds.w != s->bounds.w || was.bounds.h != s->bounds.h)
				ld_change(layer, "size %.1fx%.1f>%.1fx%.1f", was.bounds.w, was.bounds.h, s->bounds.w, s->bounds.h);
			if (was.bounds.x != s->bounds.x || was.bounds.y != s->bounds.y)
				ld_change(layer, "origin %.1f,%.1f>%.1f,%.1f", was.bounds.x, was.bounds.y, s->bounds.x, s->bounds.y);
			if (was.alpha != s->alpha) ld_change(layer, "alpha %.3f>%.3f", (double)was.alpha, (double)s->alpha);
			if (was.hidden != s->hidden) ld_change(layer, "hidden %d>%d", was.hidden, s->hidden);
			if (was.xform != s->xform) ld_change(layer, "xform m11 %.3f m41 %.1f m42 %.1f", t.m[0], t.m[12], t.m[13]);
			if (was.contents != s->contents) ld_change(layer, "contents");
		}
		if (s->animations > (fresh ? 0 : was.animations)) ld_animations(layer, fresh ? 0 : was.animations);
	}
	if (s->hidden) return;
	sublayers = LD_SEND(id, layer, "sublayers");
	for (long i = 0, n = sublayers ? LD_SEND(long, sublayers, "count") : 0; i < n; i++) {
		ld_walk(LD_SEND1(id, sublayers, "objectAtIndex:", long, i), depth + 1);
	}
}

/* (the table fills with layers that are gone: drop those not seen for a while) */
static void
ld_forget(void)
{
	static struct ld_layer keep[LD_SLOTS];
	unsigned n = 0;

	for (unsigned i = 0; i < LD_SLOTS; i++) {
		if (ld_table[i].layer != NULL && ld_table[i].seen + 120 > ld_turn) keep[n++] = ld_table[i];
	}
	memset(ld_table, 0, sizeof(ld_table));
	ld_count = 0;
	for (unsigned i = 0; i < n; i++) {
		struct ld_layer *s = ld_slot(keep[i].layer);

		*s = keep[i];
		ld_count++;
	}
}

void
layerdiff_turn(uint64_t now_us)
{
	Class window = objc_getClass("UIWindow");
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	id windows;

	if (!ld_on || window == Nil || !class_getClassMethod(window, all)) return;
	windows = ((id (*)(Class, SEL, BOOL, BOOL))objc_msgSend)(window, all, 1, 1);
	ld_turn++;
	if (ld_count > LD_SLOTS / 2) ld_forget();
	ld_at = 0, ld_changes = 0;
	for (long i = 0, n = windows ? LD_SEND(long, windows, "count") : 0; i < n; i++) {
		ld_walk(LD_SEND(id, LD_SEND1(id, windows, "objectAtIndex:", long, i), "layer"), 0);
	}
	if (ld_changes > 0) {
		static char out[sizeof(ld_line) + 64];
		int n = snprintf(out, sizeof(out) - 1, "ld: %llu %d changed:%.*s", now_us, ld_changes, ld_at, ld_line);

		if (n > (int)sizeof(out) - 2) n = (int)sizeof(out) - 2;
		out[n++] = '\n';
		write(2, out, (size_t)n);
	}
}

/*
 * Boot-arg sb_methods=CLASS,CLASS...: every method of those classes into the log, with where its code is
 * ("lm: -[CLASS selector] IMAGE+OFFSET"): which of them to look at or replace.
 */
static void
ld_methods(const char *list)
{
	char names[512];
	char *rest = names, *name;

	strlcpy(names, list, sizeof(names));
	if ((name = strchr(names, ' ')) != NULL) *name = 0;
	while ((name = strsep(&rest, ",")) != NULL) {
		Class cls = objc_getClass(name);

		for (int meta = 0; cls != Nil && meta < 2; meta++) {
			unsigned count = 0;
			Method *list = class_copyMethodList(meta ? object_getClass((id)cls) : cls, &count);

			for (unsigned m = 0; m < count; m++) {
				Dl_info where = { 0 };
				uintptr_t imp = (uintptr_t)method_getImplementation(list[m]);
				char line[400];

				dladdr((void *)imp, &where);
				int n = snprintf(line, sizeof(line), "lm: %c[%s %s] %s+%#lx\n", meta ? '+' : '-', name, sel_getName(method_getName(list[m])),
						 where.dli_fname ? strrchr(where.dli_fname, '/') + 1 : "?", (unsigned long)(imp - (uintptr_t)where.dli_fbase));
				write(2, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
			}
			free(list);
		}
		if (cls == Nil) dprintf(2, "lm: no class %s\n", name);
	}
}

/*
 * Boot-arg sb_contents=1: a big picture (a million pixels or more) given to a layer as its contents --
 * when, how big, what kind of file it came from (ImageIO's type: decoded at CoreAnimation's commit, on
 * the main thread), whose layer, and from where:
 *   lc: US WxH TYPE OWNER: caller < caller ...
 */
static void (*ld_original_set_contents)(id, SEL, id);
static unsigned long (*ld_image_type)(void);
static size_t (*ld_image_width)(const void *), (*ld_image_height)(const void *);
static const void *(*ld_image_uttype)(const void *);
extern uint64_t layerdiff_now_us(void);

static void
ld_set_contents(id layer, SEL selector, id contents)
{
	const char *kind = contents != NULL ? object_getClassName(contents) : "";

	/* (CFGetTypeID only of what is a CF object: a CGImage's class is __NSCFType, or one named for it) */
	if (contents != NULL && (strcmp(kind, "__NSCFType") == 0 || strstr(kind, "CGImage") != NULL) &&
	    CFGetTypeID((CFTypeRef)contents) == ld_image_type() &&
	    ld_image_width((const void *)contents) * ld_image_height((const void *)contents) >= 1000000) {
		void *frames[24];
		int n = backtrace(frames, 24), at;
		char line[1400], owner[96];
		const void *type = ld_image_uttype ? ld_image_uttype((const void *)contents) : NULL;
		char type_text[64] = "-";

		if (type != NULL) CFStringGetCString((CFStringRef)type, type_text, sizeof(type_text), kCFStringEncodingUTF8);
		ld_owner(layer, owner, sizeof(owner));
		at = snprintf(line, sizeof(line), "lc: %llu %zux%zu %s %s:", layerdiff_now_us(), ld_image_width((const void *)contents),
			      ld_image_height((const void *)contents), type_text, owner);
		for (int i = 1; i < n && at < (int)sizeof(line) - 160; i++) {
			char name[150];

			stallprof_name((uintptr_t)frames[i] - 1, name, sizeof(name));
			at += snprintf(line + at, sizeof(line) - (size_t)at, "%s %s", i > 1 ? " <" : "", name);
		}
		line[at < (int)sizeof(line) - 1 ? at : (int)sizeof(line) - 2] = '\n';
		write(2, line, (size_t)(at < (int)sizeof(line) - 1 ? at + 1 : (int)sizeof(line) - 1));
	}
	ld_original_set_contents(layer, selector, contents);
}

/*
 * Boot-arg sb_trace=CLASS:SELECTOR,CLASS:+SELECTOR...: a line for every call of those methods -- when, on which
 * thread, how long it took, what it gave back and who called it:
 *   lt: US main|THREAD -[CLASS SELECTOR](ARG1 ARG2) took US -> RESULT: caller < caller ...
 * Only for methods whose arguments (up to four) and result are pointers or integers (x0-x5 are passed on as
 * they are): no floating point, no structures.
 */
#define LT_MAX  24
static struct { SEL sel; Class cls; IMP original; char what[96]; } lt_methods[LT_MAX];
static unsigned lt_count;
static pthread_t lt_main;

static uintptr_t
lt_wrapper(id self, SEL cmd, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
	unsigned i;
	Class cls = object_getClass(self);
	uint64_t began = layerdiff_now_us(), took;
	uintptr_t result;

	for (i = 0; i < lt_count; i++) {
		if (lt_methods[i].sel != cmd) continue;
		/* (the method of this class, or of a superclass of it) */
		for (Class k = cls; k != Nil; k = class_getSuperclass(k)) {
			if (k == lt_methods[i].cls) goto found;
		}
	}
	return 0;
found:
	result = ((uintptr_t (*)(id, SEL, uintptr_t, uintptr_t, uintptr_t, uintptr_t))lt_methods[i].original)(self, cmd, a, b, c, d);
	took = layerdiff_now_us() - began;
	{
		void *frames[12];
		int n = backtrace(frames, 12), at;
		char line[1400], thread[32] = "main";

		if (!pthread_equal(pthread_self(), lt_main)) {
			pthread_getname_np(pthread_self(), thread, sizeof(thread));
			if (thread[0] == 0) snprintf(thread, sizeof(thread), "thread-%p", (void *)pthread_self());
		}
		at = snprintf(line, sizeof(line), "lt: %llu %s %s(%#lx %#lx) took %llu -> %#lx:", began, thread, lt_methods[i].what,
			      (unsigned long)a, (unsigned long)b, took, (unsigned long)result);
		for (int f = 1; f < n && at < (int)sizeof(line) - 160; f++) {
			char name[150];

			stallprof_name((uintptr_t)frames[f] - 1, name, sizeof(name));
			at += snprintf(line + at, sizeof(line) - (size_t)at, "%s %s", f > 1 ? " <" : "", name);
		}
		if (at > (int)sizeof(line) - 2) at = (int)sizeof(line) - 2;
		line[at++] = '\n';
		write(2, line, (size_t)at);
	}
	return result;
}

static void
lt_start(const char *list)
{
	char names[700];
	char *rest = names, *item;

	lt_main = pthread_self();
	strlcpy(names, list, sizeof(names));
	if ((item = strchr(names, ' ')) != NULL) *item = 0;
	while ((item = strsep(&rest, ",")) != NULL && lt_count < LT_MAX) {
		char *colon = strchr(item, ':');
		bool meta;
		Class cls;
		Method method;

		if (colon == NULL) continue;
		*colon++ = 0;
		meta = *colon == '+';
		if (meta) colon++;
		/* (the selector's colons are written as dots on the kernel's command line: sb_trace=C:_cacheImage.) */
		for (char *dot = colon; *dot; dot++) if (*dot == '.') *dot = ':';
		cls = objc_getClass(item);
		if (cls != Nil && meta) cls = object_getClass((id)cls);
		method = cls != Nil ? class_getInstanceMethod(cls, sel_registerName(colon)) : NULL;
		if (method == NULL) {
			dprintf(2, "lt: no %c[%s %s]\n", meta ? '+' : '-', item, colon);
			continue;
		}
		lt_methods[lt_count].sel = sel_registerName(colon);
		lt_methods[lt_count].cls = cls;
		snprintf(lt_methods[lt_count].what, sizeof(lt_methods[lt_count].what), "%c[%s %s]", meta ? '+' : '-', item, colon);
		lt_methods[lt_count].original = method_setImplementation(method, (IMP)lt_wrapper);
		dprintf(2, "lt: %s traced\n", lt_methods[lt_count].what);
		lt_count++;
	}
}

void
layerdiff_start(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *at;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "sb_diff=1")) != NULL &&
	    (at == args || at[-1] == ' ')) {
		ld_on = true;
		write(2, "ld: what each turn changes, written down (sb_diff=1)\n", 53);
	}
	if ((at = strstr(args, "sb_trace=")) != NULL && (at == args || at[-1] == ' ')) {
		lt_start(at + 9);
	}
	if ((at = strstr(args, "sb_methods=")) != NULL && (at == args || at[-1] == ' ')) {
		ld_methods(at + 11);
	}
	if ((at = strstr(args, "sb_contents=1")) != NULL && (at == args || at[-1] == ' ')) {
		Method method = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("setContents:"));

		ld_image_type = (unsigned long (*)(void))dlsym(RTLD_DEFAULT, "CGImageGetTypeID");
		ld_image_width = (size_t (*)(const void *))dlsym(RTLD_DEFAULT, "CGImageGetWidth");
		ld_image_height = (size_t (*)(const void *))dlsym(RTLD_DEFAULT, "CGImageGetHeight");
		ld_image_uttype = (const void *(*)(const void *))dlsym(RTLD_DEFAULT, "CGImageGetUTType");
		if (method != NULL && ld_image_type && ld_image_width && ld_image_height) {
			ld_original_set_contents = (void (*)(id, SEL, id))method_setImplementation(method, (IMP)ld_set_contents);
			write(2, "lc: big pictures given to layers, written down (sb_contents=1)\n", 63);
		}
	}
}
