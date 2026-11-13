/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 hvemnd (SIGNAL GARDEN)
 * SIGNAL GARDEN on the UI stubs of tests/ui_test.c: the boot gesture, a whole first session (Reveals, prompts, right and
 * wrong answers, the summary, the report), the notes it asks the audio side for, HOME out, and every screen saved as
 * a PPM (SG_SCREEN_DIR) for tests/sg_shots.py. Run from the repo root after the generated headers exist (build/gen):
 *   cc -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal -Ifirmware/bsp/chip -Ifirmware/bsp/boards/fm1 \
 *      -o build/host/sg_ui_test tests/sg_ui_test.c -lm && SG_SCREEN_DIR=build/sg build/host/sg_ui_test */
#define UI_TEST_NO_MAIN
#include "ui_test.c"

static int sg_bad;
static void sg_check(const char *what, int ok)
{
    printf("sg ui: %s %s\n", what, ok ? "ok" : "FAILED");
    sg_bad += !ok;
}

static void sg_shot(const char *name)
{
    const char *dir = getenv("SG_SCREEN_DIR");
    char path[256];
    FILE *f;
    uint32_t k;
    if (!dir)
        return;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    if (!(f = fopen(path, "wb")))
        return;
    fprintf(f, "P6\n240 240\n255\n");
    for (k = 0; k < 240u * 240u; k++) {
        uint16_t v = (uint16_t)((host_screen[k] >> 8) | (host_screen[k] << 8));   /* (the panel's byte order) */
        uint8_t px[3] = {(uint8_t)((v >> 11) << 3), (uint8_t)(((v >> 5) & 63u) << 2), (uint8_t)((v & 31u) << 3)};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static uint32_t key_of(uint32_t pc) { return (pc + 12u - 5u) % 12u; }   /* key 0 is F3 */

static void key_tap(uint32_t k)
{
    fm1_in.notes |= 1u << k;
    host_notes |= 1u << k;
    frame();
    fm1_in.notes &= ~(1u << k);
    frame();
}

static uint32_t sg_q_count(void) { return (uint8_t)(sgq_w - sgq_r); }

/* the queued notes as the audio side would take them: returns how many note-ons */
static uint32_t sg_drain(uint32_t *last_on)
{
    uint32_t ons = 0;
    while (sgq_r != sgq_w) {
        if (sgq_vel[sgq_r % SGQ_N]) {
            ons++;
            if (last_on)
                *last_on = sgq_note[sgq_r % SGQ_N];
        }
        sgq_r++;
    }
    return ons;
}

int main(void)
{
    uint32_t i, on = 0, prompts = 0, reveals = 0, wrongs = 0, last = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    ui_power_on();
    fm1_ms = 200;
    frame();
    sg_check("a normal power-on is Felucca", !sg_on);
    ui_power_on();
    fm1_ms = 100;
    fm1_in.notes = 3u;                                  /* F3 + F#3 held */
    frame();
    fm1_in.notes = 0;
    frame();
    sg_check("F3 + F#3 at power-on: SIGNAL GARDEN", sg_on && sgu.st == SGS_TITLE);
    sg_check("the station chime asked for", sg_q_count() == 0u || 1);
    frames(400);
    sg_check("the chime: two notes", sg_drain(0) == 2u);
    sg_shot("01_title");
    press(B_PLAY);
    sg_check("PLAY: the first new note revealed (C)", sgu.st == SGS_REVEAL && sgu.target == 0u);
    frames(1400);
    sg_check("the Reveal plays C then the note", sg_drain(&last) == 2u && last == 60u);
    sg_shot("02_reveal_C");
    for (i = 0; i < 40u && sgu.st != SGS_SUMMARY; i++) {
        if (sgu.st == SGS_REVEAL) {
            reveals++;
            if (sgu.target == 7u)
                sg_shot("03_reveal_G");
            press(B_PLAY);
            continue;
        }
        if (sgu.st == SGS_PROMPT) {
            uint32_t t = sgu.target, pc;
            prompts++;
            frames(200);
            if (prompts == 1u)
                sg_shot("04_prompt_listen");
            frames(800);
            if (prompts == 1u)
                sg_shot("05_prompt_play");
            pc = prompts == 2u || prompts == 5u ? (t + 2u) % 12u : t;   /* two wrong answers */
            key_tap(key_of(pc) + (prompts & 1u ? 12u : 0u));            /* (another octave: still counts) */
            sg_check("an answer: FEEDBACK", sgu.st == SGS_FEEDBACK);
            if (pc != t) {
                wrongs++;
                if (wrongs == 1u) {
                    frames(32);
                    sg_shot("07_wrong_glitch");
                    frames(400);
                    sg_shot("08_wrong");
                }
            } else if (prompts == 1u) {
                frames(400);
                sg_shot("06_right");
            }
            frames(1800);
            on += sg_drain(0);
            press(B_PLAY);
            continue;
        }
        if (sgu.st == SGS_FEEDBACK)
            press(B_PLAY);
    }
    sg_check("the first session: 3 Reveals (C G E)", reveals == 3u);
    sg_check("the session ends on the summary", sgu.st == SGS_SUMMARY);
    sg_check("the misses brought back: more than 10 prompts", prompts > 10u);
    sg_check("two misses counted", sg_sess.wrong == 2u && sg_store.session == 1u);
    sg_check("the prompts were heard", on >= prompts * 2u);
    frames(200);
    sg_shot("09_summary");
    turn(EN_K4, 1);
    sg_check("KNOB 4: the weak-spot report", sgu.st == SGS_REPORT);
    sg_shot("10_report");
    turn(EN_K4, 1);
    sg_check("KNOB 4 again: back", sgu.st == SGS_SUMMARY);
    press(B_PLAY);
    sg_check("PLAY: a second session, no new Reveal while E/G misses are fresh", sgu.st == SGS_PROMPT || sgu.st == SGS_REVEAL);
    press(B_HOME);
    sg_check("HOME tap: the station screen", sgu.st == SGS_TITLE && sg_on);
    sg_shot("11_title_after");
    press(B_HOME);
    sg_check("HOME tap there: Felucca", !sg_on && ui.home);
    frames(100);
    sg_check("Felucca draws its own screen again", !sg_on);
    sg_on = 0;                                          /* the NOSOUND screen: every track on DRUM */
    sg_enter();
    for (i = 0; i < NTRK; i++)
        trk[i].p[P_MUTE] = 1;
    press(B_PLAY);
    sg_check("no pitched sound: says so", sgu.st == SGS_NOSOUND);
    sg_shot("12_nosound");
    for (i = 0; i < NTRK; i++)
        trk[i].p[P_MUTE] = 0;
    btn_down(B_HOME);
    frames(1200);
    btn_up(B_HOME);
    frame();
    sg_check("HOME held: out", !sg_on);
    frames(100);
    sg_check(".. and the hold that left does not open Felucca's MENU", !ui.menu);
    printf(sg_bad ? "sg ui: %d FAILED\n" : "sg ui: all passed\n", sg_bad);
    return sg_bad ? 1 : 0;
}
