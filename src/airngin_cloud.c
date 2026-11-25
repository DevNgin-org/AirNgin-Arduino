#include "airngin_internal.h"
#include "airngin_utf8.h"
#include <string.h>
#include "AirNginConfig.h"
#if CONFIG_AIRNGIN_USE_AES
#include "airngin_crypto.h"
#endif

/* Printable UTF-8, no control characters, 1..cap bytes. */
static bool cloud_text_valid(const char *s, size_t cap)
{
    const size_t n = strnlen(s, cap + 1U);
    if (n == 0U || n > cap) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 32U || c == 127U) return false;
    }
    return airngin_utf8_valid((const unsigned char *)s, n);
}

static bool cloud_host_valid(const char *h)
{
    size_t label = 0;
    char prev = 0;
    for (; *h; ++h) {
        const unsigned char c = (unsigned char)*h;
        if (c == '.') {
            if (!label || prev == '-') return false;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') ||
                (!label && c == '-') || ++label > 63U) return false;
        }
        prev = (char)c;
    }
    return label && prev != '-';
}

bool airngin_cloud_valid(const airngin_profile *p)
{
    if (!p) return false;
    if (!cloud_text_valid(p->projectCode, AIRNGIN_CLOUD_PROJECT_CODE_MAX) ||
        strpbrk(p->projectCode, "/+#")) {          /* it becomes part of the MQTT topics */
        return false;
    }
    return cloud_text_valid(p->mqttBroker, AIRNGIN_CLOUD_BROKER_HOST_MAX) && cloud_host_valid(p->mqttBroker) &&
           cloud_text_valid(p->mqttUser, AIRNGIN_CLOUD_USERNAME_MAX) &&
           cloud_text_valid(p->mqttPass, AIRNGIN_CLOUD_PASSWORD_MAX) &&
           cloud_text_valid(p->clientId, AIRNGIN_CLOUD_CLIENT_ID_MAX)
#if CONFIG_AIRNGIN_USE_AES
           && airngin_aes_key_valid(p->aesKey)      /* AES build: no valid key, no credentials (never a plaintext fallback) */
#endif
           ;
}

bool airngin_cloud_to_profile(const airngin_cloud_credentials_t *c, airngin_profile *out)
{
    if (!c || !out || !c->project_code || !c->broker_host || !c->username || !c->password || !c->client_id) {
        return false;
    }
    airngin_profile p;
    memset(&p, 0, sizeof(p));
    /* Length-check before copying so an over-long input can never overrun the fixed fields. */
    if (strnlen(c->project_code, sizeof(p.projectCode)) >= sizeof(p.projectCode) ||
        strnlen(c->broker_host, sizeof(p.mqttBroker)) >= sizeof(p.mqttBroker) ||
        strnlen(c->username, sizeof(p.mqttUser)) >= sizeof(p.mqttUser) ||
        strnlen(c->password, sizeof(p.mqttPass)) >= sizeof(p.mqttPass) ||
        strnlen(c->client_id, sizeof(p.clientId)) >= sizeof(p.clientId)) {
        return false;
    }
    strcpy(p.projectCode, c->project_code);
    strcpy(p.mqttBroker, c->broker_host);
    strcpy(p.mqttUser, c->username);
    strcpy(p.mqttPass, c->password);
    strcpy(p.clientId, c->client_id);
#if CONFIG_AIRNGIN_USE_AES
    if (!c->encryption_key || strnlen(c->encryption_key, sizeof(p.aesKey)) >= sizeof(p.aesKey)) {
        airngin_secure_zero(&p, sizeof(p));
        return false;
    }
    strcpy(p.aesKey, c->encryption_key);
#endif
    if (!airngin_cloud_valid(&p)) {
        airngin_secure_zero(&p, sizeof(p));
        return false;
    }
    *out = p;
    airngin_secure_zero(&p, sizeof(p));
    return true;
}
