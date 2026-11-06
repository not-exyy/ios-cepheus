/*
 * The picture of an application that SpringBoard zooms into the screen when it opens the application, decoded
 * before it is needed (part of libhakios.sb.dylib; boot-arg sb_predecode=0 turns it off).
 *
 * The picture SpringBoard keeps of an application is a JPEG here (inject_sb.c, snapshot_output_format: an ASTC
 * texture, what an iPhone keeps, takes seconds of a core to make without Apple's vector code). The GPU takes an
 * ASTC texture as it is; a JPEG has to be decoded first, and CoreAnimation does that lazily -- at the commit of
 * the turn of SpringBoard's main run loop that opens the application, on the main thread: 30-55 ms of the
 * 120-140 ms between the finger leaving the icon and the zoom's first frame (logs/120113-run.log, sb_prof=1000:
 * AppleJPEG under CA's commit; logs/120728-run.log, sb_contents=1: -[SBFullscreenZoomView initWith...snapshot:]
 * gives a UIImageView a 1125x2436 public.jpeg). SplashBoard does keep the picture it has just made
 * (-[XBApplicationSnapshot _cacheImage:], after the home button), but lets it go before the application is
 * opened again, and then -[XBApplicationSnapshot imageForInterfaceOrientation:generationOptions:] reads the file
 * anew (a millisecond) and hands over a picture not decoded yet (logs/121215-run.log, sb_trace=).
 *
 * So: when SplashBoard has made a picture (_cacheImage: off the main thread) and when SpringBoard warms one up
 * (loadImageForPreHeat, on its own queue, as the finger goes down on an icon), the JPEG file is decoded here on a
 * queue of low priority into a plain bitmap; and when imageForInterfaceOrientation:generationOptions: hands over
 * a JPEG picture of a file that was decoded so -- the same path, size and modification time, the same pixels --
 * the bitmap goes instead. A few of them are kept (SNAP_KEEP, SNAP_BYTES: 11 MB for a full-screen one), the
 * last used last to go. Anything unexpected (another kind of image, other dimensions, a file changed since)
 * leaves SplashBoard's own picture in place.
 * With touch_lat=1 the log says what happened: "snap: decoded ...", "snap: given decoded ...", "snap: not decoded ...".
 *
 * The bitmap is an IOSurface's memory (CoreGraphics' CGImageCreateFromIOSurface; boot-arg sb_snapsurface=0: plain memory).
 * A picture in a program's own memory reaches the window server as a surface CoreAnimation makes around that memory
 * (MiniIOSurface's methodWrapClientImage), which the GPU cannot take as it is (hakios.gpu.cpp, surfaceBuffer: not in
 * whole pages, rows of any length): the window server copied all 11 MB of it into a texture, a glTexImage2D of 10 ms
 * in the zoom's second frame, 24-27 ms of drawing (logs/105430-run.log, glslow=3000). A surface the kernel made is
 * memory the GPU draws with as it is (eagl.c, surface_shared()): the window server's frames of the zoom's first
 * 100 ms 7.3 ms on average instead of 10.9, the longest 15.9 instead of 24.6 (logs/105944-run.log). The pause before
 * the zoom is not shorter: it is SpringBoard's own turn (75-80 ms) and the panel's next beat.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurfaceRef.h>
#include <ImageIO/ImageIO.h>
#include <dirent.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define SNAP_KEEP       6
#define SNAP_BYTES      (72u << 20)
#define SNAP_ZOOM_PIXELS 2000000        /* 1125x2436 is 2.7 million */

void snapdecode_start(void);

static struct snap_entry {
	char path[1024];
	struct timespec mtime;
	off_t size;
	CGImageRef image;               /* decoded: a bitmap */
	size_t bytes;
	uint64_t used;
} snap_entries[SNAP_KEEP];
static pthread_mutex_t snap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t snap_clock;
static bool snap_say;
static dispatch_queue_t snap_queue;
static dispatch_queue_t snap_soon;      /* the full-screen picture of the application under the finger: wanted in ~60 ms */
static mach_timebase_info_data_t snap_timebase;

