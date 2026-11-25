// Adapt the mobile direct-LAN payload to the existing external-network SDK.
#include "DirectProfile.h"
#include "airngin_internal.h"
#include "cJSON.h"
#include <string.h>

airngin_cloud_credentials_t directCredentials(const DirectProfile *p) {
    return (airngin_cloud_credentials_t){
        .project_code=p->project, .broker_host=p->broker, .username=p->user,
        .password=p->password, .client_id=p->clientId, .encryption_key=p->aesKey
    };
}

static bool copyText(const cJSON *root, const char *name, char *out, size_t capacity) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name));
    if (!s || strlen(s) >= capacity) return false;
    strcpy(out, s);
    return true;
}

bool parseDirectProfile(const char *json, size_t size, DirectProfile *out) {
    if (!json || !out || !size || size > 4096 || memchr(json, 0, size)) return false;
    for (size_t i = 0; i + 1 < size; ++i) {
        if (json[i] == '\\') {
            if (size - i >= 6 && !memcmp(json+i+1, "u0000", 5)) return false;
            ++i;
        }
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (!root) return false;
    while (end < json+size && strchr(" \t\r\n", *end)) ++end;
    bool ok = cJSON_IsObject(root) && end == json+size;
    for (cJSON *v=root->child; ok && v; v=v->next) {
        for (cJSON *prior=root->child; prior!=v; prior=prior->next)
            if (!strcmp(v->string,prior->string)) { ok=false; break; }
        if (!strcmp(v->string,"modemSsid") || !strcmp(v->string,"modemPass"))
            ok = ok && cJSON_IsString(v) && !v->valuestring[0];
    }
    DirectProfile p = {0};
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "isUseEncrypted");
    ok = ok && (!mode || (cJSON_IsBool(mode) && !!cJSON_IsTrue(mode) == !!CONFIG_AIRNGIN_USE_AES)) &&
         copyText(root,"projectCode",p.project,sizeof p.project) &&
         copyText(root,"mqttBroker",p.broker,sizeof p.broker) &&
         copyText(root,"mqttUser",p.user,sizeof p.user) &&
         copyText(root,"mqttPass",p.password,sizeof p.password) &&
         copyText(root,"clientId",p.clientId,sizeof p.clientId);
#if CONFIG_AIRNGIN_USE_AES
    ok = ok && copyText(root,"encryptionKey",p.aesKey,sizeof p.aesKey);
#endif
    cJSON_Delete(root);
    airngin_profile checked = {0};
    airngin_cloud_credentials_t cloud = directCredentials(&p);
    ok = ok && airngin_cloud_to_profile(&cloud, &checked);
    if (ok) *out = p;
    airngin_secure_zero(&p, sizeof p);
    airngin_secure_zero(&checked, sizeof checked);
    return ok;
}
