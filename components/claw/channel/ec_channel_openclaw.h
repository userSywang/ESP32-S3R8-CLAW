#ifndef __EC_CHANNEL_OPENCLAW_H__
#define __EC_CHANNEL_OPENCLAW_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ec_channel_openclaw_get_health(bool *connected,
                                    uint32_t *connect_count,
                                    uint32_t *disconnect_count,
                                    int64_t *last_recv_age_ms,
                                    int64_t *last_send_age_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __EC_CHANNEL_OPENCLAW_H__ */
