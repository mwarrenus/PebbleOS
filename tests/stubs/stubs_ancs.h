/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "comm/ble/kernel_le_client/ancs/ancs.h"

void ancs_perform_action(uint32_t notification_uid, uint8_t action_id) { }

BTBondingID WEAK ancs_get_bonding_id(void) {
  return BT_BONDING_ID_INVALID;
}

bool WEAK ancs_is_connected_to_device(const BTDeviceInternal *device) {
  return false;
}
