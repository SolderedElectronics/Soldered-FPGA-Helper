#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "cable.hpp"
#include "display.hpp"
#include "httplib.h"
#ifndef WIN32
#include "libusb_ll.hpp"
#endif
#include "rawParser.hpp"
#include "spiFlash.hpp"
#include "tinyprog.hpp"
#include "uart_ll.hpp"

#ifdef WIN32
#include "windows_stuff.hpp"
#endif

#define TINYPROG_INTF    1
#define TINYPROG_CTRL_INTF 0
#define TINYPROG_WRITE_EP 0x01
#define TINYPROG_READ_EP  0x81

/* TinyFPGA BX default address map, used when the board's SPI flash
 * security registers hold no bootmeta (blank/erased). 0x028000 is
 * the actual SB_WARMBOOT target read out of this board's own
 * multiboot header (verified against real hardware: bitstreams
 * written at the README's illustrative 0x30000 never booted).
 * userimage sized generously for real ICE40LP8K bitstreams (~135KB),
 * userdata gets the trailing 64KB of a 1MB (8Mbit) flash.
 */
#define BX_DEFAULT_BOOTLOADER_ADDR  0x000000
#define BX_DEFAULT_BOOTLOADER_EADDR 0x028000
#define BX_DEFAULT_USERIMAGE_ADDR   0x028000
#define BX_DEFAULT_USERIMAGE_EADDR  0x0f0000
#define BX_DEFAULT_USERDATA_ADDR    0x0f0000
#define BX_DEFAULT_USERDATA_EADDR   0x100000

TinyProg::TinyProg(std::string filename, const cable_t &cable,
	const uint32_t baudrate, const bool detect, int8_t verbose):
	_filename(filename), _baudrate(baudrate), _verbose(verbose), _out_file(),
	_uart(nullptr),
#ifdef __APPLE__
	_usb(nullptr),
#endif
	_flash(nullptr)
{
	/* Detect cable/board */
	if (detect) {
		detect_cable(cable);
		return;
	}
	std::cout << filename << std::endl;

	/* When the tty device is not provided by the user
	 * analyze usb devices to search for one or more compatible
	 * devices based on VID/PID/bus id / device number
	 */
	if (filename.empty()) {
#ifdef WIN32
		std::vector<com_port_info_t> dev_path = list_com_ports(&cable);
		int found = static_cast<int>(dev_path.size());
#else
		libusb_ll usb(0, 0, verbose);
		int found = usb.get_devices_list(&cable);
#endif
		printf("found %d devices\n", found);

		/* Detected devices must be exactly one */
		if (found != 1) {
			if (found == 0)
				throw std::runtime_error("TinyProg: no cable detected");
#ifndef WIN32
			usb.scan();
#else
			display_com_port(dev_path);
#endif
			std::ostringstream mess;
			mess << "TinyProg: too many devices found: please use:\n";
			mess << "- --device /dev/ttyACMx or\n";
			mess << "- --busdev-num with bus_num and device addr";
			throw std::runtime_error(mess.str());
		}

		/* Only one: get dev_path from usb device */
#ifndef WIN32
		std::vector<std::string> dev_path = usb.get_device_path();
#endif
		printf("%ld\n", dev_path.size());
		if (dev_path.size() != 1)
			throw std::runtime_error("TinyProg: error during device path get");
		/* First element */
#ifndef WIN32
		_filename = filename = dev_path[0];
#else
		_filename = filename = dev_path[0].port_name;
#endif
		/* But must contains a valid path */
		if (filename.empty())
			throw std::runtime_error("TinyProg: no device found");
	} /* end filename.empty() */

	if (!connect(_filename))
		throw std::runtime_error("TinyProg: Failed to open device " + filename);
}

TinyProg::~TinyProg()
{
	disconnect();
}

