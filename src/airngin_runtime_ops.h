#pragma once

#include "airngin_internal.h"

/* Managed-Wi-Fi runtime (airngin_provision.c). Only built when CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI. */
esp_err_t airngin_managed_start(void);
void airngin_managed_release_owner(void);
esp_err_t airngin_managed_stop(void);
bool airngin_managed_in_owner_task(void);
void airngin_managed_get_status(airngin_runtime_status_t *out);
esp_err_t airngin_managed_publish(const char *key, size_t key_len, const char *value, size_t value_len);
esp_err_t airngin_managed_request_provisioning(void);
esp_err_t airngin_managed_notify(bool to_user, int channel, const char *code, size_t code_len);
esp_err_t airngin_managed_cancel_provisioning(void);

/* External-network runtime (airngin_external.c). Never touches Wi-Fi, netifs, SNTP, HTTP or NVS. */
esp_err_t airngin_external_start(void);
void airngin_external_release_owner(void);
esp_err_t airngin_external_stop(void);
bool airngin_external_in_owner_task(void);
void airngin_external_get_status(airngin_runtime_status_t *out);
esp_err_t airngin_external_publish(const char *key, size_t key_len, const char *value, size_t value_len);
esp_err_t airngin_external_notify(bool to_user, int channel, const char *code, size_t code_len);
