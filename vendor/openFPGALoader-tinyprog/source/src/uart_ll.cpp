// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2022 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#include "uart_ll.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cctype>
/* DCB ByteSize uses the literal byte count; mirror the POSIX names */
#define CS5 5
#define CS6 6
#define CS7 7
#define CS8 8
/* Provide POSIX-like baud symbols so freq_to_baud() can be reused on Windows. */
#define B2400 2400
#define B4800 4800
#define B9600 9600
#define B19200 19200
#define B38400 38400
#define B57600 57600
#define B115200 115200
#define B230400 230400
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#define INVALID_HANDLE_VALUE -1
#endif

#include <stdint.h>
#include <string.h>

#include <stdexcept>

#include "display.hpp"

#ifdef _WIN32
namespace {
std::string normalizeComPath(const std::string &port)
{
	if (port.size() < 3)
		return port;
	if (port.rfind("\\\\.\\", 0) == 0)
		return port;
	if (std::toupper(static_cast<unsigned char>(port[0])) != 'C' ||
			std::toupper(static_cast<unsigned char>(port[1])) != 'O' ||
			std::toupper(static_cast<unsigned char>(port[2])) != 'M')
		return port;
	return "\\\\.\\" + port;
}
}  // namespace
#endif

Uart_ll::Uart_ll(const std::string &filename, uint32_t clkHz,
		uint8_t byteSize, bool twoStopBits,
		uint8_t readTimeout, uint8_t writeTimeout):
		_serial(INVALID_HANDLE_VALUE), _byteSize(CS8),
		_filename(filename), _clkHz(clkHz),
		_twoStopBits(twoStopBits),
		_readTimeout(readTimeout), _writeTimeout(writeTimeout)
{
	switch (byteSize) {
		case 5:
			_byteSize = CS5;
			break;
		case 6:
			_byteSize = CS6;
			break;
		case 7:
			_byteSize = CS7;
			break;
		case 8:
			_byteSize = CS8;
			break;
		default:
			throw std::runtime_error("Error: byteSize must be between 5 and 8");
	}
}

Uart_ll::~Uart_ll() {
	disconnect();
}

bool Uart_ll::connect(bool open_quiet)
{
	if (_serial != INVALID_HANDLE_VALUE) {
		printError("Uart_ll: Serial port already open");
		return false;
	}

#ifdef _WIN32
	std::string path = normalizeComPath(_filename);
	_serial = CreateFileA(path.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			0,       /* no sharing */
			NULL,    /* default security */
			OPEN_EXISTING,
			0,       /* synchronous (non-overlapped) I/O */
			NULL);
	if (_serial == INVALID_HANDLE_VALUE) {
		if (!open_quiet)
			printError("Uart_ll: failed to open " + _filename +
				" (error " + std::to_string(GetLastError()) + ")");
		return false;
	}

#else
	/* open device in non-blocking mode first.
	 * On macOS, opening some tty devices in blocking mode can wait forever
	 * for carrier/line readiness.
	 */
	_serial = open(_filename.c_str(), (O_RDWR | O_NOCTTY | O_NONBLOCK));
	if (_serial == -1) {
		if (!open_quiet)
			printError("Uart_ll: failed to open " + _filename +
				" (" + strerror(errno) + ")");
		return false;
	}

	/* restore blocking mode: termios VMIN/VTIME drives read timeout behavior */
	int flags = fcntl(_serial, F_GETFL, 0);
	if (flags == -1 || fcntl(_serial, F_SETFL, (flags & ~O_NONBLOCK)) == -1) {
		int saved_errno = errno;
		close(_serial);
		_serial = -1;
		printError("Uart_ll: failed to configure file descriptor flags for "
				+ _filename + " (" + strerror(saved_errno) + ")");
		return false;
	}
#endif

	/* configure port */
	if (port_configure() < 0) {
#ifdef _WIN32
		CloseHandle(_serial);
#else
		close(_serial);
#endif
		_serial = INVALID_HANDLE_VALUE;
		printError("Uart_ll: port configuration failed");
		return false;
	}

	/* set baudrate */
	if (setClkFreq(_clkHz) < 0) {
		disconnect();
		printError("Uart_ll: baudrate configuration failed");
		return false;
	}

	return true;
}

