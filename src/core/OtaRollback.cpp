// SPDX-FileCopyrightText: 2025-2026 Volodymyr Papush (21CNCStudio)
// SPDX-License-Identifier: GPL-3.0-or-later
// GPL-3.0-or-later: https://www.gnu.org/licenses/gpl-3.0.html
// Want to use this code in a commercial product while keeping modifications proprietary?
// Purchase a Commercial License: see COMMERCIAL_LICENSE_SUMMARY.md

#include "core/OtaRollback.h"

#include <sdkconfig.h>

#ifndef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "OTA rollback requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE in the bootloader/SDK build"
#endif

#include <esp_err.h>
#include <esp_ota_ops.h>

#include "core/Logger.h"

// Strong override for Arduino's weak hook. Keep this module referenced from
// application code so the object stays linked and initArduino() does not
// auto-confirm rollback images before setup().
extern "C" bool verifyRollbackLater() {
    return true;
}

namespace {

const char *state_name(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW:
            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY:
            return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID:
            return "VALID";
        case ESP_OTA_IMG_INVALID:
            return "INVALID";
        case ESP_OTA_IMG_ABORTED:
            return "ABORTED";
        case ESP_OTA_IMG_UNDEFINED:
            return "UNDEFINED";
        default:
            return "UNKNOWN";
    }
}

const char *partition_label(const esp_partition_t *partition) {
    return partition ? partition->label : "none";
}

bool read_partition_state(const esp_partition_t *partition,
                          esp_ota_img_states_t &state,
                          esp_err_t &err) {
    if (!partition) {
        err = ESP_ERR_INVALID_ARG;
        return false;
    }
    err = esp_ota_get_state_partition(partition, &state);
    return err == ESP_OK;
}

void log_partition(const char *prefix, const esp_partition_t *partition, bool warn_on_state) {
    if (!partition) {
        LOGI("OTA", "%s partition: none", prefix);
        return;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t state_err = ESP_OK;
    if (read_partition_state(partition, state, state_err)) {
        LOGI("OTA",
             "%s partition: label=%s subtype=0x%02x offset=0x%lx size=%lu state=%s",
             prefix,
             partition->label,
             static_cast<unsigned>(partition->subtype),
             static_cast<unsigned long>(partition->address),
             static_cast<unsigned long>(partition->size),
             state_name(state));
        return;
    }

    const Logger::Level level = warn_on_state ? Logger::Warn : Logger::Info;
    Logger::log(level,
                "OTA",
                "%s partition: label=%s subtype=0x%02x offset=0x%lx size=%lu state=unavailable (%s)",
                prefix,
                partition->label,
                static_cast<unsigned>(partition->subtype),
                static_cast<unsigned long>(partition->address),
                static_cast<unsigned long>(partition->size),
                esp_err_to_name(state_err));
}

} // namespace

namespace OtaRollback {

void logCurrentAppState() {
    LOGI("OTA", "rollback validation deferred to application stable gate");
    log_partition("running", esp_ota_get_running_partition(), true);
    log_partition("boot", esp_ota_get_boot_partition(), false);

    const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();
    if (last_invalid) {
        log_partition("last invalid", last_invalid, true);
    }
}

bool isPendingVerify() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = ESP_OK;
    return read_partition_state(running, state, err) &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool markValidIfPending(const char *reason) {
    const char *reason_text = reason ? reason : "unspecified";
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t state_err = ESP_OK;
    if (!read_partition_state(running, state, state_err)) {
        LOGW("OTA",
             "rollback state unavailable for running partition %s (%s), reason=%s",
             partition_label(running),
             esp_err_to_name(state_err),
             reason_text);
        return false;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        LOGD("OTA",
             "rollback validation not pending for %s (state=%s, reason=%s)",
             partition_label(running),
             state_name(state),
             reason_text);
        return false;
    }

    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        LOGE("OTA",
             "failed to mark app valid for %s (reason=%s): %s",
             partition_label(running),
             reason_text,
             esp_err_to_name(err));
        return false;
    }

    LOGI("OTA",
         "marked app valid for rollback (partition=%s, reason=%s)",
         partition_label(running),
         reason_text);
    return true;
}

} // namespace OtaRollback
