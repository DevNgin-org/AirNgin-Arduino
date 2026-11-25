#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char modemSsid[33];
    char modemPass[65];
    char projectCode[21];
    char mqttBroker[101];
    char mqttUser[17];
    char mqttPass[17];
    char clientId[39];
    bool recovery;
    /* CONFIG_AIRNGIN_USE_AES builds only: Base64 of the 32-byte AES-256-GCM key ("encryptionKey"). Always zero in PLAIN
     * builds. It is NOT part of the legacy "profile" NVS blob (that blob keeps its original 294-byte layout so existing
     * PLAIN profiles stay valid); AES builds store it under its own NVS key in the same commit. */
    char aesKey[45];
} airngin_profile;

/* Size of the persisted legacy blob: everything before aesKey. */
#define AIRNGIN_PROFILE_BLOB_SIZE offsetof(airngin_profile, aesKey)
_Static_assert(offsetof(airngin_profile, aesKey) == 294U, "the legacy NVS profile blob layout must not change");

#include "AirNginConfig.h"
#if CONFIG_AIRNGIN_DIAGNOSTIC_LOGS
/* Diagnostic dump of a profile: secrets follow the diagnostic secret policy (hidden + length + fingerprint by default). */
void airngin_diag_log_profile(const char *tag, const char *title, const airngin_profile *p);
#endif

esp_err_t airngin_profile_load(airngin_profile *out);
esp_err_t airngin_profile_save(const airngin_profile *profile);
esp_err_t airngin_profile_parse(const char *json, unsigned length, airngin_profile *out);
bool airngin_profile_valid(const airngin_profile *profile);
