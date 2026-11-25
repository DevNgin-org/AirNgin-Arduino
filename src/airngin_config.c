#include "airngin_profile.h"
#include "airngin_diag.h"
#include "airngin_internal.h"
#include "cJSON.h"
#include "nvs.h"
#include "AirNginConfig.h"
#if CONFIG_AIRNGIN_USE_AES
#include "airngin_crypto.h"
#endif
#include <string.h>
#include <stddef.h>
#include <ctype.h>

/*
 * Stored profile (NVS namespace "airngin", blob key "profile") and the provisioning payload parser.
 * The validation / parsing / save semantics are unchanged; the code is spelled out step by step so the
 * diagnostic builds can say exactly which step passed or failed (never printing a secret unless the
 * dangerous AIRNGIN_DIAGNOSTIC_LOG_SECRETS option is on).
 */
static const char *ag_fields[]={"modemSsid","modemPass","projectCode","mqttBroker","mqttUser","mqttPass","clientId"};
static const size_t ag_offsets[]={offsetof(airngin_profile,modemSsid),offsetof(airngin_profile,modemPass),offsetof(airngin_profile,projectCode),offsetof(airngin_profile,mqttBroker),offsetof(airngin_profile,mqttUser),offsetof(airngin_profile,mqttPass),offsetof(airngin_profile,clientId)};
static const size_t ag_limits[]={32,64,20,100,16,16,38};

static bool ag_utf8(const char *s,size_t cap){
 size_t n=strnlen(s,cap+1); if(!n||n>cap)return false;
 for(size_t i=0;i<n;){unsigned c=(unsigned char)s[i++]; if(c<32||c==127)return false;if(c<128)continue;
 unsigned k,v,min;if(c>=194&&c<=223){k=1;v=c&31;min=128;}else if(c>=224&&c<=239){k=2;v=c&15;min=2048;}else if(c>=240&&c<=244){k=3;v=c&7;min=65536;}else return false;
 if(i+k>n)return false;
 while(k--){c=(unsigned char)s[i++];if((c&192)!=128)return false;v=(v<<6)|(c&63);}if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return false;
 }return true;
}

