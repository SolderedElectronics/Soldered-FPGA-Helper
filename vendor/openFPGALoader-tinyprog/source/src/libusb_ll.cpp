// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2022 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#include <libusb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <vector>
#include <string>
#include <sstream> // For std::stringstream
#include <stdexcept>

#include "cable.hpp"
#include "display.hpp"
#include "libusb_ll.hpp"
#ifdef __APPLE__
#include "macos_stuff.hpp"
#endif

libusb_ll::libusb_ll(int vid, int pid, int8_t _verbose):
	_usb_ctx(nullptr), _dev_handle(nullptr), _verbose(_verbose >= 2),
	_if_num(-1), _kernel_driver_detached(false), _interface_claimed(false)
{
	(void)vid;
	(void)pid;
	if (libusb_init(&_usb_ctx) < 0)
		throw std::runtime_error("libusb_init_failed");
	ssize_t list_size = libusb_get_device_list(_usb_ctx, &_dev_list);
	if (list_size < 0)
		throw std::runtime_error("libusb_get_device_list_failed");
	if (list_size == 0)
		printError("No USB devices found");
	if (_verbose)
		printf("found %zd\n", list_size);
}

libusb_ll::~libusb_ll()
{
	disconnect();
	libusb_free_device_list(_dev_list, 1);
	libusb_exit(_usb_ctx);
}

int libusb_ll::get_devices_list(const cable_t *cable)
{
	int vid = 0, pid = 0;
	uint8_t bus_addr = 0, device_addr = 0;

	if (cable != nullptr) {
		vid = cable->vid;
		pid = cable->pid;
		bus_addr = cable->bus_addr;
		device_addr = cable->device_addr;
	}
	return get_devices_list(vid, pid, bus_addr, device_addr);
}

int libusb_ll::get_devices_list(const int vid, const int pid,
	const uint8_t bus_addr, const uint8_t device_addr)
{
	bool vid_pid_filter = false;  // if vid/pid only keep matching nodes
	bool bus_dev_filter = false;  // if bus/dev only keep matching nodes

	vid_pid_filter = (vid != 0) && (pid != 0);
	bus_dev_filter = !(bus_addr == 0 && device_addr == 0);

	int i = 0;
	libusb_device *usb_dev;

	_usb_dev_list.clear();
	_cable_list.clear();

	while ((usb_dev = _dev_list[i++]) != nullptr) {
		if (_verbose) {
			printf("%x %x %x %x\n", bus_addr, device_addr,
				libusb_get_device_address(usb_dev),
				libusb_get_bus_number(usb_dev));
		}

		/* bus addr and device addr provided: check */
		if (bus_dev_filter && (
				bus_addr != libusb_get_device_address(usb_dev) ||
				device_addr != libusb_get_bus_number(usb_dev)))
			continue;

		struct libusb_device_descriptor desc;
		if (libusb_get_device_descriptor(usb_dev, &desc) != 0) {
			printError("Unable to get device descriptor");
			continue;
		}

		if (_verbose) {
			printf("%x %x %x %x\n", vid, pid,
				desc.idVendor, desc.idProduct);
		}

		/* Linux host controller */
		if (desc.idVendor == 0x1d6b)
			continue;

		/* check for VID/PID */
		if (vid_pid_filter && (
				vid != desc.idVendor || pid != desc.idProduct))
			continue;

		struct cable_details_t cable(desc.idVendor, desc.idProduct, usb_dev);

		char probe_type[256];
		if (!get_probe_type(cable.vid, cable.pid, probe_type))
			continue;
		cable.probe = reinterpret_cast<const char *>(probe_type);

		get_device_informations(cable, desc);
		_cable_list.push_back(std::move(cable));

		_usb_dev_list.push_back(usb_dev);
	}

	return static_cast<int>(_usb_dev_list.size());
}

