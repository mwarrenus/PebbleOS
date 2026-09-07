/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "bluetooth.h"
#include "menu.h"
#include "remote.h"
#include "window.h"

#include "applib/app.h"
#include "applib/app_focus_service.h"
#include "applib/event_service_client.h"
#include "applib/fonts/fonts.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/gtypes.h"
#include "applib/ui/ui.h"
#include "comm/bt_lock.h"
#include "comm/ble/gap_le_connect.h"
#include "comm/ble/gap_le_connection.h"
#include "comm/ble/gap_le_device_name.h"
#include "comm/ble/gap_le_slave_reconnect.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/system_icons.h"
#include "resource/resource_ids.auto.h"
#include "pbl/services/bluetooth/bluetooth_persistent_storage.h"
#include "pbl/services/bluetooth/local_id.h"
#include "pbl/services/bluetooth/pairability.h"
#include "pbl/services/i18n/i18n.h"
#include "pbl/services/bluetooth/ble_hrm.h"
#include "pbl/services/clock.h"
#include "pbl/services/notifications/notifications.h"
#include "pbl/services/timeline/item.h"
#include "shell/system_theme.h"
#include <pbl/logging/logging.h>
#include "system/passert.h"
#include "pbl/util/string.h"

#include <bluetooth/bluetooth_types.h>
#include <bluetooth/sm_types.h>
#include <pbl/btutil/bt_device.h>

#include <stdio.h>
#include <string.h>

#define HEADER_BUFFER_SIZE 22

#define SHARING_HEART_RATE_EXTRA_HEIGHT_PX (18)

typedef enum SettingsBluetooth {
  SettingsBluetoothAirplaneMode,
  SettingsBluetoothTotal,
} SettingsBluetooth;

enum {
  BluetoothIconIdx,
  BluetoothAltIconIdx,
  AirplaneIconIdx,
  NumIcons,
};

static const uint32_t ICON_RESOURCE_ID[NumIcons] = {
  RESOURCE_ID_SETTINGS_ICON_BLUETOOTH,
  RESOURCE_ID_SETTINGS_ICON_BLUETOOTH_ALT,
  RESOURCE_ID_SETTINGS_ICON_AIRPLANE,
};

typedef enum {
  ToggleStateIdle,
  ToggleStateEnablingBluetooth,
  ToggleStateDisablingBluetooth,
} ToggleState;

typedef struct SettingsBluetoothData {
  SettingsCallbacks callbacks;

  GBitmap icon_heap_bitmap[NumIcons];

  ListNode* remote_list_head;

  char header_buffer[HEADER_BUFFER_SIZE];
  ToggleState toggle_state;
  bool did_enable_pairability;

  EventServiceInfo bt_airplane_event_info;
  EventServiceInfo bt_connection_event_info;
  EventServiceInfo bt_pairing_event_info;
  EventServiceInfo ble_device_name_updated_event_info;
#ifdef CONFIG_HRM
  EventServiceInfo ble_hrm_sharing_event_info;
#endif
} SettingsBluetoothData;

// BT stack interaction stuff
///////////////////////////

static void settings_bluetooth_toggle_airplane_mode(SettingsBluetoothData* data) {
  const bool airplane_mode = bt_ctl_is_airplane_mode_on();
  bt_ctl_set_airplane_mode_async(!airplane_mode);
  data->toggle_state =
      airplane_mode ? ToggleStateEnablingBluetooth : ToggleStateDisablingBluetooth;
  settings_menu_mark_dirty(SettingsMenuItemBluetooth);
}

bool is_remote_connected(StoredRemote* remote) {
  return (remote->ble.connection != NULL);
}

static int remote_comparator(StoredRemote* remote, StoredRemote* other) {
  if (is_remote_connected(remote) != is_remote_connected(other)) {
    return is_remote_connected(remote) ? -1 : 1;
  }
  if (bt_persistent_storage_get_max_phones() > 1) {
    int idx_remote = bt_persistent_storage_get_ble_pairing_index_by_id(remote->ble.bonding);
    int idx_other = bt_persistent_storage_get_ble_pairing_index_by_id(other->ble.bonding);
    if (idx_remote >= 0 && idx_other >= 0 && idx_remote != idx_other) {
      return (idx_remote < idx_other) ? -1 : 1;
    }
  }
  return strncmp(remote->name, other->name, sizeof(remote->name));
}