bool TinyProg::connect(const std::string &filename)
{
	/* Start in a stable/known state */
	disconnect();

#ifdef __APPLE__
	_usb = new libusb_ll(0, 0, _verbose);
	int vid = 0x1d50;
	int pid = 0x6130;
	cable_details_t dev;
	if (!_usb->get_device_info_from_path(filename, dev)) {
		printError("Error: failed to get info from path " + filename);
		return EXIT_FAILURE;
	}
	if (_verbose)
		printf("VID %x PID %x bus addr %x device addr %x\n",
			dev.vid, dev.pid,
			dev.bus, dev.device);

	if (!_usb->connect(dev.vid, dev.pid, TINYPROG_INTF)) {
		printError("Fails to connect");
		return EXIT_FAILURE;
	}
#else
	/* Open uart */
	printInfo("Open " + filename + ":");
	try {
		_uart = new Uart_ll(filename, _baudrate, 8, false);
	} catch (std::exception &e) {
		printError(e.what());
		_uart = nullptr;
		return false;
	}

	if (!_uart->connect()) {
		printError("TinyProg: Fail to open " + filename);
		disconnect();
		return false;
	}
#endif
	printSuccess("Done");

	/* Create Flash submodule */
	printInfo("Create SpiFlash sub-system");
	try {
		_flash = new SPIFlash(this, false, _verbose);
	} catch (std::exception &e) {
		printError(e.what());
		disconnect();
		return false;
	}
	printSuccess("Done");

	/* Read Meta from Flash Security registers */
	if (!read_meta()) {
		printError("TinyProg: Meta access / Parse JSON failed");
		disconnect();
		return false;
	}
	return true;
}

void TinyProg::disconnect()
{
	if (_flash) {
		delete _flash;
		_flash = nullptr;
	}

	if (_uart) {
		_uart->disconnect();
		delete _uart;
		_uart = nullptr;
	}
	if (_usb) {
		_usb->disconnect();
		delete _usb;
		_usb = nullptr;
	}
}

void TinyProg::detect_cable(const cable_t &cable)
{
	// FIXME: must add details about device VID/PID/..

	/* Get Device list matching VID/PID/Bus number/Device addr */
#ifndef WIN32
	libusb_ll usb(0, 0, _verbose);
	int found = usb.get_devices_list(&cable);
#else
	std::vector<com_port_info_t> dev_path = list_com_ports(&cable);
	int found = static_cast<int>(dev_path.size());
#endif
	printf("found %d devices\n", found);

	if (found == 0) {
		printError("TinyProg: no devices detected");
		return;
	}

	/* For all found devices: convert to /dev path */
#ifndef WIN32
	std::vector<std::string> dev_path = usb.get_device_path();
#endif
	for (size_t i = 0; i < dev_path.size(); i++) {
		printf("\n");
#ifndef WIN32
		const std::string &filename = dev_path[i];
#else
		const std::string &filename = dev_path[i].port_name;
#endif
		/* must contains a valid path */
		if (filename.empty()) {
			printError("TinyProg: no path found");
			continue;
		}
		if (!connect(filename)) {
			printError("TinyProg: Can't open " + filename);
			continue;
		}
		display_meta();
		detect_flash();
		disconnect();
	}
}

bool TinyProg::detect_flash()
{
	printf("\n");
	printInfo("Detect flash:");
	_flash->read_id();
	_flash->display_status_reg();
	printInfo("Done");
	return true;
}

bool TinyProg::dump(uint32_t base_addr, uint32_t len)
{
	if (_out_file.empty()) {
		printError("TinyProg: dump: no output filename provided");
		return false;
	}
	return _flash->dump(_out_file, base_addr, len, 256);
}

bool TinyProg::boot()
{
	const uint8_t opcode = 0x00;
	if (write(&opcode, 1) != 1) {
		printError("Error: uart write failed");
		return false;
	}
	return true;
}

