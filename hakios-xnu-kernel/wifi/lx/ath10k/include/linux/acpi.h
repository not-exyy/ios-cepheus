/*
 * ACPI, of which this machine has none: what mac.c's question to it (the regulatory domain of
 * the method WRDD, ath10k_mac_get_wrdd_regulatory()) is written in. The device has no handle,
 * and so the question is not asked.
 */
#ifndef _LX_ATH10K_LINUX_ACPI_H
#define _LX_ATH10K_LINUX_ACPI_H

#include <linux/device.h>

typedef void *acpi_handle;
typedef u32 acpi_status;
typedef char *acpi_string;
typedef u64 acpi_size;

#define AE_OK			0
#define AE_NOT_FOUND		5
#define ACPI_SUCCESS(s)		((s) == AE_OK)
#define ACPI_FAILURE(s)		((s) != AE_OK)
#define ACPI_TYPE_INTEGER	1
#define ACPI_TYPE_PACKAGE	4
#define ACPI_ALLOCATE_BUFFER	((acpi_size)-1)

union acpi_object {
	u32 type;
	struct { u32 type; u64 value; } integer;
	struct { u32 type; u32 count; union acpi_object *elements; } package;
};

struct acpi_buffer {
	acpi_size length;
	void *pointer;
};

struct acpi_object_list;

#define ACPI_HANDLE(dev)	((acpi_handle)NULL)

static inline acpi_status
acpi_get_handle(acpi_handle parent, acpi_string name, acpi_handle *handle)
{
	return (AE_NOT_FOUND);
}

static inline acpi_status
acpi_evaluate_object(acpi_handle handle, acpi_string name, struct acpi_object_list *arguments, struct acpi_buffer *result)
{
	return (AE_NOT_FOUND);
}

#endif