static void add_remote(SettingsBluetoothData* data, StoredRemote* remote) {
  const bool ascending = false;
  data->remote_list_head = list_sorted_add(data->remote_list_head, &remote->list_node,
      (Comparator) remote_comparator, ascending);
}

static StoredRemote* stored_remote_create(void) {
  StoredRemote* remote = task_malloc_check(sizeof(*remote));
  *remote = (StoredRemote){};
  return remote;
}

static void prv_copy_device_name_with_fallback(StoredRemote *remote, const char *name) {
  if (!name || strlen(name) == 0) {
    i18n_get_with_buffer("<Untitled>", remote->name, sizeof(remote->name));
  } else {
    strncpy(remote->name, name, sizeof(remote->name));
  }
}

static void prv_add_ble_remote(BTDeviceInternal *device, SMIdentityResolvingKey *irk,
                               const char *name, BTBondingID *id, void *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData*) context;
  if (!data) {
    return;
  }

  StoredRemote* remote = stored_remote_create();
  remote->ble.bonding = *id;
  prv_copy_device_name_with_fallback(remote, name);

  bt_lock();
  GAPLEConnection *connection = gap_le_connection_find_by_irk(irk);
  if (!connection) {
    connection = gap_le_connection_by_device(device);
  }
  remote->ble.connection = connection;
  if (connection && connection->device_name && connection->device_name[0] != '\0') {
    prv_copy_device_name_with_fallback(remote, connection->device_name);
  }
#ifdef CONFIG_HRM
  remote->ble.is_sharing_heart_rate = ble_hrm_is_sharing_to_connection(connection);
#endif
  bt_unlock();

  add_remote(data, remote);
}

static void prv_add_ble_remotes(SettingsBluetoothData *data) {
  bt_persistent_storage_for_each_ble_pairing(prv_add_ble_remote, data);
}

static void prv_clear_remote_list(SettingsBluetoothData* data) {
  while (data->remote_list_head) {
    StoredRemote* remote = (StoredRemote*) data->remote_list_head;
    data->remote_list_head = list_pop_head(&remote->list_node);
    task_free(remote);
  }
}

static void prv_reload_remote_list(SettingsBluetoothData* data) {
  prv_clear_remote_list(data);
  prv_add_ble_remotes(data);
}

static void settings_bluetooth_update_remotes_private(SettingsBluetoothData* data) {
  prv_reload_remote_list(data);

  if (!data->remote_list_head) {
    strncpy(data->header_buffer, i18n_get("Pairing Instructions", data), HEADER_BUFFER_SIZE);
  } else {
    const unsigned int num_remotes = list_count(data->remote_list_head);
    sniprintf(data->header_buffer, HEADER_BUFFER_SIZE,
              (num_remotes != 1) ? i18n_get("%u Paired Phones", data) :
                                   i18n_get("%u Paired Phone", data),
              num_remotes);
  }
}

void settings_bluetooth_update_remotes(SettingsBluetoothData *data) {
  settings_bluetooth_update_remotes_private(data);
  settings_menu_reload_data(SettingsMenuItemBluetooth);
}

//////////

static SettingsBluetoothData *s_bluetooth_data = NULL;

static void prv_update_pairability(SettingsBluetoothData *data) {
  if (!data) {
    return;
  }
  const unsigned int num_remotes = data->remote_list_head ? list_count(data->remote_list_head) : 0;
  const uint8_t max_phones = bt_persistent_storage_get_max_phones();
  if (num_remotes < max_phones) {
    if (!data->did_enable_pairability) {
      bt_pairability_use();
      data->did_enable_pairability = true;
      PBL_LOG_INFO("Enabled advertising - fewer than max_phones paired");
    }
  } else {
    if (data->did_enable_pairability) {
      bt_pairability_release();
      data->did_enable_pairability = false;
      PBL_LOG_INFO("Disabled advertising - max_phones paired");
    }
  }
}

