#ifndef __EC_CHANNEL_FEISHU_H__
#define __EC_CHANNEL_FEISHU_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ec_channel_feishu_get_health(bool *connected,
                                  uint32_t *connect_count,
                                  uint32_t *disconnect_count,
                                  int64_t *last_event_age_ms,
                                  int64_t *last_send_age_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __EC_CHANNEL_FEISHU_H__ */
