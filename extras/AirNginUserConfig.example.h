#pragma once
/* Optional C-header override. Rename to AirNginUserConfig.h and make sure your
 * sketch/project include directory is visible while the library is compiled.
 * Arduino IDE users should prefer build_opt.h because it is applied globally. */
#define CONFIG_AIRNGIN_USE_AES 0
#define CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE 1
#define CONFIG_AIRNGIN_DIAGNOSTIC_LOGS 1
#define CONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE 1
#define CONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS 0
#define CONFIG_AIRNGIN_NOTIFICATION_TYPE_AS_STRING 0