static void prv_settings_bluetooth_event_handler(PebbleEvent *event, void *context) {
  SettingsBluetoothData* settings_data = (SettingsBluetoothData *) context;
  PBL_LOG_DBG("BT EVENT");
  switch (event->type) {
    case PEBBLE_BT_CONNECTION_EVENT:
      // If BT Settings is open, update BLE device name upon connecting device:
      if (event->bluetooth.connection.is_ble &&
          event->bluetooth.connection.state == PebbleBluetoothConnectionEventStateConnected) {
        // https://pebbletechnology.atlassian.net/browse/PBL-22176
        // iOS seems to respond with 0x0E (Unlikely Error) when performing this request while
        // the encryption set up is going on. For non-bonded devices it will work fine though.
        gap_le_device_name_request(&event->bluetooth.connection.device);
      }
      // fall-through!
    case PEBBLE_BT_PAIRING_EVENT:
#ifdef CONFIG_HRM
    case PEBBLE_BLE_HRM_SHARING_STATE_UPDATED_EVENT:
#endif
    case PEBBLE_BLE_DEVICE_NAME_UPDATED_EVENT: {
      settings_bluetooth_update_remotes(settings_data);
      prv_update_pairability(settings_data);
      break;
    }

    case PEBBLE_BT_STATE_EVENT: {
      settings_data->toggle_state = ToggleStateIdle;
      settings_menu_mark_dirty(SettingsMenuItemBluetooth);
      break;
    }

    default:
      break;
  }
}

// UI Stuff
/////////////////////////////
// Menu Layer Callbacks
/////////////////////////////
//-- Address
//   ...
//|  Airplane Mode: Off
//-- Paired Devices
//|  Device Name
//   Connected
//|  Device Name
//