bool Uart_ll::disconnect()
{
	if (_serial == INVALID_HANDLE_VALUE)
		return true;

#ifdef _WIN32
	/* reapply original configuration */
	SetCommState(_serial, &_prev_dcb);
	SetCommTimeouts(_serial, &_prev_timeouts);
	CloseHandle(_serial);
#else
	int err;
	/* reapply original configuration */
	if ((err = tcsetattr(_serial, TCSANOW, &_prev_termios)) != 0) {
		printError("Uart_ll: error %d from tcsetattr: " + std::to_string(err));
		return false;
	}

	/* close device */
	close(_serial);
#endif
	_serial = INVALID_HANDLE_VALUE;
	return true;
}

int Uart_ll::port_configure(void)
{
	int err = 0;

#ifdef _WIN32
	/* store current configuration */
	memset(&_prev_dcb, 0, sizeof(_prev_dcb));
	_prev_dcb.DCBlength = sizeof(_prev_dcb);
	if (!GetCommState(_serial, &_prev_dcb)) {
		printError("Uart_ll: error retrieving current configuration: " +
			std::to_string(GetLastError()));
		return -1;
	}

	/* store current timeouts */
	if (!GetCommTimeouts(_serial, &_prev_timeouts)) {
		printError("Uart_ll: error retrieving current timeouts: " +
			std::to_string(GetLastError()));
		return -1;
	}

	/* copy DCB and apply new settings */
	memcpy(&_curr_dcb, &_prev_dcb, sizeof(_prev_dcb));
	_curr_dcb.DCBlength = sizeof(_curr_dcb);
	_curr_dcb.ByteSize  = _byteSize;
	_curr_dcb.Parity    = NOPARITY;
	_curr_dcb.StopBits  = _twoStopBits ? TWOSTOPBITS : ONESTOPBIT;
	_curr_dcb.fBinary        = TRUE;   /* required on Windows */
	_curr_dcb.fParity        = FALSE;
	_curr_dcb.fOutxCtsFlow   = FALSE;
	_curr_dcb.fOutxDsrFlow   = FALSE;
	_curr_dcb.fDtrControl    = DTR_CONTROL_DISABLE;
	_curr_dcb.fDsrSensitivity = FALSE;
	_curr_dcb.fOutX          = FALSE;  /* no XON/XOFF output flow control */
	_curr_dcb.fInX           = FALSE;  /* no XON/XOFF input flow control  */
	_curr_dcb.fNull          = FALSE;  /* do not discard received null bytes */
	_curr_dcb.fErrorChar     = FALSE;
	_curr_dcb.fAbortOnError  = FALSE;
	_curr_dcb.fRtsControl    = RTS_CONTROL_DISABLE;

	if (!SetCommState(_serial, &_curr_dcb)) {
		printError("Uart_ll: error configuring port: " +
			std::to_string(GetLastError()));
		return -1;
	}

	/* set timeouts: equivalent to POSIX VMIN=0, VTIME=_readTimeout*10.
	 * MAXDWORD/MAXDWORD/X: return immediately with any bytes already in
	 * the buffer; if the buffer is empty wait up to X ms for the first
	 * byte.  This mirrors the Linux VTIME behaviour used on the other
	 * path. */
	COMMTIMEOUTS timeouts = {};
	timeouts.ReadIntervalTimeout        = MAXDWORD;
	timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
	timeouts.ReadTotalTimeoutConstant   = _readTimeout * 1000U;
	timeouts.WriteTotalTimeoutMultiplier = 0;
	timeouts.WriteTotalTimeoutConstant  = _writeTimeout * 1000U;
	if (!SetCommTimeouts(_serial, &timeouts)) {
		printError("Uart_ll: error configuring timeouts: " +
			std::to_string(GetLastError()));
		return -1;
	}

#else
	/* store current configuration */
	if ((err = tcgetattr(_serial, &_prev_termios)) != 0) {
		printError("error to retrieve current configuration: " +
				std::to_string(err) + " " + strerror(errno));
		return -1;
	}

	/* copy termios structure configuration */
	memcpy(&_curr_termios, &_prev_termios, sizeof(_prev_termios));

	/* controls flags */
	/* clear default configuration */
	_curr_termios.c_cflag &= ~(CSIZE | CSTOPB | CRTSCTS);
	/* set byte size (5,6,7,8)
	 * enable receiver
	 * disable modem control lines
	 */
	_curr_termios.c_cflag |= (_byteSize | CREAD | CLOCAL);
	/* 2 stop bits ? */
	if (_twoStopBits)
		_curr_termios.c_cflag |= CSTOPB;

	/* input mode: disable all input processing (raw mode) */
	_curr_termios.c_iflag = 0;

	/* output mode */
	_curr_termios.c_oflag = 0;  // no remapping, no delays

	// no canonical processing
	_curr_termios.c_lflag = 0;  // no signaling chars, no echo,

	_curr_termios.c_cc[VMIN] = 0;  // read doesn't block
	_curr_termios.c_cc[VTIME] = _readTimeout * 10;  // read timeout (tenths of second)

	if ((err = tcsetattr(_serial, TCSANOW, &_curr_termios)) != 0) {
		printError("error %d from tcsetattr: " + std::to_string(err) +
			" " + strerror(errno));
		return -1;
	}
#endif

	return 0;
}

