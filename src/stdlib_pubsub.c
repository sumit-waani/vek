#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <string.h>

/*
 * PubSub stdlib package - local in-process publish/subscribe system.
 *
 * Provides: pubsub.publish(channel, message)
 *           pubsub.subscribe(channel, callback)
 *           pubsub.unsubscribe(channel)
 */

#define PUBSUB_MAX_CHANNELS 256
#define PUBSUB_MAX_SUBSCRIBERS 64

typedef struct {
    char channel[256];
    Value callbacks[PUBSUB_MAX_SUBSCRIBERS];
    int count;
} PubSubChannel;

static PubSubChannel channels[PUBSUB_MAX_CHANNELS];
static int channel_count = 0;

static PubSubChannel* find_channel(const char* name) {
    for (int i = 0; i < channel_count; i++) {
        if (strcmp(channels[i].channel, name) == 0) {
            return &channels[i];
        }
    }
    return NULL;
}

static PubSubChannel* find_or_create_channel(const char* name) {
    PubSubChannel* ch = find_channel(name);
    if (ch) return ch;
    if (channel_count >= PUBSUB_MAX_CHANNELS) return NULL;

    ch = &channels[channel_count++];
    snprintf(ch->channel, sizeof(ch->channel), "%s", name);
    ch->count = 0;
    return ch;
}

static Value native_pubsub_subscribe(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (!IS_CLOSURE(args[1]) && !IS_NATIVE(args[1])) return VAL_NIL;

    ObjString* channel_name = AS_STRING(args[0]);
    Value callback = args[1];

    PubSubChannel* ch = find_or_create_channel(channel_name->data);
    if (!ch) return VAL_NIL;
    if (ch->count >= PUBSUB_MAX_SUBSCRIBERS) return VAL_NIL;

    ch->callbacks[ch->count++] = callback;

    // Pin the callback so GC does not collect it
    if (IS_PTR(callback)) {
        vm_pin((ObjHeader*)AS_PTR(callback));
    }

    return VAL_TRUE;
}

static Value native_pubsub_publish(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* channel_name = AS_STRING(args[0]);
    Value message = args[1];

    PubSubChannel* ch = find_channel(channel_name->data);
    if (!ch || ch->count == 0) return INT_VAL(0);

    int delivered = 0;
    for (int i = 0; i < ch->count; i++) {
        Value cb = ch->callbacks[i];
        vm_push(cb);
        vm_push(message);
        vm_call(cb, 1);
        delivered++;
    }

    return INT_VAL(delivered);
}

static Value native_pubsub_unsubscribe(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* channel_name = AS_STRING(args[0]);

    PubSubChannel* ch = find_channel(channel_name->data);
    if (!ch) return VAL_NIL;

    // Unpin all callbacks
    for (int i = 0; i < ch->count; i++) {
        if (IS_PTR(ch->callbacks[i])) {
            vm_unpin((ObjHeader*)AS_PTR(ch->callbacks[i]));
        }
        ch->callbacks[i] = VAL_NIL;
    }
    ch->count = 0;

    return VAL_TRUE;
}

void stdlib_pubsub_init(ObjMap* pkg) {
    memset(channels, 0, sizeof(channels));
    channel_count = 0;

    stdlib_register(pkg, "subscribe", native_pubsub_subscribe, 2);
    stdlib_register(pkg, "publish", native_pubsub_publish, 2);
    stdlib_register(pkg, "unsubscribe", native_pubsub_unsubscribe, 1);
}
