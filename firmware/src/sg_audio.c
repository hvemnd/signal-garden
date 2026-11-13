/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 hvemnd (SIGNAL GARDEN)
 * SIGNAL GARDEN's notes: the UI (sg_ui.c) asks, the audio side plays. A ring of note events, written by the main loop
 * only and read by the sequencer's block (seq.c, beside STEP PREVIEW's aud_req) only, so no lock is needed. The
 * prompts play through the selected track's sound, as the keys do. Included at the top of seq.c, so every unity build
 * (felucca.c, tests/hostsim.c) has it. */
#include "sg_core.c"

#define SGQ_N 16u
static volatile uint8_t sgq_note[SGQ_N], sgq_vel[SGQ_N];   /* vel 0: note off */
static volatile uint8_t sgq_w, sgq_r;
static uint8_t sg_on;                                     /* SIGNAL GARDEN has the screen, the buttons and the knobs */

static int sg_audio_push(uint32_t note, uint32_t vel)
{
    uint8_t w = sgq_w;
    if ((uint8_t)(w - sgq_r) >= SGQ_N)
        return 0;                                         /* full: dropped (the UI never has more than 8 waiting) */
    sgq_note[w % SGQ_N] = (uint8_t)(note & 127u);
    sgq_vel[w % SGQ_N] = (uint8_t)(vel & 127u);
    sgq_w = (uint8_t)(w + 1u);
    return 1;
}

static __attribute__((noinline)) void sg_audio_poll(void)
{
    while (sgq_r != sgq_w) {
        uint8_t r = sgq_r;
        track_t *t = &trk[song.sel % NTRK];
        if (sgq_vel[r % SGQ_N])
            trk_note_on(t, sgq_note[r % SGQ_N], sgq_vel[r % SGQ_N]);
        else
            trk_note_off(t, sgq_note[r % SGQ_N]);
        sgq_r = (uint8_t)(r + 1u);
    }
}
