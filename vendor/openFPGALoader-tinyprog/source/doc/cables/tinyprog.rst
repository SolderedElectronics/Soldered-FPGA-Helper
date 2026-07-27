.. _tinyprog:

TinyProg notes
##############

The **TinyProg** cable driver implements communication with the
`TinyFPGA bootloader <https://github.com/tinyfpga/TinyFPGA-Bootloader>`_.

To use this driver, you must explicitly select it on the command line with
either ``-c tinyprog``/``--cable tinyprog`` or the board shortcut
``-b tinyFPGABX``. Without one of these options, ``openFPGALoader`` will not
use the TinyProg cable/driver.

Device selection
----------------

There are three ways to specify the TinyProg device to use. On Windows,
only option 2 (``-d COMx``) is supported.

**1. Automatic detection (no option required) -- Linux/macOS only**

When only one TinyProg device is connected, the device path is resolved
automatically via USB and no additional device-selection option is needed:

.. code-block:: bash

    openFPGALoader -b tinyFPGABX <bitfile>.bit

.. NOTE::
  Automatic detection only works when exactly one TinyProg device is
  connected. If multiple devices are present, use one of the options below.

.. NOTE::
  Automatic detection is **not available on Windows**. Windows does not
  provide a portable way to map a USB VID/PID to a ``COMx`` port number.
  Use ``-d COMx`` instead (see option 2).

**2. By serial device path (``-d``/``--device``)**

On Linux, the device appears as ``/dev/ttyACMx``:

.. code-block:: bash

    openFPGALoader -b tinyFPGABX -d /dev/ttyACM0 <bitfile>.bit

On Windows, the device appears as a ``COMx`` port (check Device Manager
under *Ports (COM & LPT)* to find the assigned number):

.. code-block:: bat

    openFPGALoader -b tinyFPGABX -d COM3 <bitfile>.bit

For ``COM10`` and higher the path is accepted with or without the
``\\.\`` prefix -- ``openFPGALoader`` adds it automatically when needed:

.. code-block:: bat

    openFPGALoader -b tinyFPGABX -d COM12 <bitfile>.bit

**3. By USB bus and device number (``--busdev-num``) -- Linux/macOS only**

Use ``--scan-usb`` to find the bus and device numbers:

.. code-block:: bash

    openFPGALoader --scan-usb
    empty
    Bus device vid:pid       probe_type manufacturer serial product
    001 004    0x1d50:0x6130 tinyprog   none         none   none

Then pass them as ``bus_num:device_addr``:

.. code-block:: bash

    openFPGALoader -b tinyFPGABX --busdev-num 1:4 <bitfile>.bit

.. NOTE::
  ``--busdev-num`` is **not available on Windows**. Windows does not provide
  a portable way to map a USB bus/device address to a ``COMx`` port number.
  Use ``-d COMx`` instead.

Programming
-----------

Program a bitstream into the ``userimage`` section:

.. code-block:: bash

    openFPGALoader -b tinyFPGABX <bitfile>.bit

Program a bitstream into ``userimage`` and an additional file into
``userdata``:

.. code-block:: bash

    openFPGALoader -b tinyFPGABX <bitfile>.bit --user-flash <bin>.<ext>

Bootloader update
-----------------

.. WARNING::
  This operation updates the bootloader. If flashing fails, the board may
  become unavailable through the TinyProg protocol and require recovery via
  direct SPI access.

Download official update files
==============================

.. code-block:: bash

    openFPGALoader -b tinyFPGABX --bootloader-update

This command downloads the required files from GitHub and writes them to SPI
flash.

Use explicit files
==================

.. code-block:: bash

    openFPGALoader -b tinyFPGABX --bootloader-update \
        <tinyfpga_bx_fw.bin> --user-flash <bootloader.bin>

The default file manifest used by TinyFPGA can be found
`here <https://tinyfpga.com/update/tinyfpga-bx/bootloader.json>`_.


Recovery after a failed bootloader update
-----------------------------------------

In some cases, a bootloader update may fail and leave the board in a broken
state. If that happens, the TinyProg protocol is no longer available and the
bootloader must be flashed again through direct SPI access.

The figure below shows the required pin mapping for an FT2232.

.. image:: https://github.com/user-attachments/assets/d3e04eb8-1c3f-4a63-97c6-f28cd07709c4
  :alt: TinyFPGA BX recovery pinout with FT2232

After wiring the FTDI adapter, flash the bootloader with:

.. code-block:: bash

    openFPGALoader -b ice40_generic tinyfpga_bx_fw.bin

``tinyfpga_bx_fw.bin`` can be downloaded
`here <https://github.com/tinyfpga/TinyFPGA-Bootloader/releases/download/1.0.1/tinyfpga_bx_fw.bin>`_.