static IMP snap_original_image, snap_original_cache, snap_original_preheat;
static bool snap_surfaces = true;       /* boot-arg sb_snapsurface=0: plain memory */
static CGImageRef (*snap_image_of_surface)(IOSurfaceRef surface, CFDictionaryRef options);

#define SEND(type, object, name)        ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))

static uint64_t
snap_ms(void)
{
	return mach_absolute_time() * snap_timebase.numer / snap_timebase.denom / 1000000;
}

static bool
snap_path_of(id snapshot, char *path, size_t size)
{
	id text = snapshot != NULL && class_respondsToSelector(object_getClass(snapshot), sel_registerName("path")) ?
		  SEND(id, snapshot, "path") : NULL;
	const char *c = text ? SEND(const char *, text, "fileSystemRepresentation") : NULL;
	size_t n = c ? strlen(c) : 0;

	if (n < 6 || n >= size || (strcasecmp(c + n - 5, ".jpeg") != 0 && strcasecmp(c + n - 4, ".jpg") != 0)) return false;
	memcpy(path, c, n + 1);
	return true;
}

/* the decoded picture of this file as it is now, retained (or NULL) */
static CGImageRef
snap_lookup(const char *path, const struct stat *st)
{
	CGImageRef found = NULL;

	pthread_mutex_lock(&snap_lock);
	for (int i = 0; i < SNAP_KEEP; i++) {
		struct snap_entry *e = &snap_entries[i];

		if (e->image != NULL && strcmp(e->path, path) == 0 && e->size == st->st_size &&
		    e->mtime.tv_sec == st->st_mtimespec.tv_sec && e->mtime.tv_nsec == st->st_mtimespec.tv_nsec) {
			e->used = ++snap_clock;
			found = CGImageRetain(e->image);
			break;
		}
	}
	pthread_mutex_unlock(&snap_lock);
	return found;
}

static void
snap_store(const char *path, const struct stat *st, CGImageRef image, size_t bytes)
{
	size_t all = bytes;
	struct snap_entry *slot = NULL;

	pthread_mutex_lock(&snap_lock);
	/* the same file's older picture goes; then the least recently used, while there are too many or too much */
	for (int i = 0; i < SNAP_KEEP; i++) {
		if (snap_entries[i].image != NULL && strcmp(snap_entries[i].path, path) == 0) {
			CGImageRelease(snap_entries[i].image);
			snap_entries[i].image = NULL;
		}
	}
	for (;;) {
		int used = 0, oldest = -1, empty = -1;

		all = bytes;
		for (int i = 0; i < SNAP_KEEP; i++) {
			if (snap_entries[i].image == NULL) {
				if (empty < 0) empty = i;
				continue;
			}
			used++;
			all += snap_entries[i].bytes;
			if (oldest < 0 || snap_entries[i].used < snap_entries[oldest].used) oldest = i;
		}
		if (empty >= 0 && all <= SNAP_BYTES) {
			slot = &snap_entries[empty];
			break;
		}
		if (oldest < 0) break;
		CGImageRelease(snap_entries[oldest].image);
		snap_entries[oldest].image = NULL;
		(void)used;
	}
	if (slot != NULL) {
		strlcpy(slot->path, path, sizeof(slot->path));
		slot->mtime = st->st_mtimespec;
		slot->size = st->st_size;
		slot->image = CGImageRetain(image);
		slot->bytes = bytes;
		slot->used = ++snap_clock;
	}
	pthread_mutex_unlock(&snap_lock);
}

static void
snap_set(CFMutableDictionaryRef props, CFStringRef key, long value)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongType, &value);

	CFDictionarySetValue(props, key, n);
	CFRelease(n);
}