/* DNS-style host name check (unchanged logic, split out so diagnostics can report it separately). */
static bool ag_host_valid(const char *h){
 size_t label=0;char prev=0;
 for(;*h;h++){unsigned char c=*h;if(c=='.'){if(!label||prev=='-')return false;label=0;}else{if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-')||(!label&&c=='-')||++label>63)return false;}prev=c;}
 return label&&prev!='-';
}

bool airngin_profile_valid(const airngin_profile *p){
 for(unsigned i=0;i<7;i++)if(!ag_utf8((const char*)p+ag_offsets[i],ag_limits[i]))return false;
 if(!ag_host_valid(p->mqttBroker))return false;
#if CONFIG_AIRNGIN_USE_AES
 if(!airngin_aes_key_valid(p->aesKey))return false;   /* AES build: a profile without usable key material is not a profile */
#endif
 return true;
}

#if AG_DIAG
/* Why a field fails ag_utf8(): empty / too long / invalid UTF-8 or control character. NULL = valid. */
static const char *ag_field_reason(const char *s,size_t cap){
 const size_t n=strnlen(s,cap+1);
 if(!n)return "empty";
 if(n>cap)return "too long";
 return ag_utf8(s,cap)?NULL:"invalid UTF-8 or control character";
}

static const char *const ag_labels[]={"Wi-Fi SSID","Wi-Fi password","project code","broker host","username","MQTT password","client ID"};

/* Step-by-step validation report of a fully parsed profile. Never prints a secret value. */
static void ag_diag_validation_report(const airngin_profile *p){
 for(unsigned i=0;i<7;i++){
  const char *field=(const char*)p+ag_offsets[i];
  const char *reason=ag_field_reason(field,ag_limits[i]);
  if(!reason&&i==3&&!ag_host_valid(field))reason="invalid host name";
  if(reason)AG_DIAG_W("AG_PROV","validate %-14s FAIL (%s, max %u)",ag_labels[i],reason,(unsigned)ag_limits[i]);
  else AG_DIAG_I("AG_PROV","validate %-14s PASS (len=%u/%u)",ag_labels[i],(unsigned)strlen(field),(unsigned)ag_limits[i]);
 }
}

/* The seven stored fields with the real struct member names; secrets obey the diagnostic secret policy. */
void airngin_diag_log_profile(const char *tag,const char *title,const airngin_profile *p){
 ESP_LOGI(tag,"========== %s ==========",title);
 for(unsigned i=0;i<7;i++){
  const char *field=(const char*)p+ag_offsets[i];
  const bool secret=(i==1||i==4||i==5);   /* Wi-Fi password, username, MQTT password */
  airngin_diag_field(tag,ag_fields[i],field,strnlen(field,ag_limits[i]+1),secret);
 }
 ESP_LOGI(tag,"%-14s = %s","recovery",p->recovery?"true":"false");
#if CONFIG_AIRNGIN_USE_AES
 {char fp[9];const size_t kl=strnlen(p->aesKey,sizeof(p->aesKey));airngin_diag_fingerprint(p->aesKey,kl,fp);
  ESP_LOGI(tag,"%-14s = <never printed> len=%u fp=%s","encryptionKey",(unsigned)kl,fp);}
#endif
 for(unsigned i=0;i<7;i++){
  if(i==0||i==1||i==4||i==5){const char *field=(const char*)p+ag_offsets[i];airngin_diag_hex(tag,ag_fields[i],field,strnlen(field,ag_limits[i]+1));}
 }
 ESP_LOGI(tag,"==========================================");
}
#endif

esp_err_t airngin_profile_parse(const char *s,unsigned n,airngin_profile *out){
 if(!n||n>4096||memchr(s,0,n)){
  AG_DIAG_W("AG_PROV","payload rejected: %s",!n?"empty":n>4096?"longer than 4096 bytes":"embedded NUL byte");
  return ESP_ERR_INVALID_ARG;
 }
 /* Reject escaped NUL before cJSON can collapse a string to a C prefix. */
 for(unsigned i=0;i<n;i++)if(s[i]=='\\'){if(i+5<n&&s[i+1]=='u'&&!memcmp(s+i+2,"0000",4)){AG_DIAG_W("AG_PROV","payload rejected: escaped NUL (\\u0000)");return ESP_ERR_INVALID_ARG;}i++;}
 const char *end=NULL;cJSON *root=cJSON_ParseWithLengthOpts(s,n,&end,false);
 if(!root){AG_DIAG_W("AG_PROV","payload rejected: JSON syntax error near byte %u",(unsigned)(end?(unsigned)(end-s):0));return ESP_ERR_INVALID_ARG;}
 while(end<s+n&&isspace((unsigned char)*end))end++;
 bool ok=end==s+n&&cJSON_IsObject(root);unsigned seen=0;airngin_profile p={0};bool aes_key_seen=false;(void)aes_key_seen;
 if(!ok)AG_DIAG_W("AG_PROV","payload rejected: %s",end!=s+n?"trailing bytes after the JSON value":"top-level value is not an object");
 for(cJSON *v=root->child;ok&&v;v=v->next){
  int f=-1;for(int i=0;i<7;i++)if(!strcmp(v->string,ag_fields[i]))f=i;
  if(f>=0){
   if(seen&(1u<<f)){AG_DIAG_W("AG_PROV","validation FAILED field=%s reason=duplicate key",ag_fields[f]);ok=false;break;}
   if(!cJSON_IsString(v)){AG_DIAG_W("AG_PROV","validation FAILED field=%s reason=not a JSON string",ag_fields[f]);ok=false;break;}
   if(!ag_utf8(v->valuestring,ag_limits[f])){AG_DIAG_W("AG_PROV","validation FAILED field=%s reason=%s (max %u)",ag_fields[f],AG_DIAG?ag_field_reason(v->valuestring,ag_limits[f]):"",(unsigned)ag_limits[f]);ok=false;break;}
   seen|=1u<<f;strcpy((char*)&p+ag_offsets[f],v->valuestring);
  }
  else if(!strcmp(v->string,"isUseEncrypted")){
#if CONFIG_AIRNGIN_USE_AES
   /* The compile-time mode is authoritative: an AES firmware never accepts "plaintext", and never silently changes mode. */
   if(!cJSON_IsBool(v)||!cJSON_IsTrue(v)){AG_DIAG_W("AG_PROV","validation FAILED field=isUseEncrypted reason=must be boolean true (this firmware is built with CONFIG_AIRNGIN_USE_AES=y; there is no plaintext fallback)");ok=false;}
#else
   if(!cJSON_IsBool(v)||cJSON_IsTrue(v)){AG_DIAG_W("AG_PROV","validation FAILED field=isUseEncrypted reason=must be boolean false (this firmware is built PLAIN: CONFIG_AIRNGIN_USE_AES=n)");ok=false;}
#endif
  }
#if CONFIG_AIRNGIN_USE_AES
  else if(!strcmp(v->string,"encryptionKey")){
   if(aes_key_seen){AG_DIAG_W("AG_PROV","validation FAILED field=encryptionKey reason=duplicate key");ok=false;break;}
   if(!cJSON_IsString(v)||!airngin_aes_key_valid(v->valuestring)){AG_DIAG_W("AG_PROV","validation FAILED field=encryptionKey reason=Encryption key missing or invalid (needs Base64 of exactly 32 bytes)");ok=false;break;}
   strcpy(p.aesKey,v->valuestring);aes_key_seen=true;   /* length is bounded by the 44-character Base64 check above */
  }
#endif
  else if(!strcmp(v->string,"closeConfig")){if(!cJSON_IsBool(v)){AG_DIAG_W("AG_PROV","validation FAILED field=closeConfig reason=must be boolean");ok=false;}}
  else if(
#if !CONFIG_AIRNGIN_USE_AES
   strcmp(v->string,"encryptionKey")&&   /* PLAIN: accepted and ignored, exactly as before */
#endif
   strcmp(v->string,"section")&&strcmp(v->string,"circuit")&&strcmp(v->string,"extender")&&strcmp(v->string,"modemChannel")&&strcmp(v->string,"encryptionKeyProject")){
   AG_DIAG_W("AG_PROV","validation FAILED unknown key \"%.40s\" (not part of the provisioning contract)",v->string);ok=false;
  }
 }
#if AG_DIAG
 if(ok&&seen!=127){for(unsigned i=0;i<7;i++)if(!(seen&(1u<<i)))AG_DIAG_W("AG_PROV","validation FAILED field=%s reason=missing",ag_fields[i]);}
 if(ok&&seen==127)ag_diag_validation_report(&p);
#endif
#if CONFIG_AIRNGIN_USE_AES
 if(ok&&!aes_key_seen){AG_DIAG_W("AG_PROV","validation FAILED field=encryptionKey reason=missing (Encryption key missing or invalid; AES build)");ok=false;}
 AG_DIAG_I("AG_PROV","Application protocol : AES (compile-time CONFIG_AIRNGIN_USE_AES=y)");
 AG_DIAG_I("AG_PROV","AES key material      : %s",aes_key_seen?"present":"MISSING");
#if AG_DIAG
 if(aes_key_seen){char fp[9];const size_t kl=strnlen(p.aesKey,sizeof(p.aesKey));airngin_diag_fingerprint(p.aesKey,kl,fp);
  AG_DIAG_I("AG_PROV","AES key validation    : PASS (Base64 -> 32 bytes, text length=%u fp=%s; key is never printed)",(unsigned)kl,fp);}
#endif
#else
 AG_DIAG_I("AG_PROV","Application protocol : PLAIN (compile-time CONFIG_AIRNGIN_USE_AES=n)");
 AG_DIAG_I("AG_PROV","Provision payload AES fields : ignored / not required (isUseEncrypted must be false)");
#endif
 ok=ok&&seen==127&&airngin_profile_valid(&p);cJSON_Delete(root);
 if(!ok){AG_DIAG_W("AG_PROV","payload validation FAILED (see the lines above)");airngin_secure_zero(&p,sizeof(p));return ESP_ERR_INVALID_ARG;}
 *out=p;airngin_secure_zero(&p,sizeof(p));
 AG_DIAG_I("AG_PROV","payload parse OK, all 7 required fields valid");
 return ESP_OK;
}

/* Single place that reads the blob. `log` is false for internal reads (save compares / verifies). */
static esp_err_t ag_load_impl(airngin_profile *p,bool log){
 nvs_handle_t h;
 if(log)AG_DIAG_I("AG_NVS","opening namespace \"airngin\" (read-only)");
 esp_err_t e=nvs_open("airngin",NVS_READONLY,&h);
 if(e){
  if(log)AG_DIAG_I("AG_NVS","nvs_open failed " AG_ERRF " - no profile stored yet",AG_ERRV(e));
  return e;
 }
 if(log)AG_DIAG_I("AG_NVS","reading profile blob \"profile\"");
 memset(p,0,sizeof(*p));
 size_t n=sizeof(*p);
 e=nvs_get_blob(h,"profile",p,&n);
#if CONFIG_AIRNGIN_USE_AES
 if(e==ESP_OK&&n==AIRNGIN_PROFILE_BLOB_SIZE){   /* AES key material has its own NVS key; it is never inferred from other bytes */
  size_t kn=sizeof(p->aesKey);
  const esp_err_t ke=nvs_get_blob(h,"aeskey",p->aesKey,&kn);
  if(ke!=ESP_OK||kn!=sizeof(p->aesKey)){
   if(log)AG_DIAG_W("AG_NVS","AES build: key material \"aeskey\" is missing (" AG_ERRF ") - profile ignored, provisioning required",AG_ERRV(ke));
   nvs_close(h);memset(p,0,sizeof(*p));return ESP_ERR_INVALID_STATE;
  }
  p->aesKey[sizeof(p->aesKey)-1]=0;
 }
#endif
 nvs_close(h);
 if(e!=ESP_OK){
  if(log)AG_DIAG_I("AG_NVS","no profile blob: nvs_get_blob " AG_ERRF,AG_ERRV(e));
  return ESP_ERR_INVALID_STATE;
 }
 if(log)AG_DIAG_I("AG_NVS","profile length=%u (expected %u)",(unsigned)n,(unsigned)AIRNGIN_PROFILE_BLOB_SIZE);
 if(n!=AIRNGIN_PROFILE_BLOB_SIZE){
  if(log)AG_DIAG_W("AG_NVS","profile blob has the wrong size: ignored");
  return ESP_ERR_INVALID_STATE;
 }
 if(!airngin_profile_valid(p)){
  if(log)AG_DIAG_W("AG_NVS","profile validation = FAILED: ignored");
  return ESP_ERR_INVALID_STATE;
 }
 if(log)AG_DIAG_I("AG_NVS","parse OK; profile validation = OK");
 return ESP_OK;
}

esp_err_t airngin_profile_load(airngin_profile *p){
 const esp_err_t e=ag_load_impl(p,true);
#if AG_DIAG
 if(e==ESP_OK){
  AG_DIAG_I("AG_NVS","profile found");
  airngin_diag_log_profile("AG_NVS","Stored Profile",p);
  AG_DIAG_I("AG_NVS","Wi-Fi SSID=\"%s\" Wi-Fi password length=%u",p->modemSsid,(unsigned)strlen(p->modemPass));
 }else{
  AG_DIAG_I("AG_NVS","no valid AirNgin profile found");
  AG_DIAG_I("AG_PROV","automatic provisioning required");
 }
#endif
 return e;
}

esp_err_t airngin_profile_save(const airngin_profile *p){
 if(!airngin_profile_valid(p)){AG_DIAG_W("AG_NVS","save refused: profile is not valid");return ESP_ERR_INVALID_ARG;}
 airngin_profile old,check;
 const bool had=ag_load_impl(&old,false)==ESP_OK;
 if(had&&!memcmp(&old,p,sizeof(old))){AG_DIAG_I("AG_NVS","identical profile already stored: nothing to write");return ESP_OK;}
 AG_DIAG_I("AG_NVS","opening namespace \"airngin\" (read/write)");
 nvs_handle_t h;
 esp_err_t e=nvs_open("airngin",NVS_READWRITE,&h);
 if(e){AG_DIAG_E("AG_NVS","nvs_open failed " AG_ERRF,AG_ERRV(e));return e;}
 AG_DIAG_I("AG_NVS","writing new profile (%u bytes, previous profile %s)",(unsigned)AIRNGIN_PROFILE_BLOB_SIZE,had?"exists":"none");
 e=nvs_set_blob(h,"profile",p,AIRNGIN_PROFILE_BLOB_SIZE);
#if CONFIG_AIRNGIN_USE_AES
 if(!e)e=nvs_set_blob(h,"aeskey",p->aesKey,sizeof(p->aesKey));   /* same commit as the profile: all or nothing */
#endif
 if(!e){e=nvs_commit(h);AG_DIAG_I("AG_NVS","commit result=" AG_ERRF,AG_ERRV(e));}
 else AG_DIAG_E("AG_NVS","nvs_set_blob failed " AG_ERRF,AG_ERRV(e));
 if(!e){
  AG_DIAG_I("AG_NVS","readback verification begin");
  e=ag_load_impl(&check,false);
 }
 if(!e&&memcmp(&check,p,sizeof(check))){e=ESP_FAIL;AG_DIAG_E("AG_NVS","readback differs from what was written");}
 if(e){
  AG_DIAG_E("AG_NVS","verification failed " AG_ERRF,AG_ERRV(e));
  AG_DIAG_W("AG_NVS","%s",had?"restoring previous profile":"removing the partially written profile");
 if(had){nvs_set_blob(h,"profile",&old,AIRNGIN_PROFILE_BLOB_SIZE);
#if CONFIG_AIRNGIN_USE_AES
   nvs_set_blob(h,"aeskey",old.aesKey,sizeof(old.aesKey));
#endif
  }else{nvs_erase_key(h,"profile");
#if CONFIG_AIRNGIN_USE_AES
   nvs_erase_key(h,"aeskey");
#endif
  }
  airngin_secure_zero(&old,sizeof(old));
  nvs_commit(h);
 }else{
  AG_DIAG_I("AG_NVS","readback verification PASS");
#if !CONFIG_AIRNGIN_USE_AES
  /* PLAIN build: a stale AES key from an earlier AES firmware must not be paired with this new profile later. */
  if(nvs_erase_key(h,"aeskey")==ESP_OK)(void)nvs_commit(h);
#endif
#if AG_DIAG
  airngin_diag_log_profile("AG_NVS","NVS PROFILE READBACK",&check);
#endif
 }
 nvs_close(h);
 airngin_secure_zero(&check,sizeof(check));
 return e;
}
