// firmware/components/network/include/network/network_task.h  (new)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Public network events (IPC contract):
 * - NET_EVT_WIFI_READY  : Wi‑Fi/NIC initialized (safe to create sockets later).
 * - NET_EVT_WIFI_IP     : Station has an IPv4 address (network usable).
 * - NET_EVT_TIME_SYNCED : SNTP completed at least once after a link‑up.
 *
 * Notes:
 * - Bits are **sticky** while the condition holds; they are **cleared** on disconnect.
 * - Higher layers (uploader/MQTT) should wait on these bits instead of polling.
 */
#define NET_EVT_WIFI_READY   BIT0
#define NET_EVT_WIFI_IP      BIT1
#define NET_EVT_TIME_SYNCED  BIT2

/** Start the network orchestration task (idempotent). */
void network_task_start(void);

/** Return the public EventGroup used for synchronization. */
EventGroupHandle_t network_events(void);

/** Format current local time; returns false if buffer is too small. */
bool network_get_time(char *buf, size_t len);

#ifdef __cplusplus
}
#endif