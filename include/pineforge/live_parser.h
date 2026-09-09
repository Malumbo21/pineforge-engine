/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PINEFORGE_LIVE_PARSER_H
#define PINEFORGE_LIVE_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define PF_LIVE_PARSER_ABI_VERSION 1u
#define PF_LIVE_PARSER_MAX_MESSAGE_BYTES (1024u * 1024u)
#define PF_LIVE_PARSER_MAX_EVENTS 1024u
#define PF_LIVE_PARSER_MAX_OUTPUT_BYTES (1024u * 1024u)

#if defined(_WIN32) || defined(__CYGWIN__)
#define PF_LIVE_PARSER_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define PF_LIVE_PARSER_API __attribute__((visibility("default")))
#else
#define PF_LIVE_PARSER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum pf_live_parser_event_kind {
    PF_LIVE_PARSER_TICK = 1,
    PF_LIVE_PARSER_BAR = 2,
    PF_LIVE_PARSER_TIME = 3
};

/* ABI v1 is fixed-layout POD; a changed layout requires a new ABI version.
 * Initialize the whole struct to zero before assigning its active fields.
 * All timestamps are Unix milliseconds and must be nonnegative.
 *
 * TICK: timestamp, strictly positive source sequence, price and quantity.
 * BAR: timestamp is a confirmed one-minute bar's open, aligned to 60000ms;
 *      open/high/low/close are positive finite prices; volume is finite >= 0.
 * TIME: timestamp is the provider's explicit completeness boundary, declaring
 *       that no earlier ticks remain. A wall clock/heartbeat is not sufficient.
 *
 * reserved must be zero. Inactive fields are ignored and normalized to zero by
 * the host. Sequence continuity, mode and stream ordering are checked by the
 * runner across messages; parsers must preserve source sequence, never invent
 * a mutable counter. BAR/TIME do not use sequence.
 */
typedef struct pf_live_parser_event_v1 {
    uint32_t kind;
    uint32_t reserved;
    int64_t timestamp;
    uint64_t sequence;
    double open;
    double high;
    double low;
    double close;
    double volume;
    double price;
    double quantity;
} pf_live_parser_event_v1_t;

/* The event pointer is borrowed only until this callback returns. The callback
 * copies it synchronously and returns 0 on acceptance or -1 on failure. A plugin
 * MUST stop and propagate a callback failure; it must not retain callback/user
 * pointers, call concurrently, or emit after parse_message returns.
 */
typedef int (*pf_live_parser_emit_v1_fn)(
    const pf_live_parser_event_v1_t* event, void* user);

PF_LIVE_PARSER_API uint32_t pf_live_parser_abi_version(void);

/* Parse ONE complete provider message into zero or more ordered events.
 * message/config_json are length-delimited borrowed bytes, not NUL-terminated
 * strings. config_json is an immutable JSON object; it remains in host memory
 * and must not be logged or copied into normalized events. Heartbeats/control
 * messages may succeed with zero events. Malformed input MUST fail.
 *
 * Return 0 on success, -1 on any failure. Never throw across this C ABI. The
 * host stages events until success, so a failed message emits no partial output
 * to the strategy or journal. Limits: 1MiB message, 1024 events, 1MiB POD output.
 * With the v1 event size the event-count bound is the tighter output bound.
 *
 * REQUIRED: stateless and deterministic for identical (message, config) bytes.
 * No I/O, clock, random values, mutable globals or retained state may influence
 * output. The journal stores canonical normalized events, not parser state.
 * Providers requiring cross-message assembly/state need an upstream adapter.
 * Plugins are trusted native executable code, not sandboxed. The host cannot
 * enforce this contract against malicious code or recover from native crashes.
 */
PF_LIVE_PARSER_API int pf_live_parse_message(
    const char* message, size_t message_size,
    const char* config_json, size_t config_size,
    pf_live_parser_emit_v1_fn emit, void* user);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* PINEFORGE_LIVE_PARSER_H */
