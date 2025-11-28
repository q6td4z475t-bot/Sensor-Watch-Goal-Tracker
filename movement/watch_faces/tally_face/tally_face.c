// movement/watch_faces/tally_face/tally_face.c
// WATCH_FACE_GOAL_TRACKER
//
// Tally face for Sensor Watch Pro (upgraded LCD).
// See tally_face.h for function prototypes.

#include "tally_face.h"
#include "watch.h"
#include "watch_private_display.h"
#include "lis2dw.h"
#include "movement.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

/* ----------------- Backup SRAM layout (bytes) -----------------
   We store 16-bit Tally A and Goal A across two bytes each so A can be triple digits.
   Layout:
     0: TALLY_A_LO
     1: TALLY_A_HI
     2: TALLY_B_LO
     3: TALLY_B_HI (unused, kept 0)
     4: GOAL_A_LO
     5: GOAL_A_HI
     6: GOAL_B_LO
     7: GOAL_B_HI (unused)
----------------------------------------------------------------*/
#define BK_TALLY_A_LO 0
#define BK_TALLY_A_HI 1
#define BK_TALLY_B_LO 2
#define BK_TALLY_B_HI 3
#define BK_GOAL_A_LO  4
#define BK_GOAL_A_HI  5
#define BK_GOAL_B_LO  6
#define BK_GOAL_B_HI  7

/* Defaults and limits */
#define GOAL_A_DEFAULT 12
#define GOAL_B_DEFAULT 4
#define MIN_GOAL 1
#define MAX_GOAL_A 999
#define MAX_GOAL_B 99

/* Action timings (seconds) */
#define INC_HOLD_SECONDS   2
#define RESET_HOLD_SECONDS 5
#define GET_SHOW_SECONDS   3

/* Tap timing (ms) */
#define TRIPLE_TAP_WINDOW_MS 1500U
#define TAP_DEBOUNCE_MS      250U

/* Tap bits (from lis2dw.h). Re-declare if missing. */
#ifndef LIS2DW_TAP_SRC_SINGLE_TAP
#define LIS2DW_TAP_SRC_SINGLE_TAP (1 << 6)
#endif
#ifndef LIS2DW_TAP_SRC_DOUBLE_TAP
#define LIS2DW_TAP_SRC_DOUBLE_TAP (1 << 5)
#endif

/* Display indexes (typical for upgraded LCD builds) */
#define TOP_DISPLAY_INDEX 0
#define MAIN_DISPLAY_INDEX 1

/* ----------------- small helpers for backup u16 ----------------- */
static uint16_t backup_read_u16(uint8_t lo_idx, uint8_t hi_idx) {
    uint8_t lo = watch_get_backup_data(lo_idx);
    uint8_t hi = watch_get_backup_data(hi_idx);
    return (uint16_t)lo | ((uint16_t)hi << 8);
}
static void backup_write_u16(uint8_t lo_idx, uint8_t hi_idx, uint16_t v) {
    watch_store_backup_data(lo_idx, (uint8_t)(v & 0xFF));
    watch_store_backup_data(hi_idx, (uint8_t)((v >> 8) & 0xFF));
}

/* ----------------- date helper ----------------- */
static uint8_t days_in_month(uint16_t y, uint8_t m) {
    static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m != 2) return mdays[m-1];
    bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
    return leap ? 29 : 28;
}
static bool get_current_date(uint16_t *year, uint8_t *month, uint8_t *day) {
    struct tm now;
    if (movement_get_local_time(&now)) {
        *year = (uint16_t)(now.tm_year + 1900);
        *month = (uint8_t)(now.tm_mon + 1);
        *day = (uint8_t)now.tm_mday;
        return true;
    }
    return false;
}
static float compute_deficit(uint16_t goal, uint16_t actual) {
    uint16_t y; uint8_t m; uint8_t d;
    if (!get_current_date(&y,&m,&d)) return 0.0f;
    uint8_t dim = days_in_month(y,m);
    float expected = ((float)goal) * ((float)d / (float)dim);
    float deficit = expected - (float)actual;
    if (deficit < 0.0f) deficit = 0.0f;
    return deficit;
}