bool TinyProg::meta_extract_start_and_end_addr(const std::string &meta,
	uint32_t &addr, uint32_t &eaddr)
{
	// Expected format: 0x000a0-0x28000
	if (meta.empty()) {
		printError("TinyProg: Empty address map entry");
		return false;
	}
	size_t delimPos = meta.find('-');
	if (delimPos == std::string::npos) {
		printError("TinyProg: Delimiter '-' not found in string " + meta);
		return false;
	}
	std::string addrStr = meta.substr(0, delimPos);
	std::string endStr = meta.substr(delimPos + 1);
	if (addrStr.empty() || endStr.empty()) {
		printError("TinyProg: Invalid address range format: " + meta);
		return false;
	}
	try {
		size_t consumed = 0;
		uint32_t start = static_cast<uint32_t>(std::stoul(addrStr, &consumed, 16));
		if (consumed != addrStr.size()) {
			printError("TinyProg: Invalid start address: " + addrStr);
			return false;
		}
		consumed = 0;
		uint32_t end = static_cast<uint32_t>(std::stoul(endStr, &consumed, 16));
		if (consumed != endStr.size()) {
			printError("TinyProg: Invalid end address: " + endStr);
			return false;
		}
		addr = start;
		eaddr = end;
	} catch (const std::exception &e) {
		printError(std::string("TinyProg: Failed to parse address range '") + meta + "': " + e.what());
		return false;
	}

	return true;
}

void TinyProg::display_meta()
{
	char mess[256];
	/* displays boardmeta attributes */
	static const std::map<std::string, std::vector<std::string>> meta_keys = {
		{"boardmeta", {"name", "hver", "fpga", "uuid"}},
		{"bootmeta", {"bootloader", "bver", "update"}},
	};

	printf("\n");
	for (auto &meta: meta_keys) {
		const auto &fk = meta.first;
		if (fk == "boardmeta")
			printSuccess("Board Meta:");
		else
			printSuccess("Boot Meta:");
		for (auto &key: meta.second) {
			std::string k = meta.first + "." + key;
			std::string v = _meta.get_value_from_key(k);
			snprintf(mess, 256, "\t%s: %s", key.c_str(), v.c_str());
			printInfo(mess);
		}
	}

	printSuccess("Address Map:");
	snprintf(mess, 256, "\tBootloader addr: 0x%08x end addr: 0x%08x",
		_bootloader_addr, _bootloader_eaddr);
	printInfo(mess);
	snprintf(mess, 256, "\tUserImage  addr: 0x%08x end addr: 0x%08x",
		_userimage_addr, _userimage_eaddr);
	printInfo(mess);
	snprintf(mess, 256, "\tUserData   addr: 0x%08x end addr: 0x%08x",
		_userdata_addr, _userdata_eaddr);
	printInfo(mess);
}

bool TinyProg::read_meta()
{
	uint8_t security_page_cnt[256];
	/* Clear meta parser to avoid accumulates lines
	 * when detect is used
	 */
	_meta.clear();

	/* Security registers pages */
	for (uint8_t page = 1; page <= 3; page++) {
		if (!_flash->read_security_register(page, security_page_cnt, 255)) {
			printError("TinyProg: Failed to read Security Register page " +
				std::to_string(page));
			return false;
		}
		security_page_cnt[255] = '\0';
		_meta.append(std::string((const char *)security_page_cnt));
	}

	if (!_meta.parse()) {
		printError("TinyProg: Failed to parse meta");
		return false;
	}

	/* needs to retrieves:
	 * - bootloader addr/eaddr
	 * - userimage addr/eaddr
	 * - userdata addr/eaddr
	 */
	std::string bootloader = _meta.get_value_from_key("bootmeta.addrmap.bootloader");
	std::string userimage = _meta.get_value_from_key("bootmeta.addrmap.userimage");
	std::string userdata = _meta.get_value_from_key("bootmeta.addrmap.userdata");

	if (bootloader.empty() && userimage.empty() && userdata.empty()) {
		/* No bootmeta at all (blank/erased security registers):
		 * fall back to TinyFPGA BX defaults instead of failing.
		 */
		printWarn("TinyProg: no bootmeta found on this board, "
			"using TinyFPGA BX default address map");
		_bootloader_addr  = BX_DEFAULT_BOOTLOADER_ADDR;
		_bootloader_eaddr = BX_DEFAULT_BOOTLOADER_EADDR;
		_userimage_addr   = BX_DEFAULT_USERIMAGE_ADDR;
		_userimage_eaddr  = BX_DEFAULT_USERIMAGE_EADDR;
		_userdata_addr    = BX_DEFAULT_USERDATA_ADDR;
		_userdata_eaddr   = BX_DEFAULT_USERDATA_EADDR;
	} else {
		if (!meta_extract_start_and_end_addr(bootloader, _bootloader_addr, _bootloader_eaddr))
			return false;
		if (!meta_extract_start_and_end_addr(userimage, _userimage_addr, _userimage_eaddr))
			return false;
		if (!meta_extract_start_and_end_addr(userdata, _userdata_addr, _userdata_eaddr))
			return false;
	}

	if (_verbose > 0) {
		display_meta();
	}

	return true;
}