bool libusb_ll::get_device_informations(cable_details_t &dev,
	const struct libusb_device_descriptor &desc)
{
	libusb_device_handle *handle;
	int ret = libusb_open(dev.usb_dev, &handle);
	if (ret != 0) {
		char mess[1024];
		snprintf(mess, 1024,
			"Error: can't open device with vid:vid = 0x%04x:0x%04x. "
			"Error code %d %s",
			dev.vid, dev.pid,
			ret, libusb_strerror(static_cast<libusb_error>(ret)));
		printError(mess);
		return false;
	}
	uint8_t iproduct[200];
	uint8_t iserial[200];
	uint8_t imanufacturer[200];
	ret = libusb_get_string_descriptor_ascii(handle,
		desc.iProduct, iproduct, 200);
	if (ret < 0)
		snprintf(reinterpret_cast<char*>(iproduct), 200, "none");
	ret = libusb_get_string_descriptor_ascii(handle,
		desc.iManufacturer, imanufacturer, 200);
	if (ret < 0)
		snprintf(reinterpret_cast<char*>(imanufacturer), 200, "none");
	ret = libusb_get_string_descriptor_ascii(handle,
		desc.iSerialNumber, iserial, 200);
	if (ret < 0)
		snprintf(reinterpret_cast<char*>(iserial), 200, "none");
	dev.product = reinterpret_cast<const char *>(iproduct);
	dev.serial = reinterpret_cast<const char *>(iserial);
	dev.manufacturer = reinterpret_cast<const char *>(imanufacturer);
	dev.bus = libusb_get_bus_number(dev.usb_dev);
	dev.device = libusb_get_device_address(dev.usb_dev);
#ifdef __APPLE__
	dev.path = macos_get_device_path(dev.usb_dev, &dev);
#endif

	libusb_close(handle);

	return true;
}

namespace fs = std::filesystem;

#ifndef __APPLE__
// Return the first child directory name inside a directory, or "".
static std::string first_child_dir(const fs::path& path)
{
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(path, ec))
		if (entry.is_directory())
			return entry.path().filename().string();
	return "";
}

static std::string tty_from_dir(const fs::path& search_dir)
{
	fs::path tty_dir = search_dir / "tty";

	if (!fs::is_directory(tty_dir))
		return "";

	std::string node_name = first_child_dir(tty_dir);
	if (node_name.empty())
		return "";

	fs::path devnode = fs::path("/dev") / node_name;

	return (fs::exists(devnode)) ? devnode.string() : "";
}

/* Starting from a /sys path, search for a tty device node. */
static std::string find_tty_node(const std::string &sys_path) {
	std::error_code ec;
	std::string result;

	for (const auto& entry : fs::directory_iterator(sys_path, ec)) {
		if (!entry.is_directory())
			continue;

		// Interface dirs contain a colon (e.g. "1-1.2:1.0")
		std::string name = entry.path().filename().string();
		if (name.find(':') == std::string::npos)
			continue;

		// Layout 1: <iface>/tty/ttyXXX   (cp210x, cdc_acm, ch341, …)
		result = tty_from_dir(entry.path());
		if (!result.empty())
			break;

		// Layout 2: <iface>/<driver_subdir>/tty/ttyXXX   (ftdi_sio, …)
		for (const auto& sub : fs::directory_iterator(entry.path(), ec)) {
			if (!sub.is_directory())
				continue;
			result = tty_from_dir(sub.path());
			if (!result.empty())
				break;
		}
		if (!result.empty())
			break;
	}

	return result;
}

// Build the sysfs path for a libusb device:
//   /sys/bus/usb/devices/<bus>-<port1>[.<port2>…]
static std::string sysfs_path(libusb_device* dev) {
	uint8_t bus = libusb_get_bus_number(dev);
	uint8_t ports[7];
	memset(ports, 0, sizeof(ports));
	int depth = libusb_get_port_numbers(dev, ports, 7);

	std::ostringstream ss;
	ss << "/sys/bus/usb/devices/" << static_cast<int>(bus) << "-";
	for (int i = 0; i < depth; ++i) {
		if (i)
			ss << '.';
		ss << static_cast<int>(ports[i]);
	}
	return ss.str();
}
#endif

std::vector<std::string> libusb_ll::get_device_path()
{
	std::vector<std::string> dev_path;
	if (_verbose)
		printf("%zu\n", _usb_dev_list.size());
	for (size_t i = 0; i < _usb_dev_list.size(); i++) {
		libusb_device *dev = _usb_dev_list[i];
#ifdef __APPLE__
		const cable_details_t *details = nullptr;
		if (i < _cable_list.size())
			details = &_cable_list[i];
		dev_path.push_back(macos_get_device_path(dev, details));
#else
		dev_path.push_back(find_tty_node(sysfs_path(dev)));
#endif
	}
	return dev_path;
}

bool libusb_ll::get_device_info_from_path(const std::string &path,
	cable_details_t &dev)
{
#ifdef __APPLE__
	if (!macos_get_device_info_from_path(path, dev))
		return false;

	for (int i = 0; _dev_list[i] != nullptr; i++) {
		libusb_device *usb_dev = _dev_list[i];
		struct libusb_device_descriptor desc;
		if (libusb_get_device_descriptor(usb_dev, &desc) != 0)
			continue;
		if (desc.idVendor != dev.vid || desc.idProduct != dev.pid)
			continue;
		if (dev.device != 0 && dev.device != libusb_get_device_address(usb_dev))
			continue;

		dev.usb_dev = usb_dev;
		dev.bus = libusb_get_bus_number(usb_dev);
		dev.device = libusb_get_device_address(usb_dev);
		char probe_type[256];
		if (get_probe_type(dev.vid, dev.pid, probe_type))
			dev.probe = reinterpret_cast<const char *>(probe_type);
		return true;
	}
	return true;
#else
	(void)path;
	(void)dev;
	return false;
#endif
}

