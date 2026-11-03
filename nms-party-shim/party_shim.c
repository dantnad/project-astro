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
// when the call really comes from OnPlayfabTokenRetrieved() and before any real chat control exists.
//
// That function holds the game's only call to PartyCreateLocalUser, and the game exports its mangled C++
// symbol, so we resolve it at runtime rather than hardcoding a file offset. A hardcoded offset stops
// matching the moment Steam patches the binary, which is what happened on 2026-11-02 (build 26386568,
// still version 4.70): the call site moved from 0x9c7c4 to 0x9cf88 and the fix silently did nothing.
#define NMS_TOKEN_RETRIEVED_SYM "_ZN21cGcMatchmakingPlayfab23OnPlayfabTokenRetrievedEv"
#define NMS_TOKEN_RETRIEVED_MAX_SIZE 0x1000ULL  // actual size is ~0x2b4; generous room for future patches

static void *token_retrieved_fn(void) {
    static void *fn;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        fn = dlsym(RTLD_DEFAULT, NMS_TOKEN_RETRIEVED_SYM);
        if (!fn) logmsg("fix: symbol %s not found (different game build?)", NMS_TOKEN_RETRIEVED_SYM);
    }
    return fn;
}

// True when ret is a return address just past a BL inside OnPlayfabTokenRetrieved().
static int called_from_token_retrieved(void *ret) {
    void *fn = token_retrieved_fn();
    if (!fn) return 0;
    uintptr_t off = (uintptr_t)ret - (uintptr_t)fn;
    if (off < 4 || off >= NMS_TOKEN_RETRIEVED_MAX_SIZE) {
        logmsg("fix: return address %p is +%#lx from OnPlayfabTokenRetrieved() at %p, outside it",
               ret, (unsigned long)off, fn);
        return 0;
    }
    uint32_t prev;
    memcpy(&prev, (const char *)ret - 4, sizeof prev);
    if ((prev >> 26) != 0x25) {  // ARM64 BL
        logmsg("fix: instruction before %p is not a BL (0x%08x)", ret, prev);
        return 0;
    }
    return 1;
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