bool TinyProg::send_opcode(uint8_t opcode, uint8_t *rx, uint16_t rx_len)
{
	return build_send_cmd(&opcode, 1, rx, rx_len);
}

/*
 * packet structure
 * 0x01 + LSB(tx_len) + MSB(tx_len) + LSB(rx_len) + MSB(rx_len) + payload
 * payload constains SPI instruction:
 * opcode + [ADDR] + [DATA]
 */

bool TinyProg::build_send_cmd(const uint8_t *tx, const uint16_t tx_len,
	uint8_t *rx, uint16_t rx_len)
{
	uint16_t kBuffSize = 0;
	std::vector<uint8_t> buffer(tx_len + 1 + 4);

	buffer[kBuffSize++] = 0x01;
	/* write len */
	buffer[kBuffSize++] = ((tx_len >> 0) & 0xff);
	buffer[kBuffSize++] = ((tx_len >> 8) & 0xff);
	/* read len */
	buffer[kBuffSize++] = ((rx_len >> 0) & 0xff);
	buffer[kBuffSize++] = ((rx_len >> 8) & 0xff);

	/* payload */
	if (tx) {
		memcpy(buffer.data() + kBuffSize, tx, tx_len);
		kBuffSize += tx_len;
	}

	if (_verbose > 1) {
		for (int i = 0; i < kBuffSize; i++)
			printf("%d %02x\n", i, buffer[i]);
		printf("\n");
	}

	if (write(buffer.data(), kBuffSize) != kBuffSize) {
		printError("TinyProg: build_send_cmd: UART write failed");
		return false;
	}

	if (rx_len > 0) {
		std::string rx_buf;
		if (read(&rx_buf, rx_len) < rx_len)
			return false;
		memcpy(rx, rx_buf.c_str(), rx_len);
		if (_verbose > 1) {
			for (int i = 0; i < rx_len; i++)
				printf("%d %02x\n", i, rx[i]);
			printf("\n");
		}
	}
	return true;
}

int TinyProg::write(const unsigned char *data, int size)
{
	int wr_len;
#ifdef __APPLE__
	if (!_usb->bulk_write(TINYPROG_WRITE_EP, data, size,
		&wr_len, 1000))
		return -1;
#else
	wr_len = _uart->write(data, size);
#endif
	return wr_len;
}

int TinyProg::read(std::string *data, int maxsize)
{
	int rd_len;
#ifdef __APPLE__
	data->resize(maxsize);
	if (!_usb->bulk_read(TINYPROG_READ_EP,
			reinterpret_cast<uint8_t *>(&(*data)[0]), maxsize,
			&rd_len, 1000)) {
		data->resize(0);
		return -1;
	}
	data->resize(rd_len);
#else
	rd_len = _uart->read(data, maxsize);
#endif
	return rd_len;
}

int TinyProg::spi_put(uint8_t cmd, const uint8_t *tx, uint8_t *rx,
	uint32_t len)
{
	if (_verbose > 1)
		printf("%x rx: %d tx: %d len: %d\n", cmd, rx != nullptr, tx != nullptr, len);
	uint32_t tx_len = (tx) ? len: 0;
	std::vector<uint8_t> buffer(tx_len + 1);
	buffer[0] = cmd;
	if (tx)
		memcpy(buffer.data() + 1, tx, len);
	return build_send_cmd(buffer.data(), static_cast<uint16_t>(buffer.size()), rx, (rx)?static_cast<uint16_t>(len):0) ? 0 : 1;
}

