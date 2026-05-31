// firmware/components/network/include/network/sntp_time.h  (new)
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void sntp_start(const char *tz);
bool sntp_wait_for_sync(unsigned timeout_ms);
void sntp_format_time_now(char *out, size_t len); // RFC3339 localtime

#ifdef __cplusplus
}
#endif