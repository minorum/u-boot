.. SPDX-License-Identifier: GPL-2.0-only

.. index::
   single: charger (command)

charger command
===============

Synopsis
--------

::

    charger status <device>

Description
-----------

Display the charging state, health, input presence, battery voltage in
microvolts, battery temperature in millidegrees Celsius and configured input
current limit in microamps for a named driver-model charger.
If a USB source is available, also display its detected type, physical
presence and current authorization separately from the accepted USB budget
and programmed charger limit.

The input current limit is not a measurement of battery or USB current.
The command does not estimate battery capacity or change charging limits.
Device lookup may probe the charger. Reading status may refresh the driver's
charging policy, including its control registers and protection watchdog.

Configuration
-------------

The command requires CONFIG_CMD_CHARGER and CONFIG_DM_CHARGER.

Return value
------------

The return value is zero when the status was obtained, including when the
charger reports a fault or is not charging. Lookup and status errors return
one and do not display a sample.
