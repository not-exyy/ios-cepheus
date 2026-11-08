/*
 * AppleARMBacklight: the screen's brightness as iOS's CoreBrightness looks for it (drivers/gpu/hakios.backlight.c
 * sends what it is given to the Mi 9's panel).
 *
 * What CoreBrightness does with it (backboardd, 20F66's shared cache, CBDisplayContaineriOS and Display.m):
 *
 *   finds it      IOServiceGetMatchingServices({ IOPropertyMatch = { "backlight-control" = true } }), and
 *                 IOObjectConformsTo(service, "AppleARMBacklight") says it is the built-in display's backlight
 *   reads         "IODisplayParameters" = { "brightness" = { "min", "max", "value" } }: the range brightness is set in
 *                 (absent: "new-backlight-architecture", "backlight-calibration-parameters" and the rest of an
 *                 iPhone's calibration, which it does without)
 *   sets          IORegistryEntrySetCFProperties(service, { "brightness" = n, "FadePeriod" = t, "backlight-level" = m }),
 *                 n an integer in min..max (its own fraction of the range times max - min, plus min)
 *
 * setProperties() below is that last call. It hands "brightness" and "FadePeriod" to the driver and keeps the value
 * in IODisplayParameters, and logs the first dictionaries it is given in full, so that what CoreBrightness sends can
 * be read in the run's log.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>

extern "C" int HakiosBacklightPublish(uint32_t min, uint32_t max, uint32_t value);
extern "C" void hakios_backlight_request(uint32_t value, uint32_t fade, int has_fade);

#define TOLD    12      /* dictionaries logged in full */

class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	void parameters(uint32_t value);

	uint32_t min, max;

private:
	unsigned told;
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

static AppleARMBacklight * backlight;

/* IODisplayParameters = { brightness = { min, max, value } } */
void
AppleARMBacklight::parameters(uint32_t value)
{
	OSDictionary * outer = OSDictionary::withCapacity(1), * brightness = OSDictionary::withCapacity(3);
	OSNumber * lo = OSNumber::withNumber(min, 32), * hi = OSNumber::withNumber(max, 32), * now = OSNumber::withNumber(value, 32);

	if (outer != NULL && brightness != NULL && lo != NULL && hi != NULL && now != NULL) {
		brightness->setObject("min", lo);
		brightness->setObject("max", hi);
		brightness->setObject("value", now);
		outer->setObject("brightness", brightness);
		setProperty("IODisplayParameters", outer);
	}
	OSSafeReleaseNULL(now);
	OSSafeReleaseNULL(hi);
	OSSafeReleaseNULL(lo);
	OSSafeReleaseNULL(brightness);
	OSSafeReleaseNULL(outer);
}

/* a number from what user space gave: an integer, or a dictionary { value = n } as IODisplay's parameters are */
static bool
number_of(OSObject * object, uint32_t * out)
{
	OSNumber * n = OSDynamicCast(OSNumber, object);
	OSDictionary * d = OSDynamicCast(OSDictionary, object);

	if (n == NULL && d != NULL) {
		n = OSDynamicCast(OSNumber, d->getObject("value"));
	}
	if (n == NULL) {
		return false;
	}
	*out = n->unsigned32BitValue();
	return true;
}

IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	uint32_t value = 0, fade = 0;
	bool has_value, has_fade;

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	if (told < TOLD) {
		OSCollectionIterator * keys = OSCollectionIterator::withCollection(dict);
		char text[400];
		int at = 0;

		told++;
		text[0] = 0;
		if (keys != NULL) {
			OSSymbol * key;

			while ((key = OSDynamicCast(OSSymbol, keys->getNextObject())) != NULL && at < (int)sizeof(text) - 80) {
				OSObject * v = dict->getObject(key);
				OSNumber * n = OSDynamicCast(OSNumber, v);

				if (n != NULL) {
					at += snprintf(text + at, sizeof(text) - at, " %s=%llu", key->getCStringNoCopy(), n->unsigned64BitValue());
				} else {
					at += snprintf(text + at, sizeof(text) - at, " %s=(%s)", key->getCStringNoCopy(),
					    v != NULL ? v->getMetaClass()->getClassName() : "?");
				}
			}
			keys->release();
		}
		IOLog("AppleARMBacklight: setProperties %u:%s\n", told, text);
	}
	has_value = number_of(dict->getObject("brightness"), &value);
	has_fade = number_of(dict->getObject("FadePeriod"), &fade);
	if (!has_value) {
		return kIOReturnSuccess;        /* (what else may come is logged above; nothing in it is acted on) */
	}
	if (value > max) {
		value = max;
	}
	parameters(value);
	hakios_backlight_request(value, fade, has_fade);
	return kIOReturnSuccess;
}

/* From the driver's thread (hakios.backlight.c), once: the service made and registered. 0 when it is up. */
int
HakiosBacklightPublish(uint32_t min, uint32_t max, uint32_t value)
{
	AppleARMBacklight * made;
	IOService * platform;

	if (backlight != NULL) {
		return 0;
	}
	made = OSTypeAlloc(AppleARMBacklight);
	if ((platform = IOService::getPlatform()) == NULL || made == NULL || !made->init()) {
		OSSafeReleaseNULL(made);
		return -1;
	}
	made->min = min;
	made->max = max;
	made->setProperty("backlight-control", kOSBooleanTrue);
	made->parameters(value);
	if (!made->attach(platform)) {
		made->release();
		return -1;
	}
	made->setName("AppleARMBacklight");
	if (!made->start(platform)) {
		made->detach(platform);
		made->release();
		return -1;
	}
	made->registerService();
	backlight = made;
	return 0;
}
