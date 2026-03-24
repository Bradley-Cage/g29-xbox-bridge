/**
 * gip_metadata.c — Compiled GIP Metadata Blob
 *
 * Generated from wheel_metadata.json by tools/compile_metadata.py
 * To regenerate:
 *   python3 tools/compile_metadata.py firmware-xbox/data/wheel_metadata.json --c-header \
 *     > firmware-xbox/include/gip_wheel_metadata.h
 */

#include "gip_wheel_metadata.h"

// Re-export for external access
const uint8_t *gip_get_metadata_blob(void) {
    return gip_wheel_metadata;
}

uint16_t gip_get_metadata_len(void) {
    return gip_wheel_metadata_len;
}
