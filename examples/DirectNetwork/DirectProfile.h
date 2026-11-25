#pragma once
#include "airngin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char project[AIRNGIN_CLOUD_PROJECT_CODE_MAX + 1];
    char broker[AIRNGIN_CLOUD_BROKER_HOST_MAX + 1];
    char user[AIRNGIN_CLOUD_USERNAME_MAX + 1];
    char password[AIRNGIN_CLOUD_PASSWORD_MAX + 1];
    char clientId[AIRNGIN_CLOUD_CLIENT_ID_MAX + 1];
    char aesKey[45];
} DirectProfile;

bool parseDirectProfile(const char *json, size_t size, DirectProfile *out);
airngin_cloud_credentials_t directCredentials(const DirectProfile *profile);

#ifdef __cplusplus
}
#endif