int TinyProg::spi_put(uint8_t cmd, const uint8_t *tx, const uint32_t tx_len,
	uint8_t *rx, const uint32_t rx_len)
{
	if (_verbose > 1)
		printf("%x rx: %d tx: %d len: %d %d\n", cmd, rx != nullptr, tx != nullptr, tx_len, rx_len);
	std::vector<uint8_t> buffer(tx_len + 1);
	buffer[0] = cmd;
	if (tx)
		memcpy(buffer.data() + 1, tx, tx_len);
	return build_send_cmd(buffer.data(), static_cast<uint16_t>(buffer.size()), rx, (rx)?static_cast<uint16_t>(rx_len):0) ? 0 : 1;
}

int TinyProg::spi_put(const uint8_t *tx, uint8_t *rx,
	uint32_t len)
{
	return 0;
}

int TinyProg::spi_wait(uint8_t cmd, uint8_t mask, uint8_t cond,
	uint32_t timeout, bool verbose)
{
	char mess[256];
	uint8_t rx;
	uint32_t count = 0;

	do {
		send_opcode(cmd, &rx, 1);
		count ++;
		if (count == timeout)
			break;

		if (verbose) {
			snprintf(mess, 256, "%02x %02x %02x %02x", rx, mask, cond, count);
			printInfo(mess);
		}
	} while((rx & mask) != cond);

	if (count == timeout) {
		snprintf(mess, 256, "TinyProg:spi_wait: Timeout %02x", rx);
		printError(mess);
		return -ETIME;
	}

	return 0;
}

bool TinyProg::program_bitstream(uint32_t addr, const uint8_t *data, uint32_t len, const bool verify)
{
	char mess[64];
	snprintf(mess, sizeof(mess), "%u bytes to program at 0x%08x -> 0x%08x", len, addr, addr+len);
	printInfo(mess);

	if (_flash->erase_and_prog(static_cast<int>(addr), data,
			static_cast<int>(len)) != 0) {
		printError("TinyProg: programming failed");
		return false;
	}

	if (verify) {
		if (!_flash->verify(static_cast<int>(addr), data, static_cast<int>(len), 255)) {
			printError("TinyProg: verification failed");
			return false;
		}
	}

	printSuccess("TinyProg: programming successful");
	return true;
}

bool TinyProg::program_area(const std::string &filename, const std::string &area_name,
	const uint32_t addr, const uint32_t meta_addr, const uint32_t meta_eaddr,
	const bool verify)
{
	std::unique_ptr<RawParser> bit;
	try {
		bit.reset(new RawParser(filename, false));
	} catch (const std::runtime_error &e) {
		printError("TinyProg: failed to open " + area_name + " file: " + e.what());
		return false;
	}

	printInfo("Parse " + area_name + " file ", false);
	if (bit->parse() == EXIT_SUCCESS) {
		printSuccess("DONE");
	} else {
		printError("FAIL");
		return false;
	}

	/* Get configuration data and length from the Parser */
	const uint8_t *data = bit->getData();
	uint32_t length = bit->getLength() / 8;

	/* Get addr from the user parameter or via meta area */
	const uint32_t area_addr = (addr > 0) ? addr : meta_addr;

	/* FIXME: padding must be applied to have a bitstream multiple of
	 * 256 Bytes
	 */

	/* Check bitstream fits within the area */
	if (area_addr + length > meta_eaddr) {
		char mess[128];
		snprintf(mess, sizeof(mess),
			"TinyProg: bitstream (0x%08x + 0x%08x) overflows %s area "
			"[0x%08x - 0x%08x]",
			area_addr, length, area_name.c_str(), meta_addr, meta_eaddr);
		printError(mess);
		return false;
	}

	return program_bitstream(area_addr, data, length, verify);
}

