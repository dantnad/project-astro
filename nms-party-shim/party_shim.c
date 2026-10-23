// Shim for No Man's Sky 4.70 (macOS): logs PlayFab Party voice-chat setup calls and works around the
// uninitialized chat-control handle that makes multiplayer fail (see my_PartyCreateLocalUser).
// Calls are otherwise passed straight through to the real PlayFabPartyMac. NMS_SHIM_NOFIX=1 disables the fix.
// Build with build.sh, run the game with launch.sh (Steam launch options can't set env vars on macOS).
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

typedef uint32_t PartyError;
typedef const char *PartyString;
typedef void *H;

PartyError PartyGetErrorMessage(PartyError error, PartyString *message);
PartyError PartyInitialize(PartyString titleId, H *handle);
PartyError PartyGetLocalDevice(H handle, H *device);
PartyError PartyCreateLocalUser(H handle, PartyString entityId, PartyString token, H *user);
PartyError PartyDeviceCreateChatControl(H device, H user, PartyString lang, void *ctx, H *chat);
PartyError PartyNetworkConnectChatControl(H network, H chat, void *ctx);
PartyError PartyChatControlSetTextToSpeechProfile(H chat, uint32_t type, PartyString profile, void *ctx);
PartyError PartyStartProcessingStateChanges(H handle, uint32_t *count, const uint32_t *const **changes);

static FILE *logf_;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static void logmsg(const char *fmt, ...) {
    pthread_mutex_lock(&mu);
    if (!logf_) {
        char path[1024];
        snprintf(path, sizeof path, "%s/Library/Logs/nms-party-shim.log", getenv("HOME"));
        logf_ = fopen(path, "a");
        if (logf_) fprintf(logf_, "\n===== launch pid %d =====\n", getpid());
    }
    if (logf_) {
        struct timeval tv; gettimeofday(&tv, NULL);
        struct tm tm; localtime_r(&tv.tv_sec, &tm);
        fprintf(logf_, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
        va_list ap; va_start(ap, fmt); vfprintf(logf_, fmt, ap); va_end(ap);
        fputc('\n', logf_); fflush(logf_);
    }
    pthread_mutex_unlock(&mu);
}

static const char *errmsg(PartyError e) {
    PartyString m = NULL;
    if (e == 0) return "OK";
    if (PartyGetErrorMessage(e, &m) != 0 || !m) return "?";
    return m;
}

static PartyError my_PartyInitialize(PartyString titleId, H *handle) {
    PartyError r = PartyInitialize(titleId, handle);
    logmsg("PartyInitialize(%s) -> %u %s handle=%p", titleId ? titleId : "", r, errmsg(r), handle ? *handle : NULL);
    return r;
}

static PartyError my_PartyGetLocalDevice(H handle, H *device) {
    PartyError r = PartyGetLocalDevice(handle, device);
    logmsg("PartyGetLocalDevice(%p) -> %u %s device=%p", handle, r, errmsg(r), device ? *device : NULL);
    return r;
}

static int chat_controls_created;

// The fix. cGcMatchmakingPlayfab's constructor never initializes its chat-control handle (this+0x2c8);
// the object comes from a non-zeroing heap allocator. OnPlayfabTokenRetrieved() then sees leftover garbage,
// assumes a chat control already exists, skips creating one, and every later
// PartyNetworkConnectChatControl fails (error 6) -> the game leaves every lobby.
// The game passes &this->localUser (this+0x2c0) here, so the handle is the next slot. We clear it, but only
// when called from that exact site in the 4.70 build and before any real chat control exists.
#define NMS_470_CREATE_LOCAL_USER_RET_OFFSET 0x9c7c4ULL  // 0x10009c7c4 minus the 0x100000000 image base

static int called_from_token_retrieved(void *ret) {
    Dl_info info;
    if (!dladdr(ret, &info) || !info.dli_fname || !strstr(info.dli_fname, "/MacOS/No Man's Sky")) return 0;
    return (uintptr_t)ret - (uintptr_t)info.dli_fbase == NMS_470_CREATE_LOCAL_USER_RET_OFFSET;
}

static PartyError my_PartyCreateLocalUser(H handle, PartyString entityId, PartyString token, H *user) {
    void *ret = __builtin_return_address(0);
    PartyError r = PartyCreateLocalUser(handle, entityId, token, user);
    logmsg("PartyCreateLocalUser(%p) -> %u %s user=%p", handle, r, errmsg(r), user ? *user : NULL);
    if (r == 0 && user && !chat_controls_created && !getenv("NMS_SHIM_NOFIX")) {
        if (called_from_token_retrieved(ret)) {
            H *chat_slot = user + 1;
            logmsg("fix: chat-control handle before = %p%s", *chat_slot, *chat_slot ? "  <-- uninitialized garbage" : "");
            *chat_slot = NULL;
        } else {
            logmsg("fix: skipped, unexpected call site %p (different game build?)", ret);
        }
    }
    return r;
}

static PartyError my_PartyDeviceCreateChatControl(H device, H user, PartyString lang, void *ctx, H *chat) {
    PartyError r = PartyDeviceCreateChatControl(device, user, lang, ctx, chat);
    if (r == 0) chat_controls_created++;
    logmsg("PartyDeviceCreateChatControl(device=%p user=%p lang=%s) -> %u %s chat=%p",
           device, user, lang ? lang : "(null)", r, errmsg(r), chat ? *chat : NULL);
    return r;
}

static PartyError my_PartyNetworkConnectChatControl(H network, H chat, void *ctx) {
    PartyError r = PartyNetworkConnectChatControl(network, chat, ctx);
    logmsg("PartyNetworkConnectChatControl(network=%p chat=%p) -> %u %s", network, chat, r, errmsg(r));
    return r;
}

static PartyError my_PartyChatControlSetTextToSpeechProfile(H chat, uint32_t type, PartyString profile, void *ctx) {
    PartyError r = PartyChatControlSetTextToSpeechProfile(chat, type, profile, ctx);
    logmsg("PartyChatControlSetTextToSpeechProfile(chat=%p) -> %u %s", chat, r, errmsg(r));
    return r;
}

// Async results arrive as state changes; most "...Completed" ones start with {type, result, errorDetail}.
static PartyError my_PartyStartProcessingStateChanges(H handle, uint32_t *count, const uint32_t *const **changes) {
    PartyError r = PartyStartProcessingStateChanges(handle, count, changes);
    if (r == 0 && count && changes && *changes) {
        for (uint32_t i = 0; i < *count; i++) {
            const uint32_t *c = (*changes)[i];
            if (c && c[1] != 0) // only non-success results
                logmsg("StateChange type=%u result=%u errorDetail=%u %s", c[0], c[1], c[2], errmsg(c[2]));
        }
    }
    return r;
}

#define INTERPOSE(f) \
    __attribute__((used)) static struct { const void *replacement, *replacee; } _interpose_##f \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)my_##f, (const void *)f }

INTERPOSE(PartyInitialize);
INTERPOSE(PartyGetLocalDevice);
INTERPOSE(PartyCreateLocalUser);
INTERPOSE(PartyDeviceCreateChatControl);
INTERPOSE(PartyNetworkConnectChatControl);
INTERPOSE(PartyChatControlSetTextToSpeechProfile);
INTERPOSE(PartyStartProcessingStateChanges);

__attribute__((constructor)) static void init(void) { logmsg("shim loaded"); }
