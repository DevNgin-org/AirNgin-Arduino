#pragma once
/*
 * AirNgin ESP32 Arduino build configuration.
 *
 * Arduino IDE has no project Kconfig. These macros replace the AirNgin Kconfig
 * options used by the ESP-IDF component. For PlatformIO every value can also
 * be overridden with build_flags (-D...).
 *
 * Keep secrets out of this file. Device identity / provisioning password can
 * be supplied at runtime through airngin_config_t or the AirNginESP32 wrapper.
 */
/* A project override must be visible to every library translation unit.
 * In Arduino IDE use sketch-local build_opt.h for global -D options. */
#if defined(__has_include)
#  if __has_include("AirNginUserConfig.h")
#    include "AirNginUserConfig.h"
#  endif
#endif

#ifndef CONFIG_AIRNGIN_PRODUCER
#define CONFIG_AIRNGIN_PRODUCER "VTRA"
#endif
#ifndef CONFIG_AIRNGIN_SERIAL
#define CONFIG_AIRNGIN_SERIAL "VTRA0000000000"
#endif
#ifndef CONFIG_AIRNGIN_PROVISIONING_PASSWORD
#define CONFIG_AIRNGIN_PROVISIONING_PASSWORD "00000000"
#endif

#ifndef CONFIG_AIRNGIN_USE_AES
#define CONFIG_AIRNGIN_USE_AES 0
#endif
#ifndef CONFIG_AIRNGIN_NOTIFICATION_AES_CONFIRMED
#define CONFIG_AIRNGIN_NOTIFICATION_AES_CONFIRMED 0
#endif
#ifndef CONFIG_AIRNGIN_NOTIFICATION_TYPE_AS_STRING
#define CONFIG_AIRNGIN_NOTIFICATION_TYPE_AS_STRING 0
#endif

/* Preserves the ESP-IDF SDK default: Cloud Database is opt-in because it adds
 * HTTPS/TLS code and increases application size. Enable it in AirNginUserConfig.h. */
#ifndef CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE
#define CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE 0
#endif
#ifndef CONFIG_AIRNGIN_DATABASE_ALLOW_IN_AES_BUILD
#define CONFIG_AIRNGIN_DATABASE_ALLOW_IN_AES_BUILD 0
#endif
#ifndef CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS
#define CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS 0
#endif
#ifndef CONFIG_AIRNGIN_DATABASE_VALUE_MAX
#define CONFIG_AIRNGIN_DATABASE_VALUE_MAX 4096
#endif
#ifndef CONFIG_AIRNGIN_DATABASE_TIMEOUT_MS
#define CONFIG_AIRNGIN_DATABASE_TIMEOUT_MS 15000
#endif
#ifndef CONFIG_AIRNGIN_DATABASE_TASK_STACK_SIZE
#define CONFIG_AIRNGIN_DATABASE_TASK_STACK_SIZE 8192
#endif

#ifndef CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI
#define CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI 1
#endif
#ifndef CONFIG_AIRNGIN_EXTERNAL_RECONNECT_DELAY_MS
#define CONFIG_AIRNGIN_EXTERNAL_RECONNECT_DELAY_MS 3000
#endif
#ifndef CONFIG_AIRNGIN_TASK_STACK_SIZE
#define CONFIG_AIRNGIN_TASK_STACK_SIZE 16384
#endif
#ifndef CONFIG_AIRNGIN_COMMAND_QUEUE_DEPTH
#define CONFIG_AIRNGIN_COMMAND_QUEUE_DEPTH 8
#endif
#ifndef CONFIG_AIRNGIN_TX_QUEUE_DEPTH
#define CONFIG_AIRNGIN_TX_QUEUE_DEPTH 8
#endif

#ifndef CONFIG_AIRNGIN_DIAGNOSTIC_LOGS
#define CONFIG_AIRNGIN_DIAGNOSTIC_LOGS 0
#endif
#ifndef CONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE
#define CONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE 0
#endif
#ifndef CONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS
#define CONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS 0
#endif
#ifndef CONFIG_AIRNGIN_STACK_DIAGNOSTICS
#define CONFIG_AIRNGIN_STACK_DIAGNOSTICS 0
#endif

/* App/test-only flags referenced by the shared diagnostic code. */
#ifndef CONFIG_AIRNGIN_SERIAL_TEST_CONSOLE
#define CONFIG_AIRNGIN_SERIAL_TEST_CONSOLE 0
#endif
#ifndef CONFIG_AIRNGIN_ALLOW_INSECURE_MQTT_TEST
#define CONFIG_AIRNGIN_ALLOW_INSECURE_MQTT_TEST 0
#endif

#define AIRNGIN_ARDUINO 1