int Uart_ll::setClkFreq(uint32_t clkHz)
{
#ifdef _WIN32
	_curr_dcb.BaudRate = (DWORD)freq_to_baud(clkHz);
	if (!SetCommState(_serial, &_curr_dcb)) {
		printError("Error during baudrate configuration: SetCommState == " +
			std::to_string(GetLastError()));
		return -1;
	}
	_clkHz = clkHz;
	return (int)clkHz;
#else
	int err;
	speed_t baud = freq_to_baud(clkHz);

	/* set input baudrate */
	cfsetispeed(&_curr_termios, baud);
	/* set output baudrate */
	cfsetospeed(&_curr_termios, baud);
	/* apply modifications */
	if ((err = tcsetattr(_serial, TCSANOW, &_curr_termios)) != 0) {
		printError("Error during baudrate configuration: tcsetattr == "
			+ std::to_string(err));
		return -1;
	}
	_clkHz = clkHz;
	_baudrate = baud;
	return clkHz;
#endif
}

uint32_t Uart_ll::getClkFreq()
{
#ifdef _WIN32
	return _curr_dcb.BaudRate;
#else
	return baud_to_freq(cfgetispeed(&_curr_termios));
#endif
}

int Uart_ll::write(const unsigned char *data, int size)
{
#ifdef _WIN32
	const char *ptr = reinterpret_cast<const char *>(data);
	DWORD remaining = (DWORD)size;

	while (remaining > 0) {
		DWORD written = 0;
		if (!WriteFile(_serial, ptr, remaining, &written, NULL)) {
			printError("Error: Failed to write: " +
				std::to_string(GetLastError()));
			return -1;
		}
		if (written == 0)
			return -3;  /* write timeout */
		ptr += written;
		remaining -= written;
	}
	/* Flush the transmit buffer so all bytes are physically sent to the
	 * device before the caller issues the next read. */
	FlushFileBuffers(_serial);
	return size;
#else
	const char *ptr = reinterpret_cast<const char *>(data);
	ssize_t len = size;
	/* set a select to block for serial data or timeout */
	fd_set fd_write;

	do {
		ssize_t s = ::write(_serial, ptr, len);
		if (s < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				printError("Error: Failed to write: " + std::to_string(errno) +
					" " + strerror(errno));
				return s;
			}
			s = 0;
		} else if (s == len) {
			break;
		}

		ptr += s;
		len -= s;

		FD_ZERO(&fd_write);
		FD_SET(_serial, &fd_write);
		timespec timeout_ts;
		timeout_ts.tv_sec = _writeTimeout;
		timeout_ts.tv_nsec = 0;
		int r = pselect(_serial + 1, NULL, &fd_write, NULL, &timeout_ts, NULL);

		if (r < 0) {
			int t = errno;
			printf("uart_ll errror: write failed with error %d (%d: %s)\n", r,
					errno, strerror(errno));
			// interrupt ?
			return (t == EINTR) ? -1 : -2;
		}

		/* timeout */
		if (r == 0)
			return -3;

		if (!FD_ISSET(_serial, &fd_write)) {
			printError(
				"uart_ll error: something to write but from file descriptor\n");
			return -4;
		}
	} while (len > 0);

	return size;
#endif
}

int Uart_ll::read(std::string *buf, int maxsize)
{
	buf->resize(maxsize);
#ifdef _WIN32
	DWORD read_size = 0;
	DWORD len = (DWORD)maxsize;

	do {
		DWORD ret = 0;
		if (!ReadFile(_serial, &(*buf)[read_size], len, &ret, NULL)) {
			char mess[256];
			snprintf(mess, 256, "Error: Failed to read: %lu",
				(unsigned long)GetLastError());
			printError(mess);
			buf->resize(read_size);
			return -1;
		}
		if (ret == 0)
			break;  /* timeout: no more data */
		read_size += ret;
		len -= ret;
	} while (read_size < (DWORD)maxsize);

#else
	int read_size = 0;
	int burst_size = maxsize;
	int len = maxsize;

	do {
		if (burst_size > len)
			burst_size = len;
		ssize_t ret = ::read(_serial, &(*buf)[read_size], burst_size);
		if (ret < 0) {
			char mess[256];
			snprintf(mess, 256, "Error: Failed to read: %d %s",
					errno, strerror(errno));
			printError(mess);
			buf->resize(read_size);
			return ret;
		}
		if (ret == 0) {
			/* VTIME timeout: stop waiting for more bytes */
			break;
		}
		read_size += ret;
		len -= ret;
	} while (read_size < maxsize);
#endif

	buf->resize(read_size);
	return read_size;
}