bool libusb_ll::connect(const int vid, const int pid, const int if_num)
{
	int ret;
	if (_dev_handle) {
		printError("libusb_ll:connect: USB device already open");
		return false;
	}

	_dev_handle = libusb_open_device_with_vid_pid(_usb_ctx, vid, pid);
	if (!_dev_handle) {
		printError("fails to open device");
		return false;
	}
	_if_num = if_num;

#ifndef __APPLE__
	/* Linux: if a kernel driver is attached, detach it before claiming. */
	ret = libusb_kernel_driver_active(_dev_handle, if_num);
	if (ret == 1) {
		ret = libusb_detach_kernel_driver(_dev_handle, if_num);
		if (ret != 0) {
			printError("libusb error while detaching kernel driver " + std::to_string(ret));
			disconnect();
			return false;
		}
		_kernel_driver_detached = true;
	} else if (ret < 0 && ret != LIBUSB_ERROR_NOT_SUPPORTED) {
		printError("libusb error while checking kernel driver state " + std::to_string(ret));
		disconnect();
		return false;
	}
#endif

	ret = libusb_claim_interface(_dev_handle, if_num);
	if (ret) {
		printError("libusb error while claiming device interface " + std::to_string(ret));
		disconnect();
		return false;
	}
	_interface_claimed = true;

	return true;
}

bool libusb_ll::disconnect()
{
	int ret;
	if (_dev_handle == nullptr)
		return true;

	if (_interface_claimed && _if_num >= 0) {
		ret = libusb_release_interface(_dev_handle, _if_num);
		if (ret != 0)
			printError("libusb error while releasing interface " +
				std::to_string(_if_num) + ": " + std::to_string(ret));
	}

	if (_kernel_driver_detached && _if_num >= 0) {
		ret = libusb_attach_kernel_driver(_dev_handle, _if_num);
		if (ret != 0 && ret != LIBUSB_ERROR_NOT_SUPPORTED)
			printError("libusb error while reattaching kernel driver on if " +
				std::to_string(_if_num) + ": " + std::to_string(ret));
	}

	libusb_close(_dev_handle);
	_dev_handle = nullptr;
	_if_num = -1;
	_kernel_driver_detached = false;
	_interface_claimed = false;
	return true;
}

bool libusb_ll::control_xfer(const uint8_t bmRequestType, const uint8_t bRequest,
		const uint16_t wValue, const uint16_t wIndex,
		const uint8_t *data, const uint32_t len, unsigned int timeout)
{
	int ret;
	if (!_dev_handle) {
		printError("libusb_ll:control_xfer: device not connected");
		return false;
	}

	ret = libusb_control_transfer(_dev_handle, bmRequestType, bRequest,
		wValue, wIndex, (unsigned char *)data, len, timeout);
	if (ret < 0) {
		printError("libusb_ll:control_xfer: failed " + std::to_string(ret) +
			" (" + libusb_strerror(static_cast<libusb_error>(ret)) + ")");
		return false;
	}
	return true;
}

bool libusb_ll::bulk_write(uint8_t write_ep, const uint8_t *tx_buf,
	uint32_t len, int *real_xfer_len, int timeout)
{
	int ret, actual_length;
	if (!_dev_handle) {
		printError("libusb_ll:bulk_write: device not connected");
		*real_xfer_len = -1;
		return false;
	}

	ret = libusb_bulk_transfer(_dev_handle, write_ep,
		(unsigned char *)tx_buf, len, &actual_length, timeout);
	if (ret < 0) {
		printError("libusb_ll:bulk_write: write failed " + std::to_string(ret));
		*real_xfer_len = ret;
		return false;
	}
	if (_verbose)
		printf("write actual_length %d ret %d\n", actual_length, ret);
	*real_xfer_len = actual_length;
	if (actual_length != (int)len) {
		printError("libusb_ll:bulk_write: short write " + std::to_string(actual_length) +
			" / " + std::to_string(len));
		return false;
	}
	return true;
}

