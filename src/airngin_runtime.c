#include "airngin_runtime_ops.h"
#include "airngin_transport.h"
#include "AirNginConfig.h"
#include <string.h>

/*
 * Routes the runtime interface used by airngin.c to the runtime selected by airngin_init().
 * The mode is fixed between init and deinit, so start/stop/status always agree.
 */
#if CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI
#define AG_MANAGED(call, fallback) (airngin_internal_network_mode() == AIRNGIN_NETWORK_EXTERNAL ? (fallback) : (call))
#else
#define AG_MANAGED(call, fallback) (fallback)
#endif

esp_err_t airngin_runtime_start(void)
{
    return AG_MANAGED(airngin_managed_start(), airngin_external_start());
}

void airngin_runtime_release_owner(void)
{
    if (airngin_internal_network_mode() == AIRNGIN_NETWORK_EXTERNAL) {
        airngin_external_release_owner();
    } else {
#if CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI
        airngin_managed_release_owner();
#endif
    }
}

esp_err_t airngin_runtime_stop(void)
{
    return AG_MANAGED(airngin_managed_stop(), airngin_external_stop());
}

bool airngin_runtime_in_owner_task(void)
{
    return AG_MANAGED(airngin_managed_in_owner_task(), airngin_external_in_owner_task());
}

void airngin_runtime_get_status(airngin_runtime_status_t *out)
{
    if (airngin_internal_network_mode() == AIRNGIN_NETWORK_EXTERNAL) {
        airngin_external_get_status(out);
    } else {
#if CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI
        airngin_managed_get_status(out);
#else
        if (out) *out = (airngin_runtime_status_t){0};
#endif
    }
}

static bool same_cloud_identity(const airngin_profile *a, const airngin_profile *b)
{
    return strcmp(a->projectCode, b->projectCode) == 0 &&
           strcmp(a->mqttBroker, b->mqttBroker) == 0 &&
           strcmp(a->mqttUser, b->mqttUser) == 0 &&
           strcmp(a->mqttPass, b->mqttPass) == 0 &&
           strcmp(a->clientId, b->clientId) == 0
#if CONFIG_AIRNGIN_USE_AES
           && strcmp(a->aesKey, b->aesKey) == 0
#endif
           ;
}

esp_err_t airngin_runtime_get_effective_cloud_credentials(airngin_effective_cloud_credentials_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    airngin_profile effective = {0};
    esp_err_t err = airngin_transport_get_effective_cloud_profile(&effective);
    if (err != ESP_OK) return err;

    const airngin_network_mode_t mode = airngin_internal_network_mode();
    if (mode == AIRNGIN_NETWORK_EXTERNAL) {
        /* A replacement updates the runtime copy before the owner rebuilds MQTT. During that short window neither
         * the old nor new identity is "current" for a new HTTPS request, so refuse it instead of using stale bytes. */
        airngin_profile configured = {0};
        if (!airngin_internal_cloud_profile(&configured, NULL) || !same_cloud_identity(&effective, &configured)) {
            airngin_secure_zero(&configured, sizeof(configured));
            airngin_secure_zero(&effective, sizeof(effective));
            return ESP_ERR_INVALID_STATE;
        }
        airngin_secure_zero(&configured, sizeof(configured));
        out->source = AIRNGIN_EFFECTIVE_CREDENTIALS_EXTERNAL_RUNTIME;
    } else {
        out->source = AIRNGIN_EFFECTIVE_CREDENTIALS_MANAGED_NVS_PROFILE;
    }

    memcpy(out->project_code, effective.projectCode, sizeof(out->project_code));
    memcpy(out->username, effective.mqttUser, sizeof(out->username));
    memcpy(out->password, effective.mqttPass, sizeof(out->password));
    memcpy(out->client_id, effective.clientId, sizeof(out->client_id));
    memcpy(out->broker_host, effective.mqttBroker, sizeof(out->broker_host));
    const char *serial = airngin_internal_serial();
    if (!serial || strnlen(serial, sizeof(out->device_serial)) >= sizeof(out->device_serial)) {
        airngin_secure_zero(&effective, sizeof(effective));
        airngin_secure_zero(out, sizeof(*out));
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(out->device_serial, serial, strlen(serial) + 1U);
    airngin_secure_zero(&effective, sizeof(effective));
    return ESP_OK;
}

esp_err_t airngin_runtime_publish(const char *key, size_t key_len, const char *value, size_t value_len)
{
    return AG_MANAGED(airngin_managed_publish(key, key_len, value, value_len),
                      airngin_external_publish(key, key_len, value, value_len));
}

esp_err_t airngin_runtime_notify(bool to_user, int channel, const char *code, size_t code_len)
{
    return AG_MANAGED(airngin_managed_notify(to_user, channel, code, code_len),
                      airngin_external_notify(to_user, channel, code, code_len));
}

esp_err_t airngin_runtime_cancel_provisioning(void)
{
#if CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI
    if (airngin_internal_network_mode() != AIRNGIN_NETWORK_EXTERNAL) return airngin_managed_cancel_provisioning();
#endif
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t airngin_runtime_request_provisioning(void)
{
#if CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI
    if (airngin_internal_network_mode() != AIRNGIN_NETWORK_EXTERNAL) return airngin_managed_request_provisioning();
#endif
    return ESP_ERR_NOT_SUPPORTED;
}
