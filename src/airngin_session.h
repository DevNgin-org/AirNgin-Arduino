#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Provisioning session bookkeeping (pure logic, no ESP-IDF dependency so it is
 * unit-testable on a host). The caller serializes access (pv_lock).
 *
 *  open     a provisioning panel is currently serving requests
 *  claimed  a configuration was saved successfully; no further /set accepted
 *  saving   exactly one request is inside the parse-validate-save section
 *
 * The claim is held ONLY around the save, never across the (slow, client
 * controlled) HTTP body receive, so a slow client cannot block other requests.
 */
typedef struct {
    int64_t deadline_us;
    bool open;
    bool claimed;
    bool saving;
} airngin_session_t;

typedef enum {
    AIRNGIN_SESSION_ADMIT = 0,
    AIRNGIN_SESSION_CLAIMED,
    AIRNGIN_SESSION_UNAVAILABLE,   /* closed or past its deadline */
} airngin_session_verdict_t;

static inline void airngin_session_open(airngin_session_t *s, int64_t now_us, int64_t ttl_us)
{
    s->open = true;
    s->claimed = false;
    s->saving = false;
    s->deadline_us = now_us + ttl_us;
}

static inline void airngin_session_close(airngin_session_t *s)
{
    s->open = false;
}

static inline airngin_session_verdict_t airngin_session_check(const airngin_session_t *s, int64_t now_us)
{
    if (s->claimed) return AIRNGIN_SESSION_CLAIMED;
    if (!s->open || now_us >= s->deadline_us) return AIRNGIN_SESSION_UNAVAILABLE;
    return AIRNGIN_SESSION_ADMIT;
}

/* Enter the save section. Fails if claimed, expired, closed or already saving. */
static inline airngin_session_verdict_t airngin_session_begin_save(airngin_session_t *s, int64_t now_us)
{
    const airngin_session_verdict_t v = airngin_session_check(s, now_us);
    if (v != AIRNGIN_SESSION_ADMIT) return v;
    if (s->saving) return AIRNGIN_SESSION_UNAVAILABLE;
    s->saving = true;
    return AIRNGIN_SESSION_ADMIT;
}

static inline void airngin_session_end_save(airngin_session_t *s, bool success)
{
    s->saving = false;
    if (success) s->claimed = true;
}

/* True when the owner may close the panel because the session timed out. */
static inline bool airngin_session_expired(const airngin_session_t *s, int64_t now_us)
{
    return s->open && !s->saving && !s->claimed && now_us >= s->deadline_us;
}