void TinyProg::split_url(const std::string &url, std::string &base,
	std::string &path, std::string &filename)
{
	size_t pos = url.find('/', 8); // skip "https://"
	base = url.substr(0, pos);
	path = url.substr(pos);
	filename = url.substr(url.find_last_of('/') + 1);
}

bool TinyProg::fetch_data(const std::string &url, std::vector<uint8_t> &data)
{
	std::string base, path, filename;

	// 1. Extract base url, path and filename from url
	split_url(url, base, path, filename);

	char mess[256];
	snprintf(mess, 256, "Fetch %s from %s%s",
		filename.c_str(), base.c_str(), path.c_str());
	printInfo(mess);

	// 2. Create a Client to fetch the file
	httplib::Client cli(base);
	cli.set_follow_location(true);  // required with github
	auto res = cli.Get(path);
	if (!res) {
		printError("TinyProg: fetch_data: failed to fetch " + url);
		return false;
	}
	if (res->status != 200) {
		printError("TinyProg: fetch_data: HTTP status " +
			std::to_string(res->status) + " for " + url);
		return false;
	}

	snprintf(mess, 256, "status: %d size: %zu", res->status, res->body.size());
	printInfo(mess);

	const auto &body = res->body;
	data.assign(reinterpret_cast<const uint8_t *>(body.data()),
		reinterpret_cast<const uint8_t *>(body.data()) + body.size());
	return true;
}


/* Perform bootloader update
 */
bool TinyProg::bootloader_update(const std::string &stage_one_file,
	const std::string &stage_two_file)
{
	std::vector<uint8_t> stage1_data, stage2_data;

	if (stage_one_file.empty() || stage_two_file.empty()) {
		/* Fetch manifest from the URL stored in flash metadata */
		std::string url = _meta.get_value_from_key("bootmeta.update");
		if (url.empty()) {
			printError("TinyProg: missing bootmeta.update URL in metadata");
			return false;
		}
		std::string base_url, url_path, filename;
		split_url(url, base_url, url_path, filename);
		url_path += "/bootloader.json";

		/* Create HTTPs client and fetch json file */
		httplib::Client cli(base_url);
		cli.set_follow_location(true);
		auto res = cli.Get(url_path);
		if (!res) {
			printError("TinyProg: failed to fetch bootloader manifest");
			return false;
		}
		if (res->status != 200) {
			printError("TinyProg: bootloader manifest HTTP status " +
				std::to_string(res->status));
			return false;
		}
		if (_verbose > 1) {
			std::cout << res->status << std::endl;
			std::cout << res->body << std::endl;
		}

		/* Create a JSONParser to extract informations from bootloader.json */
		JSONParser js;
		js.append(res->body);
		if (!js.parse()) {
			printError("TinyProg: failed to parse bootloader manifest");
			return false;
		}
		if (_verbose > 1)
			js.display();

		/* Get bitstreams URL */
		std::string stage_one_url = js.get_value_from_key("stage_one_url");
		std::string stage_two_url = js.get_value_from_key("stage_two_url");
		if (stage_one_url.empty() || stage_two_url.empty()) {
			printError("TinyProg: bootloader manifest is missing stage URLs");
			return false;
		}

		if (!fetch_data(stage_one_url, stage1_data))
			return false;
		if (!fetch_data(stage_two_url, stage2_data))
			return false;
	} else {
		/* Parse and bounds-check stage one (userimage section) */
		std::unique_ptr<RawParser> bit1;
		try {
			bit1.reset(new RawParser(stage_one_file, false));
		} catch (const std::exception &e) {
			printError("TinyProg: failed to open stage one file: " +
				std::string(e.what()));
			return false;
		}
		if (bit1->parse() != EXIT_SUCCESS) {
			printError("TinyProg: failed to parse stage one file");
			return false;
		}
		uint32_t len1 = bit1->getLength() / 8;
		if (_userimage_addr + len1 > _userimage_eaddr) {
			char mess[128];
			snprintf(mess, sizeof(mess),
				"TinyProg: stage one (0x%08x + 0x%08x) overflows "
				"userimage area [0x%08x - 0x%08x]",
				_userimage_addr, len1, _userimage_addr, _userimage_eaddr);
			printError(mess);
			return false;
		}
		stage1_data.assign(bit1->getData(), bit1->getData() + len1);

		/* Parse and bounds-check stage two (bootloader section, starts at 0) */
		std::unique_ptr<RawParser> bit2;
		try {
			bit2.reset(new RawParser(stage_two_file, false));
		} catch (const std::exception &e) {
			printError("TinyProg: failed to open stage two file: " +
				std::string(e.what()));
			return false;
		}
		if (bit2->parse() != EXIT_SUCCESS) {
			printError("TinyProg: failed to parse stage two file");
			return false;
		}
		uint32_t len2 = bit2->getLength() / 8;
		if (len2 > _userimage_eaddr) {
			char mess[128];
			snprintf(mess, sizeof(mess),
				"TinyProg: stage two (0x%08x) overflows bootloader "
				"area [0x00000000 - 0x%08x]",
				len2, _userimage_eaddr);
			printError(mess);
			return false;
		}
		stage2_data.assign(bit2->getData(), bit2->getData() + len2);
	}

	/* Write stage one into userimage section */
	if (!program_bitstream(_userimage_addr, stage1_data.data(),
				static_cast<uint32_t>(stage1_data.size()), true))
		return false;

	/* Send boot command to jump to the new bitstream */
	if (!boot())
		return false;

	delete _flash;
	_flash = nullptr;
	/* Close device */
	_uart->disconnect();

	/* Wait until device is ready to reconnect */
	printInfo("TinyProg: Wait for cable ready to reconnect (May take time)");
	uint32_t timeout;
	for (timeout = 0; timeout < 1000; timeout++) {
		sleep(1);
		// try to reopen the device
		if (_uart->connect(true))
			break;
	}
	if (timeout == 1000) {
		printError("Fail");
		printError("TinyProg: timeout after booting stage_one image");
		return false;
	}
	printSuccess("Done");
	sleep(1);

	/* Re-open the flash */
	printInfo("Create SpiFlash sub-system");
	try {
		_flash = new SPIFlash(this, false, _verbose);
	} catch (std::exception &e) {
		printError(e.what());
		return false;
	}
	printSuccess("Done");

	/* Write stage two (bootloader) at the start of flash */
	if (!program_bitstream(0, stage2_data.data(),
				static_cast<uint32_t>(stage2_data.size()), true))
		return false;

	/* Send boot command to jump to the new bootloader */
	return boot();
}

