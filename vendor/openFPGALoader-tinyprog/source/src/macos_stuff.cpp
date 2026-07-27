// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#include "macos_stuff.hpp"

#include <libusb.h>

#include <limits.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

#include "libusb_ll.hpp"

static bool cf_number_to_u32(CFTypeRef value, uint32_t &out)
{
	if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID())
		return false;
	return CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt32Type, &out);
}

static bool cf_property_u32(io_registry_entry_t service, CFStringRef key,
	uint32_t &out)
{
	CFTypeRef value = IORegistryEntryCreateCFProperty(service, key,
		kCFAllocatorDefault, 0);
	bool ret = cf_number_to_u32(value, out);
	if (value != nullptr)
		CFRelease(value);
	return ret;
}

static std::string cf_property_string(io_registry_entry_t service,
	CFStringRef key)
{
	CFTypeRef value = IORegistryEntryCreateCFProperty(service, key,
		kCFAllocatorDefault, 0);
	if (value == nullptr)
		return "";

	std::string ret;
	if (CFGetTypeID(value) == CFStringGetTypeID()) {
		char buffer[PATH_MAX];
		if (CFStringGetCString(static_cast<CFStringRef>(value), buffer,
				sizeof(buffer), kCFStringEncodingUTF8))
			ret = buffer;
	}
	CFRelease(value);
	return ret;
}

static bool macos_find_usb_parent(io_registry_entry_t service,
	io_registry_entry_t &usb_dev)
{
	io_registry_entry_t current = service;
	IOObjectRetain(current);

	while (current != IO_OBJECT_NULL) {
		if (IOObjectConformsTo(current, "IOUSBHostDevice") ||
				IOObjectConformsTo(current, "IOUSBDevice")) {
			usb_dev = current;
			return true;
		}

		io_registry_entry_t parent = IO_OBJECT_NULL;
		kern_return_t kr = IORegistryEntryGetParentEntry(current,
			kIOServicePlane, &parent);
		IOObjectRelease(current);
		if (kr != KERN_SUCCESS)
			break;
		current = parent;
	}

	return false;
}

static bool macos_serial_from_service(io_registry_entry_t serial_service,
	macos_serial_port_info_t &dev)
{
	dev.callout = cf_property_string(serial_service, CFSTR(kIOCalloutDeviceKey));
	dev.dialin = cf_property_string(serial_service, CFSTR(kIODialinDeviceKey));
	if (dev.callout.empty() && dev.dialin.empty())
		return false;

	io_registry_entry_t usb_dev = IO_OBJECT_NULL;
	if (!macos_find_usb_parent(serial_service, usb_dev))
		return false;

	uint32_t value = 0;
	if (cf_property_u32(usb_dev, CFSTR("idVendor"), value))
		dev.vid = value;
	if (cf_property_u32(usb_dev, CFSTR("idProduct"), value))
		dev.pid = value;
	if (cf_property_u32(usb_dev, CFSTR("USB Address"), value))
		dev.device = value;
	if (cf_property_u32(usb_dev, CFSTR("locationID"), value)) {
		dev.location = value;
		dev.bus = (value >> 24) & 0xff;
	}

	dev.serial = cf_property_string(usb_dev, CFSTR("USB Serial Number"));
	dev.product = cf_property_string(usb_dev, CFSTR("USB Product Name"));
	dev.manufacturer = cf_property_string(usb_dev, CFSTR("USB Vendor Name"));

	IOObjectRelease(usb_dev);
	return dev.vid != 0 && dev.pid != 0;
}

std::vector<macos_serial_port_info_t> list_macos_serial_ports()
{
	std::vector<macos_serial_port_info_t> devices;
	CFMutableDictionaryRef matching = IOServiceMatching(kIOSerialBSDServiceValue);
	if (matching == nullptr)
		return devices;
	CFDictionarySetValue(matching, CFSTR(kIOSerialBSDTypeKey),
		CFSTR(kIOSerialBSDAllTypes));

	io_iterator_t iterator = IO_OBJECT_NULL;
	kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
		matching, &iterator);
	if (kr != KERN_SUCCESS)
		return devices;

	io_object_t service;
	while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
		macos_serial_port_info_t dev;
		if (macos_serial_from_service(service, dev))
			devices.push_back(std::move(dev));
		IOObjectRelease(service);
	}
	IOObjectRelease(iterator);
	return devices;
}

static bool same_known_string(const std::string &a, const std::string &b)
{
	return !a.empty() && a != "none" && !b.empty() && b != "none" && a == b;
}

std::string macos_get_device_path(libusb_device *usb_dev,
	const cable_details_t *details)
{
	struct libusb_device_descriptor desc;
	if (libusb_get_device_descriptor(usb_dev, &desc) != 0)
		return "";

	uint8_t bus = libusb_get_bus_number(usb_dev);
	uint8_t device = libusb_get_device_address(usb_dev);
	std::vector<macos_serial_port_info_t> devices = list_macos_serial_ports();
	std::string device_address_match;
	int device_address_matches = 0;

	for (const macos_serial_port_info_t &dev : devices) {
		if (dev.vid != desc.idVendor || dev.pid != desc.idProduct)
			continue;
		if (details != nullptr && same_known_string(details->serial, dev.serial))
			return !dev.callout.empty() ? dev.callout : dev.dialin;
		if (dev.device == device && (dev.bus == bus || dev.bus == 0 || bus == 0))
			return !dev.callout.empty() ? dev.callout : dev.dialin;
		if (dev.device == device) {
			device_address_match = !dev.callout.empty() ? dev.callout : dev.dialin;
			device_address_matches++;
		}
	}

	return device_address_matches == 1 ? device_address_match : "";
}

bool macos_get_device_info_from_path(const std::string &path,
	cable_details_t &details)
{
	std::vector<macos_serial_port_info_t> devices = list_macos_serial_ports();
	for (const macos_serial_port_info_t &dev : devices) {
		if (dev.callout != path && dev.dialin != path)
			continue;

		details.vid = dev.vid;
		details.pid = dev.pid;
		details.bus = dev.bus;
		details.device = dev.device;
		details.serial = dev.serial.empty() ? "none" : dev.serial;
		details.product = dev.product.empty() ? "none" : dev.product;
		details.manufacturer = dev.manufacturer.empty() ? "none" : dev.manufacturer;
		details.path = dev.callout.empty() ? dev.dialin : dev.callout;
		return true;
	}
	return false;
}
