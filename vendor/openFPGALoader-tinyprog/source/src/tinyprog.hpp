// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#ifndef SRC_TINYPROG_HPP_
#define SRC_TINYPROG_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "cable.hpp"
#include "jsonParser.hpp"
#ifdef __APPLE__
#include "libusb_ll.hpp"
#endif
#include "spiInterface.hpp"

class Uart_ll;
class SPIFlash;

/*!
 * \file tinyprog.hpp
 * \class TinyProg
 * \brief driver for TinyProg protocol
 * \author Gwenhael Goavec-Merou
 */
class TinyProg: public SPIInterface {
	public:
		/*!
		 * \brief constructor
		 * \param[in] filename: UART device path
		 * \param[in] verbose: verbose level
		 */
		TinyProg(std::string filename, const cable_t &cable,
			const uint32_t baudrate, const bool detect,
			int8_t verbose);
		~TinyProg();

		/*!
		 * \brief ask bootloader to start user design
		 * \return false if UART write fails, true otherwise
		 */
		bool boot();

		/*!
		 * \brief program userimage/userdata sections from raw files
		 * \param[in] userimage_file: user image file path (empty to skip)
		 * \param[in] userdata_file: user data file path (empty to skip)
		 * \param[in] userimage_addr: override address for user image section
		 * \param[in] userdata_addr: override address for user data section
		 * \param[in] combined_bitstream: program userimage as combined section
		 * \param[in] verify: verify written content after programming
		 * \return false when parsing/programming/verification fails
		 */
		bool program(const std::string& userimage_file,
			const std::string& userdata_file,
			uint32_t userimage_addr,
			uint32_t userdata_addr,
			bool combined_bitstream, const bool verify);

		/*!
		 * \brief update TinyProg bootloader in two steps
		 * \param[in] stage_one_file: temporary bootloader, used to write
		 *            bootloader
		 * \param[in] stage_two_file: final bootloader written at SPI Flash
		 *            start section
		 * \return false when missing file, write failed or timeout
		 */
		bool bootloader_update(const std::string &stage_one_file,
			const std::string &stage_two_file);

		/* ------------------------------------------------- */
		/*  SPInterface                                      */
		/* ------------------------------------------------- */

		/*!
		 * \brief detect and display SPI flash information
		 * \return always true
		 */
		bool detect_flash();

		 /*!
		  * \brief read flash offset byte starting at base_addr and
		  *        store into filename
		  * \param[in] filename: file to store
		  * \param[in] base_addr: offset into flash
		  * \param[in] len: byte len to read
		  * \param[in] rd_burst: read flash by rd_burst bytes
		  * \param[in] verbose: verbose level
		  * \return false when something fails
		  */
		bool dump(uint32_t base_addr, uint32_t len);

		void set_out_file(const std::string &out_file) {_out_file = out_file; }

		/*!
		 * \brief send one SPI command with symmetric payload length
		 * \param[in] cmd: opcode/command to send
		 * \param[in] tx: optional payload to write
		 * \param[out] rx: optional destination buffer for readback
		 * \param[in] len: number of byte to write/read after cmd
		 * \return 0 on success, non-zero otherwise
		 */
		int spi_put(uint8_t cmd, const uint8_t *tx, uint8_t *rx,
			uint32_t len) override;

		/*!
		 * \brief send one SPI command with distinct write/read lengths
		 * \param[in] cmd: opcode/command to send
		 * \param[in] tx: optional payload to write
		 * \param[in] tx_len: number of byte to write after cmd
		 * \param[out] rx: optional destination buffer for readback
		 * \param[in] rx_len: number of byte to read
		 * \return 0 on success, non-zero otherwise
		 */
		int spi_put(uint8_t cmd, const uint8_t *tx, const uint32_t tx_len,
			uint8_t *rx, const uint32_t rx_len) override;

		/*!
		 * \brief send one SPI frame without implicit opcode byte
		 * \param[in] tx: payload to write
		 * \param[out] rx: destination buffer for readback
		 * \param[in] len: number of byte to write/read
		 * \return 0 on success, non-zero otherwise
		 */
		int spi_put(const uint8_t *tx, uint8_t *rx,
			uint32_t len) override;

		/*!
		 * \brief poll SPI register bits until condition or timeout
		 * \param[in] cmd: opcode used to read status byte
		 * \param[in] mask: mask applied to status byte
		 * \param[in] cond: expected masked value
		 * \param[in] timeout: maximum polling iterations
		 * \param[in] verbose: display poll details
		 * \return 0 on success, -ETIME on timeout
		 */
		int spi_wait(uint8_t cmd, uint8_t mask, uint8_t cond,
			uint32_t timeout, bool verbose = false) override;
	private:
		/*!
		 * \brief open TinyProg transport and initialize UART + SPI flash helpers
		 * \param[in] filename: UART device path
		 * \return false when UART/SPI flash initialization fails
		 */
		bool connect(const std::string &filename);

		/*!
		 * \brief close TinyProg transport and release UART + SPI flash resources
		 */
		void disconnect();

		/*!
		 * \brief enumerate and display devices matching cable filter options
		 * \param[in] cable: cable description provided by command line
		 */
		void detect_cable(const cable_t &cable);