bool Uart_ll::flush(int maxsize)
{
#ifdef _WIN32
	(void)maxsize;
	/* Discard all data currently in the receive buffer. */
	PurgeComm(_serial, PURGE_RXCLEAR);
	return true;
#else
	int size = maxsize;
	char c;
	int timeout = 1000;
	std::string buf;

	do {
		ssize_t ret = ::read(_serial, &c, 1);
		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return true;
			char mess[256];
			snprintf(mess, 256, "Error: Failed to read: %d %s",
					errno, strerror(errno));
			printError(mess);
			return false;
		} else if (ret == 0) {
			timeout--;
		} else {
			buf.append(1, c);
			size--;
		}
	} while (size > 0 && timeout > 0);

	return true;
#endif
}

int Uart_ll::read_until(std::string *buf, uint8_t end)
{
#ifdef _WIN32
	int read_size = 0;
	bool is_end = false;
	char c;

	do {
		DWORD ret = 0;
		if (!ReadFile(_serial, &c, 1, &ret, NULL)) {
			printError("uart_ll error: read failed (error " +
				std::to_string(GetLastError()) + ")");
			return -1;
		}
		if (ret == 0)
			return -3;  /* timeout */
		if (c == '\r' && end == '\n')
			continue;
		if (c == (char)end) {
			is_end = true;
		} else {
			buf->append(1, c);
			read_size++;
		}
	} while (!is_end);

	return read_size;
#else
	int read_size = 0;
	char c[256];
	bool is_end = false;
	/* set a select to block for serial data or timeout */
	fd_set fd_read;
	do {
		FD_ZERO(&fd_read);
		FD_SET(_serial, &fd_read);
		timespec timeout_ts;
		timeout_ts.tv_sec = 5;
		timeout_ts.tv_nsec = 0;
		int r = pselect(_serial + 1, &fd_read, NULL, NULL, &timeout_ts, NULL);

		if (r < 0) {
			int t = errno;
			printf("uart_ll errror: read failed with error %d (%d: %s)\n", r,
					errno, strerror(errno));
			if (t == EINTR)  // interrupt
				return -1;
			return -2;
		}

		/* timeout */
		if (r == 0)
			return -3;

		if (!FD_ISSET(_serial, &fd_read)) {
			printError(
				"uart_ll error: something to read but from file descriptor\n");
			return -4;
		}

		int ret = ::read(_serial, c, 256);
		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {  // temporarily unavailable
				printError("uart_ll error: device unavailable\n");
				return -5;
			}
			printError("uart_ll error: read failed " + std::to_string(errno) +
				" " + strerror(errno));
			return ret;
		}

		for (int i = 0; i < ret; i++) {
			if (c[i] == '\r' && end == '\n')
				continue;
			if (c[i] == end) {
				is_end = true;
				break;
			}
			buf->append(1, c[i]);
			read_size++;
		}
	} while (!is_end);

	return read_size;
#endif
}

speed_t Uart_ll::freq_to_baud(uint32_t clkHz)
{
	if (clkHz > 115200)
		return B230400;
	else if (clkHz > 57600)
		return B115200;
	else if (clkHz > 38400)
		return B57600;
	else if (clkHz > 19200)
		return B38400;
	else if (clkHz > 9600)
		return B19200;
	else if (clkHz > 4800)
		return B9600;
	else if (clkHz > 2400)
		return B4800;
	else  // no exhaustive list
		return B2400;
}

uint32_t Uart_ll::baud_to_freq(speed_t baud)
{
	switch(baud) {
	case B230400:
		return 230400;
	case B115200:
		return 115200;
	case B57600:
		return 57600;
	case B38400:
		return 38400;
	case B19200:
		return 19200;
	case B9600:
		return 9600;
	case B4800:
		return 4800;
	default:  // no exhaustive list
		return 2400;
	}
}
