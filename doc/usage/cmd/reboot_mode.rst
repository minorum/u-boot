.. SPDX-License-Identifier: GPL-2.0+

reboot_mode command
===================

Synopsis
--------

::

    reboot_mode <device> <mode>

Description
-----------

Write a named mode using a reboot-mode device. Names and values come from its
device-tree mode properties. The backend must support writes. The command does
not reset the system.

Return value
------------

Zero indicates a successful write. An unknown device or mode, an unsupported
write operation, or a backend error returns failure.

Configuration
-------------

The command requires CONFIG_CMD_REBOOT_MODE.
