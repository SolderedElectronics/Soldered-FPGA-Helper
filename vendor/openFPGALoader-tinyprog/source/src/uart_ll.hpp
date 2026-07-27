// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2022 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#ifndef SRC_UART_LL_HPP_
#define SRC_UART_LL_HPP_

#include <stdint.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
using speed_t = uint32_t;
#else
#  include <termios.h>
#endif

#include <string>

/*!
 * \file uart_ll
 * \class Uart_ll
 * \brief low level implementation for UART protocol
 * \author Gwenhael Goavec-Merou
 */
class Uart_ll {
	public:
		/*!
		 * \brief constructor
		 * \param[in] filename: /dev/ttyxx path (Linux/macOS) or COMx path (Windows)
		 * \param[in] clkHz: baudrate (in Hz)
		 * \param[in] byteSize: transaction size (5 to 8)
		 * \param[in] twoStopBits: false for 1 stop bit, true for 2 stop bits
		 * \param[in] readTimeout: read timeout in seconds (default: 2)
		 * \param[in] writeTimeout: write timeout in seconds (default: 5)
		 */

		Uart_ll(const std::string &filename, uint32_t clkHz,
			uint8_t byteSize, bool twoStopBits,
			uint8_t readTimeout = 2, uint8_t writeTimeout = 5);
		~Uart_ll();

		/*!
		 * \brief open device and port configuration
		 * \param[in] open_quiet: when true no errors are displayed if port can't be opened.
		 * \return false if the device if already opened, open fails or configure fails
		 */
		bool connect(bool open_quiet=false);

		/*!
		 * \brief close device and reapply original configuration
		 * \return false if the device if configure step fails
		 */
		bool disconnect();

		int setClkFreq(uint32_t clkHz);
		uint32_t getClkFreq();

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
		int read(std::string *buf, int maxsize);
		/*!
		 * \brief read char from device until receiving end char
		 * \param[in] buf: buffer to fill
		 * \param[in] end: end character
		 * \return read size if ok, < 0 otherwise
		 */
		int read_until(std::string *buf, uint8_t end = '\n');

		/*!
		 * \brief flush by reading maxsize char from device
		 * \param[in] maxsize: number of char to read
		 * \return true if ok, false otherwise
		 */
		bool flush(int maxsize=64);
	private:
		/*!
		 * \brief serial port configuration
		 * \return -1 if something fails, 0 otherwise
		 */
		int port_configure(void);
		/*!
		 * \brief convert a frequency to the baudrate (BXXX)
		 * \param[in] clkHz: clock frequency
		 * \return the corresponding baudrate value
		 */
		speed_t freq_to_baud(uint32_t clkHz);
		/*!
		 * \brief convert the baudrate (BXXX) to the corresponding frequency
		 * \param[in] baud: a speed_t (BXXX) value
		 * \return the corresponding frequency (Hz)
		 */
		uint32_t baud_to_freq(speed_t baud);

#ifdef _WIN32
		HANDLE _serial;              /*! device handle                      */
		DCB _prev_dcb;               /*! original port configuration        */
		DCB _curr_dcb;               /*! current port configuration         */
		COMMTIMEOUTS _prev_timeouts; /*! original timeout configuration     */
		BYTE _byteSize;              /*! transfer size                      */
#else
		struct termios _prev_termios; /*! original serial port configuration */
		struct termios _curr_termios; /*! current serial port configuration  */
		speed_t _baudrate;            /*! current baudrate                   */
		int _serial;                  /*! device file descriptor             */
		tcflag_t _byteSize;           /*! transfer size                      */
#endif
		std::string _filename;        /*! device path                        */
		uint32_t _clkHz;              /*! current clk frequency              */
		bool _twoStopBits;
		uint8_t _readTimeout;         /*! read timeout in seconds            */
		uint8_t _writeTimeout;        /*! write timeout in seconds           */
};
#endif  // SRC_UART_LL_HPP_
