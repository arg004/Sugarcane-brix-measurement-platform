// firmware/components/telemetry/include/telemetry/heartbeat.h  (new)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// start background heartbeat; idempotent; returns false on immediate config error
bool heartbeat_start(void);

// attach an optional user key/value pair included in every payload (thread-safe copy)
bool heartbeat_set_extra_kv(const char *key, const char *value);

#ifdef __cplusplus
}
#endif