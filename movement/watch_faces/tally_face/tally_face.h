#ifndef TALLY_FACE_H_
#define TALLY_FACE_H_

#include "movement.h"

/* Movement v2 watch face API */
void tally_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void **context_ptr);
void tally_face_activate(movement_settings_t *settings, void *context);
bool tally_face_loop(movement_event_t event, movement_settings_t *settings, void *context);
void tally_face_resign(movement_settings_t *settings, void *context);

/* Watch face descriptor used in movement_config.h */
#define goal_tracker_face ((const watch_face_t){ \
    tally_face_setup, \
    tally_face_activate, \
    tally_face_loop, \
    tally_face_resign, \
    NULL, \
})

#endif // TALLY_FACE_H_
