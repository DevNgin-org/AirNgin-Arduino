#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// tick/stop belong to one firmware owner task, never an SDK callback or a mutex-held section.
// trusted is an atomic snapshot for transport tasks. No network operation precedes a real IP.
void airngin_clock_tick(bool wifi_has_ip);
// EXTERNAL network mode: the host owns time acquisition. When enabled, AirNgin only DETECTS whether the
// system clock is trusted (epoch >= 2024-01-01) and never starts or stops SNTP. Default: acquisition (managed).
void airngin_clock_set_external(bool trust_detection_only);
void airngin_clock_stop(void);
bool airngin_clock_trusted(void);
#ifdef __cplusplus
}
#endif
