#ifndef HANDSHAKE_H
#define HANDSHAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "link.h"
#include "model.h"

// The handshake after every connect (D-R3, F-R33, F-R35): identify,
// list_objects, set_link, set_motion for each stepper with an INI section,
// settings_done, then get_state for each stepper, to fill the machine
// model. One request at a time, each sent from the previous one's reply, so
// the main loop keeps running.

#define HANDSHAKE_PROTO       1     // the host-link protocol this daemon speaks
#define HANDSHAKE_BUSY_RETRY_MS 50  // set_motion on a moving axis: retry...
#define HANDSHAKE_BUSY_MAX_MS 2000  // ...for this long

typedef enum {
    HANDSHAKE_RUNNING,
    HANDSHAKE_READY,
    HANDSHAKE_FAILED,   // a setting rejected, an unsupported protocol, or an
                        // INI section that doesn't fit its object (error);
                        // the caller retries with backoff (D-R31). A link
                        // drop is not this.
} handshake_status_t;

typedef struct {
    link_t *link;
    const config_t *cfg;
    model_t *model;             // filled from list_objects and get_state
    handshake_status_t status;
    bool quiet;                 // a retry: progress at debug level (D-R31)
    char error[256];            // HANDSHAKE_FAILED: why
    size_t next;                // device getting its settings or get_state
    uint64_t retry_at;          // set_motion again (busy); 0: no retry due
    uint64_t busy_since;        // first busy reply for this device; 0: none
    char fw[32];                // from identify
} handshake_t;

// Start on a new link; the model is reset. quiet: a retry, so progress is
// logged at debug level only (D-R31).
void handshake_start(handshake_t *h, link_t *link, const config_t *cfg, model_t *model,
                     bool quiet);

// For poll(): ms until handshake_tick() has something to do; -1: nothing
int handshake_poll_timeout(const handshake_t *h);

// Retries due
void handshake_tick(handshake_t *h);

// The set_motion fields for a stepper's INI section. Soft limits the
// section leaves out are sent as null, i.e. no limit (F-R25); other keys
// left out aren't sent, so the firmware keeps its value (F-R34). The
// handshake then replaces the fields clients changed at runtime (D-R3).
void handshake_motion_fields(const config_device_t *dev, cJSON *req);

#endif
