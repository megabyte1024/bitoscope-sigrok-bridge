/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bitoscope_driver.h"

#include "bridge_driver.h"
#include "usb_id_table.h"

int main(int argc, char** argv) {
  sigrok_bridge::Bridge bridge(sigrok_bridge::driverTable(), sigrok_bridge::demoModeOfEnvironment());
  const bitoscope_driver driver = bridge.description();
  return bitoscope_run_driver(argc, argv, &driver);
}