/* a surface of w x h blue-first 32-bit pixels, in rows of the length the kernel likes (its own choice) */
static IOSurfaceRef
snap_surface(size_t w, size_t h)
{
	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
								 &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef surface;

	snap_set(props, kIOSurfaceWidth, (long)w);
	snap_set(props, kIOSurfaceHeight, (long)h);
	snap_set(props, kIOSurfaceBytesPerElement, 4);
	snap_set(props, kIOSurfacePixelFormat, 'BGRA');
	surface = IOSurfaceCreate(props);
	CFRelease(props);
	return surface;
}

/* decode the JPEG file at `path` into a bitmap, unless that is done already (on snap_queue) */
static void
snap_decode(const char *path, const char *why, size_t min_pixels)
{
	struct stat st;
	CGImageRef known;
	CFURLRef url;
	CGImageSourceRef source;
	CGImageRef lazy, decoded = NULL;
	uint64_t began = snap_ms();

	if (stat(path, &st) != 0 || st.st_size == 0) return;
	if ((known = snap_lookup(path, &st)) != NULL) {
		CGImageRelease(known);
		return;
	}
	url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	source = url ? CGImageSourceCreateWithURL(url, NULL) : NULL;
	if (url) CFRelease(url);
	if (source == NULL) return;
	lazy = CGImageSourceGetCount(source) > 0 ? CGImageSourceCreateImageAtIndex(source, 0, NULL) : NULL;
	CFRelease(source);
	if (lazy != NULL && CGImageGetWidth(lazy) * CGImageGetHeight(lazy) < min_pixels) {
		CGImageRelease(lazy);
		lazy = NULL;
	}
	if (lazy != NULL) {
		size_t w = CGImageGetWidth(lazy), h = CGImageGetHeight(lazy);
		CGColorSpaceRef space = CGImageGetColorSpace(lazy);
		CGColorSpaceRef rgb = space != NULL && CGColorSpaceGetModel(space) == kCGColorSpaceModelRGB ?
				      CGColorSpaceRetain(space) : CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		CGContextRef context = NULL;
		IOSurfaceRef surface = snap_surfaces && snap_image_of_surface != NULL ? snap_surface(w, h) : NULL;
		bool locked = surface != NULL && IOSurfaceLock(surface, 0, NULL) == 0;

		if (locked) {
			context = CGBitmapContextCreate(IOSurfaceGetBaseAddress(surface), w, h, 8, IOSurfaceGetBytesPerRow(surface), rgb,
							kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
		}
		if (context == NULL && surface != NULL) {
			if (locked) IOSurfaceUnlock(surface, 0, NULL);
			locked = false;
			CFRelease(surface);
			surface = NULL;
		}
		if (context == NULL) {
			context = CGBitmapContextCreate(NULL, w, h, 8, 0, rgb, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
		}
		if (context != NULL) {
			CGContextSetBlendMode(context, kCGBlendModeCopy);
			CGContextDrawImage(context, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), lazy);
			if (surface != NULL) {
				CGContextRelease(context);
				context = NULL;
				IOSurfaceUnlock(surface, 0, NULL);
				locked = false;
				decoded = snap_image_of_surface(surface, NULL);
				if (decoded != NULL && (CGImageGetWidth(decoded) != w || CGImageGetHeight(decoded) != h)) {
					CGImageRelease(decoded);
					decoded = NULL;
				}
			} else {
				decoded = CGBitmapContextCreateImage(context);
			}
			if (decoded != NULL) {
				struct stat after;

				/* (the file must not have changed while it was read) */
				if (stat(path, &after) == 0 && after.st_size == st.st_size &&
				    after.st_mtimespec.tv_sec == st.st_mtimespec.tv_sec &&
				    after.st_mtimespec.tv_nsec == st.st_mtimespec.tv_nsec) {
					snap_store(path, &st, decoded, (surface ? IOSurfaceGetBytesPerRow(surface) : CGImageGetBytesPerRow(decoded)) * h);
					if (snap_say) {
						const char *name = strrchr(path, '/');

						dprintf(2, "snap: decoded %s %zux%zu in %llu ms (%s)%s\n", name ? name + 1 : path, w, h,
							snap_ms() - began, why, surface ? ", an IOSurface" : "");
					}
				}
				CGImageRelease(decoded);
			}
			if (context != NULL) CGContextRelease(context);
		}
		if (locked) IOSurfaceUnlock(surface, 0, NULL);
		if (surface != NULL) CFRelease(surface);
		CGColorSpaceRelease(rgb);
		CGImageRelease(lazy);
	}
}

static void
snap_decode_later(id snapshot, const char *why, double seconds, size_t min_pixels)
{
	char path[1024];

	if (!snap_path_of(snapshot, path, sizeof(path))) return;
	{
		char *copy = strdup(path);

		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), snap_queue, ^{
			snap_decode(copy, why, min_pixels);
			free(copy);
		});
	}
}