#if PBL_RECT
static void prv_draw_stored_remote_item_rect(GContext *ctx, const Layer *cell_layer,
                                             const char *remote_name, const char *connected_string,
                                             const char *le_string,
                                             const char *is_sharing_heart_rate_string,
                                             int phone_idx) {
  const GFont font = ((le_string || is_sharing_heart_rate_string) ?
                      fonts_get_system_font(FONT_KEY_GOTHIC_18) : NULL);

  if (le_string) {
    GRect box = cell_layer->bounds;
    box.size.w -= 5;
    box.origin.y += 20;
    box.size.h = 24;

    graphics_draw_text(ctx, le_string, font, box, GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }

  if (is_sharing_heart_rate_string) {
    const int horizontal_margin = menu_cell_basic_horizontal_inset();
    GRect box = grect_inset(cell_layer->bounds, GEdgeInsets(0, horizontal_margin));
    box.origin.y += 38;
    box.size.h = 24;

    graphics_draw_text(ctx, is_sharing_heart_rate_string, font, box,
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    // Gross hack to avoid centering the title / subtitle labels in the entire cell:
    ((Layer *)cell_layer)->bounds.size.h -= SHARING_HEART_RATE_EXTRA_HEIGHT_PX;
  }

  const bool has_indicator = (phone_idx >= 0 && connected_string && connected_string[0] != '\0');
  menu_cell_basic_draw(ctx, cell_layer, remote_name, has_indicator ? "" : connected_string, NULL);

  if (has_indicator) {
    const GFont title_font = system_theme_get_font_for_default_size(TextStyleFont_MenuCellTitle);
    const int16_t title_height = fonts_get_font_height(title_font);
    const GFont subtitle_font =
        system_theme_get_font_for_default_size(TextStyleFont_MenuCellSubtitle);
    const int16_t subtitle_height = fonts_get_font_height(subtitle_font);
    const int16_t full_height = title_height + subtitle_height + 10;
    const int horizontal_margin = menu_cell_basic_horizontal_inset();
    const int vertical_margin = (cell_layer->bounds.size.h - full_height) / 2;

    GRect sub_box = cell_layer->bounds;
    sub_box.origin.x += horizontal_margin;
    sub_box.origin.y += vertical_margin + title_height;
    sub_box.size.w -= horizontal_margin;
    sub_box.size.h = subtitle_height + 4;

    const bool is_highlighted = menu_cell_layer_is_highlighted(cell_layer);
    const GColor text_color = is_highlighted ? GColorWhite : GColorBlack;
    GColor indicator_color = is_highlighted ? GColorWhite : GColorBlack;
#if PBL_COLOR
    if (!is_highlighted) {
      indicator_color = GColorDarkGray;
    }
#endif
    graphics_context_set_text_color(ctx, text_color);
    graphics_context_set_fill_color(ctx, indicator_color);

    const int16_t pip_x = sub_box.origin.x;
    const int16_t pip_cy = sub_box.origin.y + (subtitle_height / 2);

    if (phone_idx == 0) {
      // Phone 1: single vertical pip / dot
      const GRect pip = GRect(pip_x, pip_cy - 1, 2, 3);
      graphics_fill_rect(ctx, &pip);
    } else if (phone_idx == 1) {
      // Phone 2: two vertical pips / dots
      const GRect pip1 = GRect(pip_x, pip_cy - 4, 2, 3);
      const GRect pip2 = GRect(pip_x, pip_cy + 1, 2, 3);
      graphics_fill_rect(ctx, &pip1);
      graphics_fill_rect(ctx, &pip2);
    }

    sub_box.origin.x += 7;
    sub_box.size.w -= 7;
    graphics_draw_text(ctx, connected_string, subtitle_font, sub_box,
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  }

  if (is_sharing_heart_rate_string) {
    // Restore original height:
    ((Layer *)cell_layer)->bounds.size.h += SHARING_HEART_RATE_EXTRA_HEIGHT_PX;
  }
}
#endif

bool settings_bluetooth_is_sharing_heart_rate_for_stored_remote(StoredRemote* remote) {
#ifdef CONFIG_HRM
  return remote->ble.is_sharing_heart_rate;
#else
  return false;
#endif  // CONFIG_HRM
}

#if PBL_ROUND
static void prv_draw_stored_remote_item_round(GContext *ctx, const Layer *cell_layer,
                                              const char *remote_name, const char *connected_string,
                                              const char *le_string,
                                              const char *is_sharing_heart_rate_string,
                                              int phone_idx) {
#  ifdef CONFIG_HRM
  _Static_assert(false, "FIXME: Implement round drawing code to show heart rate sharing status!");
#  endif  // CONFIG_HRM
  menu_cell_basic_draw(ctx, cell_layer, remote_name, connected_string, NULL);
}
#endif  // PBL_ROUND

static void draw_stored_remote_item(GContext *ctx, const Layer *cell_layer,
                                    uint16_t device_index, SettingsBluetoothData *data) {
  const uint32_t num_remotes = list_count(data->remote_list_head);
  PBL_ASSERT(device_index < num_remotes, "Got index %" PRId16 " only have %" PRId32,
             device_index, num_remotes);
  StoredRemote* remote = (StoredRemote*) list_get_at(data->remote_list_head, device_index);
  bool connected = is_remote_connected(remote);

  const char *le_string = NULL;

  const char *connected_string = connected ? i18n_get("Connected", data) :
                                 PBL_IF_RECT_ELSE("", NULL);

  // Add ellipsis if the name might have been cut off by the mobile
  const char ellipsis[] = UTF8_ELLIPSIS_STRING;
  const size_t max_name_size = BT_DEVICE_NAME_BUFFER_SIZE - 2;
  const size_t name_size = strnlen(remote->name, BT_DEVICE_NAME_BUFFER_SIZE);
  char *remote_name = task_zalloc_check(max_name_size + sizeof(ellipsis));
  strncpy(remote_name, remote->name, max_name_size);
  if (name_size > max_name_size) {
    const size_t ellipsis_start_offset = utf8_get_size_truncate(remote_name, max_name_size);
    strncpy(&remote_name[ellipsis_start_offset], ellipsis, sizeof(ellipsis));
  }

  const char *is_sharing_heart_rate =
      (settings_bluetooth_is_sharing_heart_rate_for_stored_remote(remote) ?
       i18n_get("Sharing Heart Rate ❤", data) : NULL);

  int phone_idx = -1;
  if (connected && (bt_persistent_storage_get_max_phones() > 1)) {
    phone_idx = bt_persistent_storage_get_ble_pairing_index_by_id(remote->ble.bonding);
    if (phone_idx < 0) {
      phone_idx = (int)device_index;
    }
  }

  PBL_IF_RECT_ELSE(prv_draw_stored_remote_item_rect,
                   prv_draw_stored_remote_item_round)(ctx, cell_layer, remote_name,
                                                      connected_string, le_string,
                                                      is_sharing_heart_rate, phone_idx);

  task_free(remote_name);
}


static void prv_send_test_notification(uint8_t phone_idx, const char *title, const char *body) {
  AttributeList attr_list = {};
  attribute_list_add_cstring(&attr_list, AttributeIdTitle, title);
  attribute_list_add_cstring(&attr_list, AttributeIdBody, body);

  AttributeList dismiss_attr = {};
  attribute_list_add_cstring(&dismiss_attr, AttributeIdTitle, "Dismiss");
  TimelineItemActionGroup action_group = {
    .num_actions = 1,
    .actions = (TimelineItemAction[]){
      { .id = 0, .type = TimelineItemActionTypeDismiss, .attr_list = dismiss_attr },
    },
  };

  TimelineItem *item = timeline_item_create_with_attributes(
      rtc_get_time(), 0, TimelineItemTypeNotification, LayoutIdNotification, &attr_list,
      &action_group);
  attribute_list_destroy_list(&attr_list);
  attribute_list_destroy_list(&dismiss_attr);
  if (item) {
    item->header.phone_idx = phone_idx;
    item->header.ancs_notif = (phone_idx == 1);
    notifications_add_notification(item);
    timeline_item_destroy(item);
  }
}

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  const uint8_t max_phones = bt_persistent_storage_get_max_phones();
  return list_count(data->remote_list_head) + (max_phones > 1 ? 5 : 3);
}

static int16_t prv_row_height_cb(SettingsCallbacks *context, uint16_t row, bool is_selected) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  const unsigned int num_remotes = list_count(data->remote_list_head);
  const uint8_t max_phones = bt_persistent_storage_get_max_phones();
  const uint16_t instruction_row = num_remotes + (max_phones > 1 ? 4 : 2);
  if (row == instruction_row) {
    return 60;
  }
#if PBL_RECT
#  ifdef CONFIG_HRM
  int heart_rate_sharing_text_height = 0;
  if (row > 0 && row <= num_remotes) {
    const uint16_t device_index = row - 1;
    StoredRemote* remote = (StoredRemote*) list_get_at(data->remote_list_head, device_index);
    if (settings_bluetooth_is_sharing_heart_rate_for_stored_remote(remote)) {
      heart_rate_sharing_text_height = SHARING_HEART_RATE_EXTRA_HEIGHT_PX;
    }
  }
#  else
  const int heart_rate_sharing_text_height = 0;
#  endif  // CONFIG_HRM
  return menu_cell_basic_cell_height() + heart_rate_sharing_text_height;
#elif PBL_ROUND
  return (is_selected ? MENU_CELL_ROUND_FOCUSED_TALL_CELL_HEIGHT :
          MENU_CELL_ROUND_UNFOCUSED_SHORT_CELL_HEIGHT);
#else
#endif
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx,
                            const Layer *cell_layer, uint16_t row, bool selected) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  const unsigned int num_remotes = list_count(data->remote_list_head);

  if (row == 0) {
    char device_name_buffer[BT_DEVICE_NAME_BUFFER_SIZE];
    const char *subtitle = NULL;
    const char *title = i18n_get("Connection", data);
    GBitmap *icon = NULL;
    if (data->toggle_state == ToggleStateIdle) {
      if (bt_ctl_is_airplane_mode_on()) {
        subtitle = i18n_get("Airplane Mode", data);
        icon = &data->icon_heap_bitmap[AirplaneIconIdx];
      } else {
        // Always show device name as subtitle
        bt_local_id_copy_device_name(device_name_buffer, false);
        subtitle = device_name_buffer;
        icon = &data->icon_heap_bitmap[BluetoothIconIdx];
      }
    } else {
      subtitle = (data->toggle_state == ToggleStateDisablingBluetooth)
          ? i18n_get("Disabling...", data) : i18n_get("Enabling...", data);
      icon = &data->icon_heap_bitmap[BluetoothAltIconIdx];
    }

    menu_cell_basic_draw(ctx, cell_layer, title, subtitle, icon);
  } else if (row <= num_remotes) {
    const uint16_t device_index = row - 1;
    draw_stored_remote_item(ctx, cell_layer, device_index, data);
  } else if (row == num_remotes + 1) {
    // Phones Allowed setting row
    const uint8_t max_phones = bt_persistent_storage_get_max_phones();
    const char *title = i18n_get("Phones Allowed", data);
    const char *subtitle = (max_phones == 1) ? i18n_get("1 Phone", data) : i18n_get("2 Phones", data);
    menu_cell_basic_draw(ctx, cell_layer, title, subtitle, NULL);
  } else if (row == num_remotes + 2 && bt_persistent_storage_get_max_phones() > 1) {
    menu_cell_basic_draw(ctx, cell_layer, i18n_get("Test Phone 1 Alert", data),
                         i18n_get("Send test notification", data), NULL);
  } else if (row == num_remotes + 3 && bt_persistent_storage_get_max_phones() > 1) {
    menu_cell_basic_draw(ctx, cell_layer, i18n_get("Test Phone 2 Alert", data),
                         i18n_get("Send test notification", data), NULL);
  } else {
    // Instruction text row
    const uint8_t max_phones = bt_persistent_storage_get_max_phones();
    graphics_context_set_text_color(ctx, selected ? GColorWhite : GColorBlack);
    GFont font = system_theme_get_font_for_default_size(TextStyleFont_MenuCellSubtitle);
    const int16_t horizontal_inset = menu_cell_basic_horizontal_inset() * 2;
    GRect box = cell_layer->bounds;
    box.origin.x += horizontal_inset;
    box.origin.y += 6;
    box.size.w -= horizontal_inset * 2;
    box.size.h -= 6;

    const char *msg = NULL;
    if (num_remotes == 0) {
      if (bt_ctl_is_airplane_mode_on()) {
        msg = i18n_get("Disable Airplane Mode to connect.", data);
      } else {
        msg = i18n_get("Open the Pebble app on your phone to connect.", data);
      }
    } else {
      if (num_remotes < max_phones) {
        msg = i18n_get("Open the Pebble app on another phone to connect.", data);
      } else {
        msg = i18n_get(num_remotes == 1 ? "Forget this device to pair a new device." :
                                          "Forget a device to pair a new device.", data);
      }
    }
    graphics_draw_text(ctx, msg, font, box, GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
  }
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  if (row == 0) {
    settings_bluetooth_toggle_airplane_mode(data);
    return;
  }
  const unsigned int num_remotes = list_count(data->remote_list_head);
  if (row <= num_remotes) {
    prv_reload_remote_list(data);
    StoredRemote* remote = (StoredRemote*) list_get_at(data->remote_list_head, row - 1);
    if (remote) {
      settings_remote_menu_push(data, remote);
    }
    return;
  }
  if (row == num_remotes + 1) {
    const uint8_t cur_max = bt_persistent_storage_get_max_phones();
    const uint8_t new_max = (cur_max == 1) ? 2 : 1;
    bt_persistent_storage_set_max_phones(new_max);

    gap_le_connect_enforce_max_slave_connections();
    gap_le_slave_reconnect_start();
    settings_bluetooth_update_remotes(data);
    prv_update_pairability(data);
    return;
  }
  if (row == num_remotes + 2 && bt_persistent_storage_get_max_phones() > 1) {
    prv_send_test_notification(0, "Android (Phone 1)", "Message received from Android device.");
    return;
  }
  if (row == num_remotes + 3 && bt_persistent_storage_get_max_phones() > 1) {
    prv_send_test_notification(1, "iPhone (Phone 2)", "Message received from iPhone device.");
    return;
  }
}

static void prv_focus_handler(bool in_focus) {
  if (!in_focus) {
    return;
  }
  if (s_bluetooth_data) {
    settings_bluetooth_update_remotes(s_bluetooth_data);
    prv_update_pairability(s_bluetooth_data);
  } else {
    settings_menu_reload_data(SettingsMenuItemBluetooth);
  }
}

static void prv_expand_cb(SettingsCallbacks *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  s_bluetooth_data = data;

  settings_bluetooth_update_remotes(data);

  // When entering the BT Settings, update device names of all connected devices:
  if (!bt_ctl_is_airplane_mode_on()) {
    gap_le_device_name_request_all();
  }

  data->bt_airplane_event_info = (EventServiceInfo) {
    .type = PEBBLE_BT_STATE_EVENT,
    .handler = prv_settings_bluetooth_event_handler,
    .context = data,
  };
  data->bt_connection_event_info = (EventServiceInfo) {
    .type = PEBBLE_BT_CONNECTION_EVENT,
    .handler = prv_settings_bluetooth_event_handler,
    .context = data,
  };
  data->bt_pairing_event_info = (EventServiceInfo) {
    .type = PEBBLE_BT_PAIRING_EVENT,
    .handler = prv_settings_bluetooth_event_handler,
    .context = data,
  };
  data->ble_device_name_updated_event_info = (EventServiceInfo) {
    .type = PEBBLE_BLE_DEVICE_NAME_UPDATED_EVENT,
    .handler = prv_settings_bluetooth_event_handler,
    .context = data,
  };
#ifdef CONFIG_HRM
  data->ble_hrm_sharing_event_info = (EventServiceInfo) {
    .type = PEBBLE_BLE_HRM_SHARING_STATE_UPDATED_EVENT,
    .handler = prv_settings_bluetooth_event_handler,
    .context = data,
  };
  event_service_client_subscribe(&data->ble_hrm_sharing_event_info);
#endif
  event_service_client_subscribe(&data->bt_airplane_event_info);
  event_service_client_subscribe(&data->bt_connection_event_info);
  event_service_client_subscribe(&data->bt_pairing_event_info);
  event_service_client_subscribe(&data->ble_device_name_updated_event_info);
  
  data->did_enable_pairability = false;
  prv_update_pairability(data);
  
  // Reload & redraw after pairing popup
  app_focus_service_subscribe_handlers((AppFocusHandlers) { .did_focus = prv_focus_handler });
}

// Turns off services that are part of the bluetooth settings menu such as enabling
// discovery. We don't want to keep these services running longer than necessary because
// they consume a fair amount of power
static void prv_hide_cb(SettingsCallbacks *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  
  // Only release pairability if we enabled it
  if (data->did_enable_pairability) {
    bt_pairability_release();
    data->did_enable_pairability = false;
  }
  s_bluetooth_data = NULL;
  
#ifdef CONFIG_HRM
  event_service_client_unsubscribe(&data->ble_hrm_sharing_event_info);
#endif
  event_service_client_unsubscribe(&data->bt_airplane_event_info);
  event_service_client_unsubscribe(&data->bt_connection_event_info);
  event_service_client_unsubscribe(&data->bt_pairing_event_info);
  event_service_client_unsubscribe(&data->ble_device_name_updated_event_info);
  app_focus_service_unsubscribe();
}

static void prv_deinit_cb(SettingsCallbacks *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  s_bluetooth_data = NULL;

  i18n_free_all(data);

  prv_clear_remote_list(data);
  for (unsigned int idx = 0; idx < NumIcons; ++idx) {
    gbitmap_deinit(&data->icon_heap_bitmap[idx]);
  }
  app_free(data);
}

static Window *prv_init(void) {
  SettingsBluetoothData *data = app_malloc_check(sizeof(SettingsBluetoothData));
  *data = (SettingsBluetoothData){};

  for (unsigned int idx = 0; idx < NumIcons; ++idx) {
    gbitmap_init_with_resource(&data->icon_heap_bitmap[idx], ICON_RESOURCE_ID[idx]);
  }

  data->callbacks = (SettingsCallbacks) {
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
    .row_height = prv_row_height_cb,
    .expand = prv_expand_cb,
    .hide = prv_hide_cb,
  };

  return settings_window_create(SettingsMenuItemBluetooth, &data->callbacks);
}

const SettingsModuleMetadata *settings_bluetooth_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    .name = i18n_noop("Bluetooth"),
    .init = prv_init,
  };

  return &s_module_info;
}

#undef HEADER_BUFFER_SIZE