/* This method only handles userdata/userimage
 * Three possible use case:
 * - userimage: FPGA bitstream. If userimage_addr and userimage_addr_valid
 *   this information is used, otherwise bootmeta.addrmap.userimage is used
 * - userdata: User flash area. Same thing than userimage.
 * - combined userimage+userdata. Same thing too but start with userimage_addr (or overriden value)
 *   and stop at userdata location.
 * In all cases bitstream size is compared to the allowed area.
 * Note: This method only uses RawParser to handle bitstream => requires to implements
 * mcsParser uses too.
 */
bool TinyProg::program(const std::string& userimage_file,
		const std::string& userdata_file,
		uint32_t userimage_addr,
		uint32_t userdata_addr,
		bool combined_bitstream, const bool verify)
{
	/* User Image area */
	if (!userimage_file.empty() && !combined_bitstream) {
		if (!program_area(userimage_file, "User Image",
			userimage_addr, _userimage_addr, _userimage_eaddr, verify))
			return false;
	}

	/* User Data area */
	if (!userdata_file.empty()) {
		if (!program_area(userdata_file, "User Data",
			userdata_addr, _userdata_addr, _userdata_eaddr, verify))
			return false;
	}

	/* User Image+Data area (combined) */
	if (!userimage_file.empty() && combined_bitstream) {
		if (!program_area(userimage_file, "User Image + Data",
			userimage_addr, _userimage_addr, _userdata_eaddr, verify))
			return false;
	}

	return true;
}