extern id objc_retain(id);
extern void objc_release(id);

/* a snapshot just made, `tries` seconds ago: decode its file once it is there (it is released here) */
static void
snap_made_later(id snapshot, int tries)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), snap_queue, ^{
		char path[1024];
		struct stat st;

		if (snap_path_of(snapshot, path, sizeof(path)) && stat(path, &st) == 0 && st.st_size > 0) {
			snap_decode(path, "made", SNAP_ZOOM_PIXELS);
			objc_release(snapshot);
		} else if (tries < 5) {
			snap_made_later(snapshot, tries + 1);
		} else {
			if (snap_say) {
				id text = class_respondsToSelector(object_getClass(snapshot), sel_registerName("path")) ? SEND(id, snapshot, "path") : NULL;

				dprintf(2, "snap: a picture made, no JPEG file of it after %d s (%s)\n", tries,
					text ? SEND(const char *, text, "UTF8String") : "no path");
			}
			objc_release(snapshot);
		}
	});
}

/* -[XBApplicationSnapshot _cacheImage:]: SplashBoard has (made or read) a picture; its file is (or soon is) there */
static void
snap_cache_image(id self, SEL selector, id image)
{
	((void (*)(id, SEL, id))snap_original_cache)(self, selector, image);
	if (!pthread_main_np()) {
		/* (just made: its path and file are settled a little later -- the path it has now is not yet the JPEG's --
		 * so the snapshot is looked at again then; only the full-screen ones, which the zoom shows: the smaller
		 * ones are for the switcher of applications, and are decoded when they are asked for) */
		snap_made_later(objc_retain(self), 1);
	}
}

/* the pixels of the JPEG file at `path`, from its header (0: not a picture) */
static size_t
snap_pixels(const char *path)
{
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	CGImageSourceRef source = url ? CGImageSourceCreateWithURL(url, NULL) : NULL;
	CFDictionaryRef props = source && CGImageSourceGetCount(source) > 0 ? CGImageSourceCopyPropertiesAtIndex(source, 0, NULL) : NULL;
	long w = 0, h = 0;

	if (props != NULL) {
		CFNumberRef n;

		if ((n = CFDictionaryGetValue(props, kCGImagePropertyPixelWidth)) != NULL) CFNumberGetValue(n, kCFNumberLongType, &w);
		if ((n = CFDictionaryGetValue(props, kCGImagePropertyPixelHeight)) != NULL) CFNumberGetValue(n, kCFNumberLongType, &h);
		CFRelease(props);
	}
	if (source) CFRelease(source);
	if (url) CFRelease(url);
	return w > 0 && h > 0 ? (size_t)w * (size_t)h : 0;
}

/*
 * The picture SpringBoard warms up as the finger goes down on an icon is the small one, for the switcher of
 * applications (788x1706, in the snapshot's folder "downscaled"); the zoom that follows shows the full-screen one
 * of the same application (1125x2436), which nothing had decoded when it was not made since the phone started:
 * so the first opening of an application after a start decoded its JPEG at the commit, a turn of 93-138 ms against
 * 68-81 (logs/131839-run.log, 132220: "snap: not decoded yet ... 1125x2436" right after "decoded ... 788x1706 ...
 * (warmed up)"). Here the newest full-screen JPEG beside the warmed-up one (or a folder above "downscaled") is
 * decoded too, at once, on a queue of its own.
 */