/* ----------------- state ----------------- */
typedef enum { MODE_NORMAL=0, MODE_SHOW_GET, MODE_SET_A, MODE_SET_B } mode_t;

typedef struct {
    uint16_t tally_a;
    uint16_t tally_b;
    uint16_t goal_a;
    uint16_t goal_b;

    uint8_t hold_sec_a;
    uint8_t hold_sec_b;
    bool action_done_a;
    bool action_done_b;

    /* tap tracking */
    uint32_t ms_clock;          // advanced by 1000ms on each second tick
    uint32_t last_tap_ms;
    uint8_t tap_count;
    uint32_t last_gesture_ms;

    /* mode & GET countdown */
    mode_t mode;
    uint8_t get_sec_remaining;

} state_t;

/* ----------------- forward prototypes ----------------- */
void tally_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void **context_ptr);
void tally_face_activate(movement_settings_t *settings, void *context);
bool tally_face_loop(movement_event_t event, movement_settings_t *settings, void *context);
void tally_face_resign(movement_settings_t *settings, void *context);

/* ----------------- helper renders ----------------- */
static void render_top_line(state_t *s, char *buf, size_t len) {
    // A: up to 3 digits, B: up to 2 digits
    snprintf(buf, len, "A:%03u B:%02u", (unsigned)s->tally_a, (unsigned)s->tally_b);
}

/* ----------------- tap action handlers ----------------- */
static void handle_single_tap(state_t *s) {
    float def_a = compute_deficit(s->goal_a, s->tally_a);
    if (def_a > 0.0001f) {
        s->mode = MODE_SHOW_GET;
        s->get_sec_remaining = GET_SHOW_SECONDS;
    }
}
static void handle_double_tap(state_t *s) {
    float def_b = compute_deficit(s->goal_b, s->tally_b);
    if (def_b > 0.0001f) {
        s->mode = MODE_SHOW_GET;
        s->get_sec_remaining = GET_SHOW_SECONDS;
    }
}
static void handle_triple_tap(state_t *s) {
    // Option 3: toggle between SET A and SET B
    if (s->mode == MODE_SET_A) s->mode = MODE_SET_B;
    else s->mode = MODE_SET_A;
}

/* ----------------- lifecycle ----------------- */
void tally_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void **context_ptr) {
    (void)settings; (void)watch_face_index;
    if (*context_ptr == NULL) {
        state_t *s = malloc(sizeof(state_t));
        if (!s) return;
        s->tally_a = backup_read_u16(BK_TALLY_A_LO, BK_TALLY_A_HI);
        s->tally_b = backup_read_u16(BK_TALLY_B_LO, BK_TALLY_B_HI);
        uint16_t sg_a = backup_read_u16(BK_GOAL_A_LO, BK_GOAL_A_HI);
        uint16_t sg_b = backup_read_u16(BK_GOAL_B_LO, BK_GOAL_B_HI);
        s->goal_a = (sg_a < MIN_GOAL || sg_a > MAX_GOAL_A) ? GOAL_A_DEFAULT : sg_a;
        s->goal_b = (sg_b < MIN_GOAL || sg_b > MAX_GOAL_B) ? GOAL_B_DEFAULT : sg_b;
        if (s->tally_a > MAX_GOAL_A) s->tally_a = MAX_GOAL_A;
        if (s->tally_b > MAX_GOAL_B) s->tally_b = MAX_GOAL_B;
        s->hold_sec_a = 0; s->hold_sec_b = 0;
        s->action_done_a = false; s->action_done_b = false;
        s->ms_clock = 0; s->last_tap_ms = 0; s->tap_count = 0; s->last_gesture_ms = 0;
        s->mode = MODE_NORMAL; s->get_sec_remaining = 0;
        *context_ptr = s;
    }
}

void tally_face_activate(movement_settings_t *settings, void *context) {
    (void)settings; (void)context;
    watch_clear_display();
    movement_request_tick_frequency(1); // 1 Hz
}

