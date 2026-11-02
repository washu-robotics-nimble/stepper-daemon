#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// INI configuration (D-R3, D-R43): [daemon] and [link] for the daemon
// itself, then one section per device, named by its oid. The device type is
// known only from the firmware's list_objects (F-R35), so a device section
// is checked against its type at the handshake; here, every key is only
// checked to be a known setting of some type, with a valid value.

#define CONFIG_OID_MAX     32   // oid length, including the NUL
#define CONFIG_MAX_DEVICES 16
#define CONFIG_MAX_VALUES  16   // keys per device section

typedef enum {
    CONFIG_INT,         // int32 in [min, max]
    CONFIG_POW2,        // int32 in [min, max], a power of 2
    CONFIG_BOOL,        // true / false, stored as 1 / 0
    CONFIG_POLARITY,    // H / L, stored as 'H' / 'L'
} config_kind_t;

// A device setting
typedef struct {
    const char *name;       // INI key; for firmware settings, also the
                            // set_motion field
    const char *type;       // object type it belongs to (F-R35)
    bool firmware;          // sent to the firmware; false: daemon only
    config_kind_t kind;
    int32_t min, max;
} config_key_t;

typedef struct {
    const config_key_t *key;
    int32_t value;
} config_value_t;

typedef struct {
    char oid[CONFIG_OID_MAX];
    int line;               // of its first key or its header, for messages
    size_t count;
    config_value_t values[CONFIG_MAX_VALUES];
} config_device_t;

typedef struct {
    // [daemon]
    char device[256];       // the MCU's serial device
    char socket[108];       // client socket (D-R12); sun_path is 108 bytes
    char group[64];
    // [link]
    int32_t report_period;      // F-R32, ms; 0 = off
    int32_t timeout_ms;         // F-R22, the firmware's; 0 = off
    int32_t reply_timeout_ms;   // D-R30, the daemon's
    // device sections
    size_t device_count;
    config_device_t devices[CONFIG_MAX_DEVICES];
} config_t;

// Load and check a config file. Every problem is logged with its line.
// Returns 0 on success, -1 on any error.
int config_load(const char *path, config_t *cfg);

// The same, from a string; name is used in messages
int config_parse_string(const char *text, const char *name, config_t *cfg);

// The section for oid, or NULL
const config_device_t *config_find_device(const config_t *cfg, const char *oid);

// The value of key in dev, or NULL if the section doesn't set it
const config_value_t *config_get(const config_device_t *dev, const char *key);

#endif
