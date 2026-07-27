// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#ifndef SRC_WINDOWS_STUFF_HPP_
#define SRC_WINDOWS_STUFF_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "cable.hpp"

/*!
 * \brief COM port information as reported by SetupAPI
 */
struct com_port_info_t {
	std::string port_name;
	std::string friendly_name;
	std::string manufacturer;
	std::string hardware_id;
	uint16_t vid = 0;
	uint16_t pid = 0;
	bool has_vid_pid = false;
	uint8_t bus_num = 0;  /*!< SPDRP_BUSNUMBER: PCI bus number of the host controller */
	uint8_t address = 0;  /*!< SPDRP_ADDRESS: port number on the parent hub */
	bool has_bus_addr = false;
};

/*!
 * \brief enumerate COM ports with optional filtering
 * \param[in] vid: USB vendor ID filter (0: disabled)
 * \param[in] pid: USB product ID filter (0: disabled)
 * \param[in] bus_num: SPDRP_BUSNUMBER filter (0: disabled)
 * \param[in] address: SPDRP_ADDRESS filter (0: disabled)
 * \return list of matching COM ports
 */
std::vector<com_port_info_t> list_com_ports(const cable_t *cable);

bool display_com_port(std::vector<com_port_info_t> ports);

#endif  // SRC_WINDOWS_STUFF_HPP_
