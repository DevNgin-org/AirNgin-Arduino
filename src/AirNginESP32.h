#pragma once

#include "airngin.h"

/**
 * Arduino-friendly facade over the AirNgin 1.6.0 C API.
 *
 * The low-level C API in airngin.h remains public and fully usable. This class
 * only shortens common setup/teardown code for Arduino sketches.
 */
class AirNginESP32Class {
public:
    esp_err_t beginManaged(const char *serial,
                           const char *producerCode,
                           airngin_command_handler_t commandHandler = nullptr,
                           airngin_event_handler_t eventHandler = nullptr,
                           void *userContext = nullptr,
                           const char *provisioningLabel = "Device",
                           const char *provisioningPassword = CONFIG_AIRNGIN_PROVISIONING_PASSWORD);

    esp_err_t beginExternal(const char *serial,
                            const char *producerCode,
                            const airngin_cloud_credentials_t &cloud,
                            airngin_command_handler_t commandHandler = nullptr,
                            airngin_event_handler_t eventHandler = nullptr,
                            void *userContext = nullptr,
                            uint32_t reconnectDelayMs = CONFIG_AIRNGIN_EXTERNAL_RECONNECT_DELAY_MS);

    esp_err_t stop();
    esp_err_t end();

    airngin_state_t state() const { return airngin_get_state(); }
    bool ready() const { return airngin_is_ready(); }
    esp_err_t status(airngin_status_t &out) const {
        out.struct_size = sizeof(out);
        return airngin_get_status(&out);
    }

    esp_err_t publish(const char *key, const char *value) const { return airngin_publish(key, value); }
    esp_err_t report(const char *key, const char *value) const { return airngin_report(key, value); }

    esp_err_t requestProvisioning() const { return airngin_request_provisioning(); }
    esp_err_t cancelProvisioning() const { return airngin_cancel_provisioning(); }

    esp_err_t setExternalNetworkConnected(bool connected) const {
        return airngin_external_network_set_connected(connected);
    }
    esp_err_t replaceExternalCredentials(const airngin_cloud_credentials_t &cloud) const {
        return airngin_external_set_cloud_credentials(&cloud);
    }

    esp_err_t subscribeTime(airngin_time_source_t source) const { return airngin_time_subscribe(source); }
    esp_err_t unsubscribeTime() const { return airngin_time_unsubscribe(); }
    esp_err_t latestTime(airngin_time_info_t &out) const { return airngin_time_get_latest(&out); }

    esp_err_t databaseSet(const char *key, const char *value, uint32_t *requestId = nullptr) const {
        return airngin_database_set(key, value, requestId);
    }
    esp_err_t databaseGet(const char *key, uint32_t *requestId = nullptr) const {
        return airngin_database_get(key, requestId);
    }
    esp_err_t databaseDelete(const char *key, uint32_t *requestId = nullptr) const {
        return airngin_database_delete(key, requestId);
    }

    esp_err_t notifyCenter(const char *code) const { return airngin_send_notification_to_center(code); }
    esp_err_t notifyUser(const char *code, airngin_notification_channel_t channel) const {
        return airngin_send_notification_to_user(code, channel);
    }
    esp_err_t smsCenter(const char *code) const { return airngin_send_sms_to_center(code); }
    esp_err_t smsUser(const char *code) const { return airngin_send_sms_to_user(code); }
    esp_err_t pushUser(const char *code) const { return airngin_send_push_to_user(code); }
};

extern AirNginESP32Class AirNgin;