bool tally_face_loop(movement_event_t event, movement_settings_t *settings, void *context) {
    state_t *s = (state_t *)context;
    if (!s) return false;

    switch (event.event_type) {
        case EVENT_ACTIVATE:
            s->hold_sec_a = 0; s->hold_sec_b = 0;
            s->action_done_a = false; s->action_done_b = false;
            break;

        case EVENT_TICK:
            // advance ms clock once per second
            if (event.subsecond == 0) s->ms_clock += 1000;

            if (event.subsecond == 0) {
                /* BUTTON HOLD LOGIC (seconds resolution) */
                if (movement_is_button_pressed(BUTTON_LIGHT)) {
                    s->hold_sec_a++;
                    if (!s->action_done_a) {
                        if (s->hold_sec_a >= INC_HOLD_SECONDS && s->hold_sec_a < RESET_HOLD_SECONDS) {
                            // increment A
                            if (s->tally_a < MAX_GOAL_A) s->tally_a++;
                            backup_write_u16(BK_TALLY_A_LO, BK_TALLY_A_HI, s->tally_a);
                            s->action_done_a = true;
                        } else if (s->hold_sec_a >= RESET_HOLD_SECONDS) {
                            // reset A
                            s->tally_a = 0;
                            backup_write_u16(BK_TALLY_A_LO, BK_TALLY_A_HI, s->tally_a);
                            s->action_done_a = true;
                        }
                    }
                } else {
                    s->hold_sec_a = 0; s->action_done_a = false;
                }

                if (movement_is_button_pressed(BUTTON_ALARM)) {
                    s->hold_sec_b++;
                    if (!s->action_done_b) {
                        if (s->hold_sec_b >= INC_HOLD_SECONDS && s->hold_sec_b < RESET_HOLD_SECONDS) {
                            // increment B
                            if (s->tally_b < MAX_GOAL_B) s->tally_b++;
                            backup_write_u16(BK_TALLY_B_LO, BK_TALLY_B_HI, s->tally_b);
                            s->action_done_b = true;
                        } else if (s->hold_sec_b >= RESET_HOLD_SECONDS) {
                            // reset B
                            s->tally_b = 0;
                            backup_write_u16(BK_TALLY_B_LO, BK_TALLY_B_HI, s->tally_b);
                            s->action_done_b = true;
                        }
                    }
                } else {
                    s->hold_sec_b = 0; s->action_done_b = false;
                }

                /* ACCEL TAPS: read LIS2DW int source register */
                uint8_t int_src = lis2dw_get_int_source();
                uint32_t now = s->ms_clock;

                // immediate double-tap check
                if (int_src & LIS2DW_TAP_SRC_DOUBLE_TAP) {
                    if (now - s->last_gesture_ms > TAP_DEBOUNCE_MS) {
                        handle_double_tap(s);
                        s->last_gesture_ms = now;
                        s->tap_count = 0; s->last_tap_ms = 0;
                    }
                }

                // single tap reported
                if (int_src & LIS2DW_TAP_SRC_SINGLE_TAP) {
                    if (now - s->last_gesture_ms > TAP_DEBOUNCE_MS) {
                        if (s->tap_count == 0) {
                            s->tap_count = 1;
                            s->last_tap_ms = now;
                        } else {
                            if (now - s->last_tap_ms <= TRIPLE_TAP_WINDOW_MS) {
                                s->tap_count++;
                                s->last_tap_ms = now;
                            } else {
                                s->tap_count = 1;
                                s->last_tap_ms = now;
                            }
                        }
                        if (s->tap_count >= 3) {
                            if (now - s->last_gesture_ms > TAP_DEBOUNCE_MS) {
                                handle_triple_tap(s);
                                s->last_gesture_ms = now;
                            }
                            s->tap_count = 0; s->last_tap_ms = 0;
                        }
                    }
                }

                // if tap window expired -> confirm single tap
                if (s->tap_count > 0) {
                    if (now - s->last_tap_ms > TRIPLE_TAP_WINDOW_MS) {
                        if (now - s->last_gesture_ms > TAP_DEBOUNCE_MS) {
                            handle_single_tap(s);
                            s->last_gesture_ms = now;
                        }
                        s->tap_count = 0; s->last_tap_ms = 0;
                    }
                }

                /* GET countdown */
                if (s->mode == MODE_SHOW_GET) {
                    if (s->get_sec_remaining > 0) s->get_sec_remaining--;
                    if (s->get_sec_remaining == 0) s->mode = MODE_NORMAL;
                }
            } // end subsecond==0

            /* RENDER */
            if (s->mode == MODE_SHOW_GET) {
                float def_a = compute_deficit(s->goal_a, s->tally_a);
                float def_b = compute_deficit(s->goal_b, s->tally_b);
                if (def_a > 0.0001f) {
                    watch_display_string("GET A", TOP_DISPLAY_INDEX);
                    char mb[12];
                    snprintf(mb, sizeof(mb), "%5.2f", def_a);
                    watch_display_string(mb, MAIN_DISPLAY_INDEX);
                } else if (def_b > 0.0001f) {
                    watch_display_string("GET B", TOP_DISPLAY_INDEX);
                    char mb[12];
                    snprintf(mb, sizeof(mb), "%5.2f", def_b);
                    watch_display_string(mb, MAIN_DISPLAY_INDEX);
                } else {
                    char top[16];
                    render_top_line(s, top, sizeof(top));
                    watch_display_string(top, TOP_DISPLAY_INDEX);
                    watch_display_time(settings->bit.clock_24h);
                }
            } else if (s->mode == MODE_SET_A) {
                watch_display_string("SET A", TOP_DISPLAY_INDEX);
                char mb[12];
                snprintf(mb, sizeof(mb), "%3u", (unsigned)s->goal_a);
                watch_display_string(mb, MAIN_DISPLAY_INDEX);
            } else if (s->mode == MODE_SET_B) {
                watch_display_string("SET B", TOP_DISPLAY_INDEX);
                char mb[12];
                snprintf(mb, sizeof(mb), "%2u", (unsigned)s->goal_b);
                watch_display_string(mb, MAIN_DISPLAY_INDEX);
            } else {
                char top[16];
                render_top_line(s, top, sizeof(top));
                watch_display_string(top, TOP_DISPLAY_INDEX);
                watch_display_time(settings->bit.clock_24h);
            }

            break;

        case EVENT_LIGHT_BUTTON_UP:
            // In SET modes: LIGHT increments goal; else holds already handled increments/resets
            if (s->mode == MODE_SET_A) {
                if (s->goal_a < MAX_GOAL_A) s->goal_a++;
                if (s->goal_a < MIN_GOAL) s->goal_a = MIN_GOAL;
                if (s->goal_a > MAX_GOAL_A) s->goal_a = MAX_GOAL_A;
                backup_write_u16(BK_GOAL_A_LO, BK_GOAL_A_HI, s->goal_a);
            }
            break;

        case EVENT_ALARM_BUTTON_UP:
            if (s->mode == MODE_SET_A) {
                if (s->goal_a > MIN_GOAL) s->goal_a--;
                backup_write_u16(BK_GOAL_A_LO, BK_GOAL_A_HI, s->goal_a);
            } else if (s->mode == MODE_SET_B) {
                if (s->goal_b > MIN_GOAL) s->goal_b--;
                backup_write_u16(BK_GOAL_B_LO, BK_GOAL_B_HI, s->goal_b);
            }
            break;

        case EVENT_MODE_BUTTON_UP:
            if (s->mode == MODE_SET_A || s->mode == MODE_SET_B) {
                s->mode = MODE_NORMAL;
            } else {
                return false; // allow leaving face
            }
            break;

        default:
            break;
    }
    return true;
}

void tally_face_resign(movement_settings_t *settings, void *context) {
    (void)settings; (void)context;
    // nothing special to cleanup
}
