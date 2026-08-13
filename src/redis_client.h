#ifndef VEK_REDIS_CLIENT_H
#define VEK_REDIS_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Redis client stub interface.
 * Connection is configured via environment variable:
 *   REDIS_URL - Redis connection URL (e.g., redis://localhost:6379)
 *
 * Redis is optional. If REDIS_URL is not set, redis_connect returns NULL.
 */

#define REDIS_OK        0
#define REDIS_ERROR     1
#define REDIS_NIL       2

typedef struct redis_conn redis_conn;

redis_conn* redis_connect(void);
void redis_disconnect(redis_conn* conn);

int redis_get(redis_conn* conn, const char* key, char** value, int* value_len);
int redis_set(redis_conn* conn, const char* key, const char* value, int value_len);
int redis_del(redis_conn* conn, const char* key);
int redis_incr(redis_conn* conn, const char* key, int64_t* new_value);
int redis_expire(redis_conn* conn, const char* key, int64_t seconds);

int redis_publish(redis_conn* conn, const char* channel, const char* message, int msg_len);
int redis_subscribe(redis_conn* conn, const char* channel);

bool redis_is_connected(redis_conn* conn);

#endif // VEK_REDIS_CLIENT_H