bool libusb_ll::bulk_read(uint8_t read_ep, uint8_t *rx_buf,
	uint32_t len, int *real_xfer_len, int timeout)
{
	int ret, actual_length;
	if (!_dev_handle) {
		printError("libusb_ll:bulk_read: device not connected");
		*real_xfer_len = -1;
		return false;
	}
	ret = libusb_bulk_transfer(_dev_handle, read_ep,
		rx_buf, len, &actual_length, timeout);
	if (ret < 0) {
		printError("libusb_ll: bulk_read: read failed " + std::to_string(ret));
		*real_xfer_len = ret;
		return false;
	}
	*real_xfer_len = actual_length;
	if (actual_length == 0) {
		printError("libusb_ll: bulk_read: received 0 bytes");
		return false;
	}
	if (_verbose) {
		printf("actual_length %d ret %d\n", actual_length, ret);
		for (uint32_t i = 0; i < len; i++)
			printf("%d %02x\n", i, rx_buf[i]);
	}
	return true;
}

std::string formatHex(uint16_t c, int len) {
	std::stringstream ss;
	ss << "0x";
	ss << std::hex << std::setfill('0') << std::setw(len)
	   << (static_cast<unsigned int>(static_cast<unsigned short>(c)) & 0xFFFF);
	return ss.str();
}

std::string formatDec(uint8_t c, int len) {
	std::stringstream ss;
	ss << std::setfill('0') << std::setw(len) << std::to_string(c);
	return ss.str();
}

bool libusb_ll::get_probe_type(const uint16_t vid, const uint16_t pid,
	char *probe_type)
{
	/* ftdi devices */
	// FIXME: missing iProduct in cable_list
	if (vid == 0x403) {
		switch (pid) {
		case 0x6010:
			snprintf(probe_type, 256, "FTDI2232");
			break;
		case 0x6011:
			snprintf(probe_type, 256, "ft4232");
			break;
		case 0x6001:
			snprintf(probe_type, 256, "ft232RL");
			break;
		case 0x6014:
			snprintf(probe_type, 256, "ft232H");
			break;
		case 0x6015:
			snprintf(probe_type, 256, "ft231X");
			break;
		case 0x6043:
			snprintf(probe_type, 256, "FT4232HP");
			break;
		default:
			snprintf(probe_type, 256, "unknown FTDI");
			break;
		}
		return true;
	} else {
		// FIXME: DFU device can't be detected here
		for (const auto& b : cable_list) {
			const cable_t *c = &b.second;
			if (c->vid == vid && c->pid == pid) {
				snprintf(probe_type, 256, "%s", b.first.c_str());
				return true;
			}
		}
	}
	return false;
}

bool libusb_ll::scan()
{
	size_t manufacturer_len = 12;
	size_t probe_len = 10;
	size_t serial_len = 6;
#ifdef __APPLE__
	size_t path_len = 4;
#endif

	get_devices_list(nullptr);

	for (const cable_details_t &cable: _cable_list) {
		const uint32_t ml = static_cast<uint32_t>(cable.manufacturer.size());
		const uint32_t pl = static_cast<uint32_t>(cable.probe.size());
		const uint32_t sl = static_cast<uint32_t>(cable.serial.size());
#ifdef __APPLE__
		const uint32_t pathl = static_cast<uint32_t>(cable.path.size());
#endif

		if (ml > manufacturer_len)
			manufacturer_len = ml;
		if (pl > probe_len)
			probe_len = pl;
		if (sl > serial_len)
			serial_len = sl;
#ifdef __APPLE__
		if (pathl > path_len)
			path_len = pathl;
#endif
	}

	manufacturer_len++;
	serial_len++;
	probe_len++;
#ifdef __APPLE__
	path_len++;
#endif

	std::stringstream buffer;
	buffer << std::left
		<< std::setw(4) << "Bus"
		<< std::setw(7) << "device"
		<< std::setw(14) << "vid:pid"
		<< std::setw(probe_len) << "probe_type"
		<< std::setw(manufacturer_len) << "manufacturer"
		<< std::setw(serial_len) << "serial"
#ifdef __APPLE__
		<< std::setw(path_len) << "path"
#endif
		<< "product";
	printSuccess(buffer.str());

	for (const auto& cable : _cable_list) {
		std::stringstream buffer;
		buffer << std::left // Left-align all fields
			<< std::setw(4) << formatDec(cable.bus, 3)
			<< std::setw(7) << formatDec(cable.device, 3)
			<< std::setw(14)
			<< (formatHex(cable.vid, 4) + ":" + formatHex(cable.pid, 4))
			<< std::setw(probe_len) << cable.probe
			<< std::setw(manufacturer_len) << cable.manufacturer
			<< std::setw(serial_len) << cable.serial
#ifdef __APPLE__
			<< std::setw(path_len) << cable.path
#endif
			<< cable.product;

		printInfo(buffer.str());
	}

	return true;
}
