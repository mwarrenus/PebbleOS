/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/comm_session/session_remote_version.h"
#include "pbl/util/attributes.h"
#include <bluetooth/bluetooth_types.h>
#include <bluetooth/id.h>

void WEAK bt_persistent_storage_get_cached_system_capabilities(
    PebbleProtocolCapabilities *capabilities_out) {
  if (capabilities_out) {
    capabilities_out->flags = 0;
  }
}

uint8_t WEAK bt_persistent_storage_get_max_phones(void) {
  return 1;
}

void WEAK bt_persistent_storage_set_max_phones(uint8_t max_phones) {
}

int WEAK bt_persistent_storage_get_ble_pairing_index_by_id(BTBondingID bonding_id) {
  return (bonding_id != BT_BONDING_ID_INVALID) ? 0 : -1;
}

const char * WEAK bt_persistent_storage_get_connection_marker_prefix(BTBondingID bonding_id) {
  return "";
}

BTBondingID WEAK bt_persistent_storage_get_ble_ancs_bonding(void) {
  return 1;
}
