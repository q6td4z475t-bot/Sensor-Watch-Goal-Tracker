#ifndef TALLY_FACE_H
#define TALLY_FACE_H

#include "movement.h"
#include "movement_config.h"
#include "watch.h"
#include <stdbool.h>
#include <stdint.h>

/* Movement v2 watch face API */
void tally_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void **context_ptr);
void tally_face_activate(movement_settings_t *settings, void *context);
bool tally_face_loop(movement_event_t event, movement_settings_t *settings, void *context);
void tally_face_resign(movement_settings_t *settings, void *context);

#endif // TALLY_FACE_H
