#include "AirNginESP32.h"

AirNginESP32Class AirNgin;

static esp_err_t start_config(airngin_config_t &cfg)
{
    esp_err_t err = airngin_init(&cfg);
    if (err != ESP_OK) return err;
    err = airngin_start();
    if (err != ESP_OK) {
        (void)airngin_deinit();
    }
    return err;
}

esp_err_t AirNginESP32Class::beginManaged(const char *serial,
                                          const char *producerCode,
                                          airngin_command_handler_t commandHandler,
                                          airngin_event_handler_t eventHandler,
                                          void *userContext,
                                          const char *provisioningLabel,
                                          const char *provisioningPassword)
{
    airngin_config_t cfg = AIRNGIN_CONFIG_DEFAULT();
    cfg.serial = serial;
    cfg.producer_code = producerCode;
    cfg.network_mode = AIRNGIN_NETWORK_MANAGED_WIFI;
    cfg.provisioning_label = provisioningLabel;
    cfg.provisioning_password = provisioningPassword;
    cfg.command_handler = commandHandler;
    cfg.event_handler = eventHandler;
    cfg.user_ctx = userContext;
    cfg.cloud_credentials = nullptr;
    return start_config(cfg);
}

esp_err_t AirNginESP32Class::beginExternal(const char *serial,
                                           const char *producerCode,
                                           const airngin_cloud_credentials_t &cloud,
                                           airngin_command_handler_t commandHandler,
                                           airngin_event_handler_t eventHandler,
                                           void *userContext,
                                           uint32_t reconnectDelayMs)
{
    airngin_config_t cfg = AIRNGIN_CONFIG_DEFAULT();
    cfg.serial = serial;
    cfg.producer_code = producerCode;
    cfg.network_mode = AIRNGIN_NETWORK_EXTERNAL;
    cfg.command_handler = commandHandler;
    cfg.event_handler = eventHandler;
    cfg.user_ctx = userContext;
    cfg.cloud_credentials = &cloud;
    cfg.external_reconnect_delay_ms = reconnectDelayMs;
    return start_config(cfg);
}

esp_err_t AirNginESP32Class::stop()
{
    return airngin_stop();
}

esp_err_t AirNginESP32Class::end()
{
    esp_err_t err = airngin_stop();
    if (err != ESP_OK) return err;
    return airngin_deinit();
}