static void
snap_full_beside(const char *warmed)
{
	char folder[1024], best[1024] = "", file[1280];
	struct timespec newest = { 0, 0 };
	const char *slash = strrchr(warmed, '/');
	size_t n = slash ? (size_t)(slash - warmed) : 0;
	DIR *dir;
	struct dirent *entry;

	if (n == 0 || n >= sizeof(folder)) return;
	memcpy(folder, warmed, n);
	folder[n] = 0;
	if ((slash = strrchr(folder, '/')) != NULL && strcmp(slash + 1, "downscaled") == 0) {
		folder[slash - folder] = 0;
	}
	for (int pass = 0; pass < 2 && best[0] == 0; pass++) {
		/* (pass 0: the newest JPEG; pass 1, if that was a small one: the newest full-screen one) */
		newest.tv_sec = newest.tv_nsec = 0;
		if ((dir = opendir(folder)) == NULL) return;
		while ((entry = readdir(dir)) != NULL) {
			size_t len = strlen(entry->d_name);
			struct stat st;

			if (len < 6 || strcasecmp(entry->d_name + len - 5, ".jpeg") != 0) continue;
			snprintf(file, sizeof(file), "%s/%s", folder, entry->d_name);
			if (strcmp(file, warmed) == 0 || stat(file, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) continue;
			if (st.st_mtimespec.tv_sec < newest.tv_sec ||
			    (st.st_mtimespec.tv_sec == newest.tv_sec && st.st_mtimespec.tv_nsec <= newest.tv_nsec)) continue;
			if (pass == 1 && snap_pixels(file) < SNAP_ZOOM_PIXELS) continue;
			newest = st.st_mtimespec;
			snprintf(best, sizeof(best), "%s", file);
		}
		closedir(dir);
		if (best[0] != 0 && pass == 0 && snap_pixels(best) < SNAP_ZOOM_PIXELS) best[0] = 0;
	}
	if (best[0] != 0) {
		char *copy = strdup(best);

		dispatch_async(snap_soon, ^{
			snap_decode(copy, "beside the warmed-up one", SNAP_ZOOM_PIXELS);
			free(copy);
		});
	}
}

/* -[XBApplicationSnapshot loadImageForPreHeat]: SpringBoard warms up a picture it is about to need */
static void
snap_preheat(id self, SEL selector)
{
	char path[1024];

	if (snap_path_of(self, path, sizeof(path))) {
		snap_full_beside(path);
	}
	((void (*)(id, SEL))snap_original_preheat)(self, selector);
	if (!pthread_main_np() && snap_path_of(self, path, sizeof(path))) {
		/* (already on a queue of SpringBoard's own, which waits for it: decoded here and now) */
		snap_decode(path, "warmed up", 0);
	} else {
		snap_decode_later(self, "warmed up", 0, 0);
	}
}

/* -[XBApplicationSnapshot imageForInterfaceOrientation:generationOptions:] */
static id
snap_image(id self, SEL selector, long orientation, unsigned long options)
{
	id image = ((id (*)(id, SEL, long, unsigned long))snap_original_image)(self, selector, orientation, options);
	char path[1024];
	struct stat st;
	CGImageRef lazy, decoded;
	CFStringRef type;
	Class ui = objc_getClass("UIImage");

	if (image == NULL || ui == Nil || !snap_path_of(self, path, sizeof(path)) ||
	    !class_respondsToSelector(object_getClass(image), sel_registerName("CGImage")) ||
	    (lazy = SEND(CGImageRef, image, "CGImage")) == NULL ||
	    (type = CGImageGetUTType(lazy)) == NULL || CFStringCompare(type, CFSTR("public.jpeg"), 0) != kCFCompareEqualTo ||
	    stat(path, &st) != 0) {
		return image;
	}
	if ((decoded = snap_lookup(path, &st)) == NULL || CGImageGetWidth(decoded) != CGImageGetWidth(lazy) ||
	    CGImageGetHeight(decoded) != CGImageGetHeight(lazy)) {
		if (snap_say) {
			const char *name = strrchr(path, '/');

			dprintf(2, "snap: not decoded yet: %s %zux%zu (the commit decodes it)\n", name ? name + 1 : path,
				CGImageGetWidth(lazy), CGImageGetHeight(lazy));
		}
		if (decoded != NULL) CGImageRelease(decoded);
		snap_decode_later(self, "asked for", 0, 0);
		return image;
	}
	{
		double scale = SEND(double, image, "scale");
		long turned = SEND(long, image, "imageOrientation");
		id replacement = ((id (*)(Class, SEL, CGImageRef, double, long))objc_msgSend)(ui,
				  sel_registerName("imageWithCGImage:scale:orientation:"), decoded, scale, turned);

		CGImageRelease(decoded);
		if (replacement == NULL) return image;
		if (snap_say) {
			const char *name = strrchr(path, '/');

			dprintf(2, "snap: given decoded %s %zux%zu\n", name ? name + 1 : path, CGImageGetWidth(lazy), CGImageGetHeight(lazy));
		}
		return replacement;
	}
}

static IMP
snap_replace(const char *class_name, const char *selector, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (method == NULL) {
		dprintf(2, "snap: -[%s %s] not found; pictures are decoded at the commit\n", class_name, selector);
		return NULL;
	}
	return method_setImplementation(method, replacement);
}

void
snapdecode_start(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *at;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		if ((at = strstr(args, "sb_predecode=0")) != NULL && (at == args || at[-1] == ' ')) return;
		snap_say = (at = strstr(args, "touch_lat=1")) != NULL && (at == args || at[-1] == ' ');
		snap_surfaces = !((at = strstr(args, "sb_snapsurface=0")) != NULL && (at == args || at[-1] == ' '));
	}
	mach_timebase_info(&snap_timebase);
	snap_image_of_surface = (CGImageRef (*)(IOSurfaceRef, CFDictionaryRef))dlsym(RTLD_DEFAULT, "CGImageCreateFromIOSurface");
	if (snap_surfaces && snap_image_of_surface == NULL) {
		dprintf(2, "snap: no CGImageCreateFromIOSurface; pictures in plain memory\n");
	}
	snap_queue = dispatch_queue_create("com.hakios.snapdecode", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
					   QOS_CLASS_UTILITY, 0));
	snap_soon = dispatch_queue_create("com.hakios.snapdecode.soon", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
					  QOS_CLASS_USER_INITIATED, 0));
	/* (all three, or none: half of it would only cost) */
	if (objc_getClass("XBApplicationSnapshot") == Nil ||
	    !class_getInstanceMethod(objc_getClass("XBApplicationSnapshot"), sel_registerName("imageForInterfaceOrientation:generationOptions:")) ||
	    !class_getInstanceMethod(objc_getClass("XBApplicationSnapshot"), sel_registerName("_cacheImage:")) ||
	    !class_getInstanceMethod(objc_getClass("XBApplicationSnapshot"), sel_registerName("loadImageForPreHeat"))) {
		dprintf(2, "snap: XBApplicationSnapshot is not as expected; pictures are decoded at the commit\n");
		return;
	}
	snap_original_image = snap_replace("XBApplicationSnapshot", "imageForInterfaceOrientation:generationOptions:", (IMP)snap_image);
	snap_original_cache = snap_replace("XBApplicationSnapshot", "_cacheImage:", (IMP)snap_cache_image);
	snap_original_preheat = snap_replace("XBApplicationSnapshot", "loadImageForPreHeat", (IMP)snap_preheat);
}