		/*!
		 * \brief erase, program and verify a raw buffer at addr in flash.
		 *        Caller is responsible for bounds checking.
		 * \param[in] addr: destination base address in flash
		 * \param[in] data: data buffer to write
		 * \param[in] len: number of byte to write
		 * \param[in] verify: verify flash content after write
		 * \return false when erase/program/verify fails
		 */
		bool program_bitstream(uint32_t addr, const uint8_t *data, uint32_t len, const bool verify);

		/*!
		 * \brief parse one raw file and program one meta-defined section
		 * \param[in] filename: raw file path to parse/program
		 * \param[in] area_name: display name for log messages
		 * \param[in] addr: optional address override (0 to use section start)
		 * \param[in] meta_addr: default start address from metadata
		 * \param[in] meta_eaddr: section exclusive end address from metadata
		 * \param[in] verify: verify flash content after write
		 * \return false on parse failure, bounds overflow or write failure
		 */
		bool program_area(const std::string &filename, const std::string &area_name,
			const uint32_t addr,
			const uint32_t meta_addr, const uint32_t meta_eaddr, const bool verify);

		/*!
		 * \brief send a one-byte opcode as SPI payload
		 * \param[in] opcode: opcode to send
		 * \param[out] rx: optional destination buffer for readback
		 * \param[in] rx_len: number of byte to read
		 * \return false on UART transfer failure
		 */
		bool send_opcode(uint8_t opcode, uint8_t *rx, uint16_t rx_len);

		/*!
		 * \brief build and send one TinyProg UART command packet
		 * \param[in] tx: payload to send after packet header
		 * \param[in] tx_len: payload length in byte
		 * \param[out] rx: optional destination buffer for readback
		 * \param[in] rx_len: number of byte to read from target
		 * \return false on UART write/read failure
		 */
		bool build_send_cmd(const uint8_t *tx, const uint16_t tx_len,
			uint8_t *rx, uint16_t rx_len);

		/* ------------------------------------------------- */
		/*  Meta                                             */
		/* ------------------------------------------------- */

		/*!
		 * \brief read, parse and decode metadata from security registers
		 * \return false on read/parse/decode failure
		 */
		bool read_meta();

		/*!
		 * \brief parse one address range string ("start-end")
		 * \param[in] meta: range string encoded in hexadecimal
		 * \param[out] addr: parsed start address
		 * \param[out] eaddr: parsed end address
		 * \return false on malformed range
		 */
		bool meta_extract_start_and_end_addr(const std::string &meta, uint32_t &addr, uint32_t &eaddr);

		/*!
		 * \brief display board/boot metadata and address map
		 */
		void display_meta();

		/* ---------- */
		/* Bootloader */
		/* ---------- */
		/*!
		 * \brief split a full URL into base, path and filename components
		 * \param[in] url: full HTTPS URL to split
		 * \param[out] base: scheme + host (e.g. "https://example.com")
		 * \param[out] path: absolute path portion (e.g. "/foo/bar/file.bin")
		 * \param[out] filename: last path component (e.g. "file.bin")
		 */
		void split_url(const std::string &url, std::string &base,
			std::string &path, std::string &filename);
		/*!
		 * \brief fetch a file from a URL and return its content in memory
		 * \param[in] url: full HTTPS URL to fetch
		 * \param[out] data: buffer filled with the response body
		 * \return false on connection or HTTP error
		 */
		bool fetch_data(const std::string &url, std::vector<uint8_t> &data);

		/* ----- Low level read/write wrappers ----- */
		/*!
		 * \brief send buffer content
		 * \param[in] data: buffer
		 * \param[in] size: number of char to send
		 * \return size if ok, < 0 otherwise
		 */
		int write(const unsigned char *data, int size);

		/*!
		 * \brief read maxsize char from device
		 * \param[in] buf: buffer to fill
		 * \param[in] maxsize: number of char to read
		 * \return read size if ok, < 0 otherwise
		 */
		int read(std::string *data, int maxsize);

		std::string _filename;      /**< UART device path */
		uint32_t _baudrate;         /**< UART baudrate */
		int8_t _verbose;            /**< verbose level */
		std::string _out_file;      /**< file name to uses in dump method */

		/* ----- UART ----- */
		Uart_ll *_uart;             /**< low level UART interface */

#ifdef __APPLE__
		/* ---- LIBUSB ---- */
		libusb_ll *_usb;
#endif

		/* ----- SPI  ----- */
		SPIFlash *_flash;           /**< SPI flash helper */

		/* ----- Meta ----- */
		JSONParser _meta;           /**< parsed metadata JSON */
		uint32_t _bootloader_addr;  /**< bootloader start address */
		uint32_t _bootloader_eaddr; /**< bootloader end address (exclusive) */
		uint32_t _userimage_addr;   /**< user image start address */
		uint32_t _userimage_eaddr;  /**< user image end address (exclusive) */
		uint32_t _userdata_addr;    /**< user data start address */
		uint32_t _userdata_eaddr;   /**< user data end address (exclusive) */
};
#endif  // SRC_TINYPROG_HPP_
