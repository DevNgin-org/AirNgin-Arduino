#pragma once
/* AirNgin time feed: pure payload parser (host tested). Payload: {"time","date","dayOfWeek","dayOfYear","weekNumber"}. */
#include <stddef.h>
#include "airngin.h"

typedef enum {
    AIRNGIN_TIME_PARSE_OK = 0,
    AIRNGIN_TIME_PARSE_BAD_ARG,
    AIRNGIN_TIME_PARSE_TOO_LONG,
    AIRNGIN_TIME_PARSE_NOT_JSON_OBJECT,
    AIRNGIN_TIME_PARSE_MISSING_FIELD,   /* a required field is absent or has an unusable type */
    AIRNGIN_TIME_PARSE_BAD_TIME,        /* "time" is not H:M[:S] */
    AIRNGIN_TIME_PARSE_BAD_DATE,        /* "date" is not Y/M/D */
    AIRNGIN_TIME_PARSE_BAD_NUMBER,      /* dayOfWeek/dayOfYear/weekNumber is not a non-negative integer */
    AIRNGIN_TIME_PARSE_OUT_OF_RANGE,
} airngin_time_parse_t;

#define AIRNGIN_TIME_PAYLOAD_MAX 512U

const char *airngin_time_parse_name(airngin_time_parse_t r);

/* Copy `in` to `out`, mapping Persian (U+06F0-06F9) and Arabic-Indic (U+0660-0669) digits to ASCII '0'-'9'.
 * Every other byte is copied unchanged. Returns the output length, or -1 if `cap` (incl. NUL) is too small. */
int airngin_time_normalize_digits(const char *in, size_t in_len, char *out, size_t cap);

/* Parse one packet. Numeric fields may be JSON numbers or strings (ASCII, Persian or Arabic-Indic digits).
 * Structural ranges only (the server's calendar conventions are not interpreted):
 *   hour 0-23, minute 0-59, second 0-59 (optional, default 0), year 1-9999, month 1-12, day 1-31,
 *   dayOfWeek 0-7, dayOfYear 1-366, weekNumber 0-54.
 * `received_at_ms` is left 0 for the caller. */
airngin_time_parse_t airngin_time_parse(const char *payload, size_t len, airngin_time_source_t source, airngin_time_info_t *out);
