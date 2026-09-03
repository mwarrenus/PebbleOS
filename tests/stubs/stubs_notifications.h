/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/notifications/notifications.h"
#include "pbl/util/attributes.h"

void WEAK notifications_init(void) {}

void WEAK notifications_handle_notification_action_result(
    PebbleSysNotificationActionResult *action_result) {}

void WEAK notifications_handle_notification_added(Uuid *notification_id) {}

void WEAK notifications_handle_notification_acted_upon(Uuid *notification_id) {}

void WEAK notifications_handle_notification_removed(Uuid *notification_id) {}

void WEAK notifications_handle_ancs_notification_removed(uint32_t ancs_uid) {}

void WEAK notifications_migrate_timezone(const int new_tz_offset) {}

void WEAK notifications_add_notification(TimelineItem *notification) {}
bool WEAK notifications_are_multi_phone_indicators_enabled(void) { return false; }
