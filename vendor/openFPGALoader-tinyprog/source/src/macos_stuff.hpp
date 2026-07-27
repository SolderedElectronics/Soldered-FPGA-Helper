// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#ifndef SRC_MACOS_STUFF_HPP_
#define SRC_MACOS_STUFF_HPP_

#include <cstdint>
#include <string>
#include <vector>

struct cable_details_t;
struct libusb_device;

/*!
 * \brief serial device information as reported by IOKit
 */
struct macos_serial_port_info_t {
	uint16_t vid = 0;         /*!< USB vendor ID */
	uint16_t pid = 0;         /*!< USB product ID */
	uint8_t bus = 0;          /*!< bus number extracted from locationID */
	uint8_t device = 0;       /*!< USB device address */
	uint32_t location = 0;    /*!< IOKit locationID */
	std::string callout;      /*!< callout device path (/dev/cu.*) */
	std::string dialin;       /*!< dial-in device path (/dev/tty.*) */
	std::string serial;       /*!< USB serial number */
	std::string product;      /*!< USB product name */
	std::string manufacturer; /*!< USB vendor/manufacturer name */
};

/*!
 * \brief enumerate macOS serial ports backed by an USB device
 * \return list of serial ports with USB parent information
 */
std::vector<macos_serial_port_info_t> list_macos_serial_ports();

/*!
 * \brief find the serial device path associated with a libusb device
 * \param[in] usb_dev: libusb device to match against IOKit serial devices
 * \param[in] details: optional USB details used to improve matching
 * \return callout or dial-in path when a unique match is found, empty otherwise
 */
std::string macos_get_device_path(libusb_device *usb_dev,
	const cable_details_t *details);

/*!
 * \brief retrieve USB details from a macOS serial device path
 * \param[in] path: callout or dial-in serial device path
 * \param[out] details: USB details filled from the matching IOKit device
 * \return true if the path was found and details were filled, false otherwise
 */
bool macos_get_device_info_from_path(const std::string &path,
	cable_details_t &details);

#endif  // SRC_MACOS_STUFF_HPP_
