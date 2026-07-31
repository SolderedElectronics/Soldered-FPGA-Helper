// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>
/* <windows.h> defines 'interface' as a macro for COM; undefine it before
 * including cable.hpp which uses 'interface' as a struct field name. */
#undef interface

#include <charconv>
#include <cstdint>
#include <ios>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "cable.hpp"
#include "display.hpp"
#include "windows_stuff.hpp"

static std::string get_device_registry_property_string(HDEVINFO hDevInfo,
	SP_DEVINFO_DATA &dev_info_data, DWORD property)
{
	DWORD data_type = 0, required_size = 0;

	SetupDiGetDeviceRegistryPropertyA(hDevInfo, &dev_info_data, property,
		&data_type, nullptr, 0, &required_size);

	if (required_size == 0)
		return {};

	std::vector<char> buffer(required_size, '\0');

	if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, &dev_info_data, property,
			&data_type, reinterpret_cast<PBYTE>(buffer.data()),
			static_cast<DWORD>(buffer.size()), nullptr))
		return {};

	if (data_type == REG_SZ || data_type == REG_MULTI_SZ)
		return std::string(buffer.data());

	return {};
}

static bool get_device_registry_property_dword(HDEVINFO hDevInfo,
	SP_DEVINFO_DATA &dev_info_data, DWORD property, uint32_t &value)
{
	DWORD data_type = 0, data = 0;
	DWORD size = sizeof(data);

	if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, &dev_info_data, property,
			&data_type, reinterpret_cast<PBYTE>(&data), size, nullptr))
		return false;

	if (data_type != REG_DWORD)
		return false;

	value = static_cast<uint32_t>(data);
	return true;
}

static std::string get_port_name_from_registry(HDEVINFO hDevInfo,
	SP_DEVINFO_DATA &dev_info_data)
{
	HKEY hDeviceKey = SetupDiOpenDevRegKey(hDevInfo, &dev_info_data,
		DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);

	if (hDeviceKey == INVALID_HANDLE_VALUE)
		return {};

	char port_name[256] = {};
	DWORD buf_size = static_cast<DWORD>(sizeof(port_name));
	DWORD value_type = 0;

	const LONG ret = RegQueryValueExA(hDeviceKey, "PortName", nullptr,
		&value_type, reinterpret_cast<LPBYTE>(port_name), &buf_size);

	RegCloseKey(hDeviceKey);

	if (ret == ERROR_SUCCESS && value_type == REG_SZ)
		return std::string(port_name);

	return {};
}

static bool extract_vid_pid(const std::string &hardware_id, uint16_t &vid,
	uint16_t &pid)
{
	const std::size_t vid_pos = hardware_id.find("VID_");
	const std::size_t pid_pos = hardware_id.find("PID_");

	if (vid_pos == std::string::npos || pid_pos == std::string::npos)
		return false;

	if (vid_pos + 8 > hardware_id.size() || pid_pos + 8 > hardware_id.size())
		return false;

	const char *vid_begin = hardware_id.data() + vid_pos + 4;
	const char *vid_end = vid_begin + 4;

	const char *pid_begin = hardware_id.data() + pid_pos + 4;
	const char *pid_end = pid_begin + 4;

	const auto vid_result = std::from_chars(vid_begin, vid_end, vid, 16);
	const auto pid_result = std::from_chars(pid_begin, pid_end, pid, 16);

	if (vid_result.ec != std::errc{} || vid_result.ptr != vid_end)
		return false;

	if (pid_result.ec != std::errc{} || pid_result.ptr != pid_end)
		return false;

	return true;
}

std::vector<com_port_info_t> list_com_ports(const cable_t *cable)
{
	std::vector<com_port_info_t> ports;

	HDEVINFO hDevInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr,
		nullptr, DIGCF_PRESENT);

	if (hDevInfo == INVALID_HANDLE_VALUE)
		return ports;

	SP_DEVINFO_DATA dev_info_data{};
	dev_info_data.cbSize = sizeof(dev_info_data);

	for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &dev_info_data); ++i) {
		com_port_info_t info;

		info.friendly_name = get_device_registry_property_string(hDevInfo,
			dev_info_data, SPDRP_FRIENDLYNAME);
		info.manufacturer = get_device_registry_property_string(hDevInfo,
			dev_info_data, SPDRP_MFG);
		info.hardware_id = get_device_registry_property_string(hDevInfo,
			dev_info_data, SPDRP_HARDWAREID);
		info.port_name = get_port_name_from_registry(hDevInfo, dev_info_data);

		info.has_vid_pid = extract_vid_pid(info.hardware_id, info.vid, info.pid);

		uint32_t bus_num_dw = 0, address_dw = 0;
		info.has_bus_addr =
			get_device_registry_property_dword(hDevInfo, dev_info_data,
				SPDRP_BUSNUMBER, bus_num_dw) &&
			get_device_registry_property_dword(hDevInfo, dev_info_data,
				SPDRP_ADDRESS, address_dw);
		if (info.has_bus_addr) {
			info.bus_num = static_cast<uint8_t>(bus_num_dw);
			info.address = static_cast<uint8_t>(address_dw);
		}

		if (info.port_name.empty())
			continue;
		if (cable->vid != 0 && (!info.has_vid_pid || info.vid != cable->vid))
			continue;
		if (cable->pid != 0 && (!info.has_vid_pid || info.pid != cable->pid))
			continue;
		if (cable->bus_addr != 0 && (!info.has_bus_addr
				|| info.bus_num != cable->bus_addr))
			continue;
		if (cable->device_addr != 0 && (!info.has_bus_addr
				|| info.address != cable->device_addr))
			continue;

		ports.push_back(std::move(info));
	}

	SetupDiDestroyDeviceInfoList(hDevInfo);
	return ports;
}

static std::string formatHex(uint16_t c, int len) {
	std::stringstream ss;
	ss << "0x";
	ss << std::hex << std::setfill('0') << std::setw(len)
	   << (static_cast<unsigned int>(static_cast<unsigned short>(c)) & 0xFFFF);
	return ss.str();
}

static std::string formatDec(char c, int len) {
	std::stringstream ss;
	ss << std::setfill('0') << std::setw(len) << std::to_string(c);
	return ss.str();
}

bool display_com_port(std::vector<com_port_info_t> ports)
{
	std::stringstream buffer;
	buffer << std::left
		<< std::setw(4) << "Bus"
		<< std::setw(7) << "device"
		<< std::setw(14) << "vid:pid"
		<< "COM port";
	printSuccess(buffer.str());

	for (const auto& cable : ports) {
		std::stringstream buffer;
		buffer << std::left // Left-align all fields
			<< std::setw(4) << formatDec(cable.bus_num, 3)
			<< std::setw(7) << formatDec(cable.address, 3)
			<< std::setw(14)
			<< (formatHex(cable.vid, 4) + ":" + formatHex(cable.pid, 4))
			<< cable.port_name;

		printInfo(buffer.str());
	}

	return true;
}
