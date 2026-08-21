/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/comm_session/session_remote_version.h"
#include "pbl/util/attributes.h"

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
