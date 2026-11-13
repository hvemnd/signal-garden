/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 hvemnd (SIGNAL GARDEN)
 * SIGNAL GARDEN: an ear trainer on Felucca. Station 88.1, NOTE ID (sg_core.c has the learning engine).
 *
 * Getting in: hold F3 and F#3 (the two lowest keys) while switching the FM-1 on, until the station screen shows.
 * Getting out: hold HOME (one second), or tap HOME on the station screen: Felucca as it always is. Nothing of
 * Felucca's is changed by the mode: its sounds, projects and settings stay as they were. A normal power-on is Felucca.
 *
 * Controls (the screen does the feedback; the keys' LEDs show the reference C and, after an answer, the right note):
 *   keys      answer (any octave: the pitch class counts); in a Reveal, play freely
 *   PLAY      start, next, and hear the prompt again while it waits for an answer
 *   KNOB 1    hear the prompt (or the Reveal) again
 *   KNOB 4    on the summary: the weak-spot report (per note), and back
 *   OCT- OCT+ move the keys' range (the answer does not depend on it)
 *   HOME      tap: back to the station screen (there: out); hold: out
 *
 * The prompts play through the selected track's sound. A sound with its own key map (DRUM, SLICE) has no pitch to
 * learn from: the mode picks the first track with a pitched sound, or says it needs one.
 *
 * Not yet: progress is kept in RAM only, so it lasts until power-off (a flash record needs a sector pair that is
 * verified free; see docs/SIGNAL_GARDEN.md). */

enum { SGS_TITLE, SGS_REVEAL, SGS_PROMPT, SGS_FEEDBACK, SGS_SUMMARY, SGS_REPORT, SGS_NOSOUND };

/* the Hidden Net palette: four colours a screen, never more (ink and paper on every screen, then two of the three
 * signal colours: the station screen and the summary green and pink, a prompt cyan, a Reveal green, an answer green
 * or pink) */
#define SG_INK RGB(0x0b, 0x0b, 0x10)
#define SG_PAPER RGB(0xf2, 0xed, 0xe4)
#define SG_PINK RGB(0xff, 0x2e, 0x88)
#define SG_CYAN RGB(0x00, 0xe5, 0xff)
#define SG_GREEN RGB(0x39, 0xff, 0x14)
#define SG_SCAN RGB(0x16, 0x16, 0x1e)                   /* the scanlines: ink lifted, not a fifth colour */

#define SG_REF 60u                                      /* the reference C (C4) */
#define SG_VEL 100u
#define SG_NEV 8u

static sg_store_t sg_store;
static sg_session_t sg_sess;
static struct {
    uint8_t st, dirty, fx, fxn;                         /* fx: 1 glitch (wrong), 2 chromatic split (right); fxn frames */
    uint8_t cue;                                        /* PROMPT: 1 once the target has started (PLAY THE NOTE) */
    uint8_t target, answered, grade, intro;
    uint8_t ev_note[SG_NEV], ev_vel[SG_NEV], ev_n;      /* the notes to come (ev_t ascending) */
    uint32_t ev_t[SG_NEV];
    uint8_t on[4], non;                                 /* sounding now */
    uint32_t t_target;                                  /* fm1_ms the prompt's target started (0: not yet) */
    uint32_t home_t0;                                   /* fm1_ms | 1 HOME went down, 0 up */
    uint32_t frame;
    char why1[40], why2[40], trap[40];
} sgu;

/* ------------------------------------------------------------ notes --- */
static void sg_silence(void)
{
    uint32_t i;
    for (i = 0; i < sgu.non; i++)
        (void)sg_audio_push(sgu.on[i], 0);
    sgu.non = 0;
    sgu.ev_n = 0;
}

static void sg_at(uint32_t t, uint32_t note, uint32_t vel)
{
    uint32_t i;
    if (sgu.ev_n >= SG_NEV)
        return;
    for (i = sgu.ev_n; i > 0 && (int32_t)(sgu.ev_t[i - 1u] - t) > 0; i--) {   /* kept in time order */
        sgu.ev_t[i] = sgu.ev_t[i - 1u];
        sgu.ev_note[i] = sgu.ev_note[i - 1u];
        sgu.ev_vel[i] = sgu.ev_vel[i - 1u];
    }
    sgu.ev_t[i] = t;
    sgu.ev_note[i] = (uint8_t)note;
    sgu.ev_vel[i] = (uint8_t)vel;
    sgu.ev_n++;
}

static void sg_note(uint32_t t, uint32_t note, uint32_t len)
{
    sg_at(t, note, SG_VEL);
    sg_at(t + len, note, 0);
}

/* the prompt: the reference C, then the target; the answer's time counts from the target */
static void sg_play_prompt(void)
{
    uint32_t now = fm1_ms;
    sg_silence();
    sg_note(now + 40u, SG_REF, 420u);
    sg_note(now + 640u, SG_REF + sgu.target, 600u);
    sgu.t_target = now + 640u;
}

static void sg_tick(void)
{
    while (sgu.ev_n && (int32_t)(fm1_ms - sgu.ev_t[0]) >= 0) {
        uint32_t i, n = sgu.ev_note[0], v = sgu.ev_vel[0];
        if (sg_audio_push(n, v)) {
            if (v && sgu.non < 4u)
                sgu.on[sgu.non++] = (uint8_t)n;
            else if (!v)
                for (i = 0; i < sgu.non; i++)
                    if (sgu.on[i] == n) {
                        sgu.on[i] = sgu.on[--sgu.non];
                        break;
                    }
        }
        for (i = 1; i < sgu.ev_n; i++) {
            sgu.ev_t[i - 1u] = sgu.ev_t[i];
            sgu.ev_note[i - 1u] = sgu.ev_note[i];
            sgu.ev_vel[i - 1u] = sgu.ev_vel[i];
        }
        sgu.ev_n--;
    }
}

/* the two-note chime of a screen change, in the keys' register (a 5th up: the station locking on) */
static void sg_chime(void)
{
    uint32_t base = (uint32_t)(SG_REF + 12 * song.octave);
    sg_silence();
    sg_note(fm1_ms + 10u, base, 90u);
    sg_note(fm1_ms + 120u, base + 7u, 140u);
}

/* --------------------------------------------------------- the flow --- */
static void sg_go(uint32_t st)
{
    sgu.st = (uint8_t)st;
    sgu.dirty = 1;
}

static int sg_pitched(const track_t *t) { return kb_engine(t, 0) < 0 && !t->p[P_MUTE]; }

static int sg_pick_track(void)                          /* the selected track, or the first pitched one */
{
    uint32_t i;
    if (sg_pitched(&trk[song.sel % NTRK]))
        return 1;
    for (i = 0; i < NTRK; i++)
        if (sg_pitched(&trk[i])) {
            song.sel = (uint8_t)i;
            return 1;
        }
    return 0;
}

static void sg_next(void)                               /* the next prompt (or its Reveal), or the summary */
{
    sg_silence();
    sgu.t_target = 0;
    if (sg_sess.pos >= sg_sess.n) {
        sg_session_end(&sg_store);
        sg_chime();
        sg_go(SGS_SUMMARY);
        return;
    }
    sgu.target = sg_sess.q[sg_sess.pos];
    sgu.intro = sg_sess.intro[sg_sess.pos];
    if (sgu.intro && sgu.st != SGS_REVEAL) {            /* a new note: shown and heard first, no score */
        sg_go(SGS_REVEAL);
        sg_play_prompt();
        sgu.t_target = 0;
        return;
    }
    sg_go(SGS_PROMPT);
    sg_play_prompt();
}

static void sg_session_start(void)
{
    if (!sg_pick_track()) {
        sg_go(SGS_NOSOUND);
        return;
    }
    sg_session_build(&sg_store, &sg_sess, fm1_ticks());
    sgu.st = SGS_TITLE;
    sg_next();
}

static void sg_enter(void)
{
    if (song.playing || chain_busy() || seq_counting())
        transport_req = 2;                              /* (stopped: the prompts are heard alone) */
    if (!sg_store_ok(&sg_store))
        sg_store_init(&sg_store);
    sg_on = 1;
    sgu.home_t0 = 0;
    sg_go(SGS_TITLE);
    sg_chime();
}

static void sg_exit(void)
{
    sg_silence();
    sg_on = 0;
    ui.home = 1;
    ui.force = 1;
}

/* F3 + F#3 held in the first 3 s the UI runs after power-on */
static int sg_boot_check(void)
{
    static uint8_t done, started;
    static uint32_t t0;
    if (done)
        return 0;
    if (!started) {
        started = 1;
        t0 = fm1_ms;
    }
    if ((int32_t)(fm1_ms - t0) > 3000) {
        done = 1;
        return 0;
    }
    if ((fm1_in.notes & 3u) == 3u) {
        done = 1;
        sg_enter();
        return 1;
    }
    return 0;
}

static void sg_answer_key(uint32_t k)
{
    uint32_t ms = fm1_ms - sgu.t_target, pc = (53u + k) % 12u;
    sg_grade_t g;
    sgu.answered = (uint8_t)pc;
    g = sg_answer(&sg_store, &sg_sess, sgu.target, pc, ms);
    sgu.grade = (uint8_t)g;
    sg_why(sgu.target, pc, g, sgu.why1, sgu.why2, sizeof sgu.why1);
    sg_trap(&sg_store, sgu.target, sgu.trap, sizeof sgu.trap);
    sgu.fx = g == SG_WRONG ? 1u : 2u;                   /* a glitch on a miss, a chromatic split on a hit */
    sgu.fxn = g == SG_WRONG ? 10u : 6u;
    sg_sess.pos++;
    sgu.ev_n = 0;                                       /* (the prompt's rest not played over the answer) */
    if (g == SG_WRONG) {                                /* the comparison again: C, then the right note */
        sg_note(fm1_ms + 500u, SG_REF, 380u);
        sg_note(fm1_ms + 1000u, SG_REF + sgu.target, 600u);
    }
    sg_go(SGS_FEEDBACK);
}

static void sg_input(uint32_t pressed, uint32_t notes)
{
    uint32_t play = (pressed >> panel.btn[B_PLAY]) & 1u, k;
    int32_t k1 = panel_enc(EN_K1), k4 = panel_enc(EN_K4);
    uint32_t home_dn = (fm1_in.buttons >> panel.btn[B_HOME]) & 1u, home_tap = 0;
    (void)panel_enc(EN_K2);                             /* (the other knobs: read, not used yet) */
    (void)panel_enc(EN_K3);
    (void)panel_enc(EN_SELECT);
    (void)panel_enc(EN_ALGO);
    (void)panel_enc(EN_PRESET);
    song.grid = 0;                                      /* the keys play the selected track's notes */
    sg_tick();
    if (home_dn && !sgu.home_t0)
        sgu.home_t0 = fm1_ms | 1u;
    if (home_dn && sgu.home_t0 && (int32_t)(fm1_ms - (sgu.home_t0 & ~1u)) > 1000) {
        sg_exit();                                      /* HOME held: out */
        return;
    }
    if (!home_dn && sgu.home_t0) {
        sgu.home_t0 = 0;
        home_tap = 1;
    }
    if (home_tap) {
        if (sgu.st == SGS_TITLE || sgu.st == SGS_NOSOUND) {
            sg_exit();
            return;
        }
        sg_silence();
        sg_go(SGS_TITLE);
        return;
    }
    if ((pressed >> panel.btn[B_OCTUP]) & 1u)
        song.octave += song.octave < 3 ? 1 : 0;
    if ((pressed >> panel.btn[B_OCTDN]) & 1u)
        song.octave -= song.octave > -3 ? 1 : 0;
    switch (sgu.st) {
    case SGS_TITLE:
    case SGS_NOSOUND:
        if (play)
            sg_session_start();
        break;
    case SGS_REVEAL:
        if (k1)
            sg_play_prompt(), sgu.t_target = 0;
        if (play)                                       /* now the same note as a prompt */
            sg_go(SGS_PROMPT), sg_play_prompt();
        break;
    case SGS_PROMPT:
        if (k1 || play) {
            sg_play_prompt();
            break;
        }
        if (sgu.t_target && (int32_t)(fm1_ms - sgu.t_target) >= 0 && notes)
            for (k = 0; k < 27u; k++)
                if ((notes >> k) & 1u) {
                    sg_answer_key(k);
                    break;
                }
        break;
    case SGS_FEEDBACK:
        if (k1) {
            sg_silence();
            sg_note(fm1_ms + 20u, SG_REF, 380u);
            sg_note(fm1_ms + 520u, SG_REF + sgu.target, 600u);
        }
        if (play)
            sg_next();
        break;
    case SGS_SUMMARY:
    case SGS_REPORT:
        if (k4)
            sg_go(sgu.st == SGS_SUMMARY ? SGS_REPORT : SGS_SUMMARY);
        if (play)
            sg_session_start();
        break;
    }
}

/* ----------------------------------------------------------- lights --- */
static void sg_leds(void)
{
    uint8_t nl[FM1_NCOL] = {0}, nd[FM1_NCOL] = {0}, nb[FM1_NCOL] = {0};
    uint32_t k, c, show = sgu.st == SGS_REVEAL || sgu.st == SGS_FEEDBACK;
    static uint8_t ready;
    if (!ready) {
        led_pos_init();
        ready = 1;
    }
    for (k = 0; k < 27u; k++) {
        uint32_t pc = (53u + k) % 12u;
        if (show && pc == sgu.target)
            led_put(nl, 14u + k, 1);                    /* the note: lit */
        else if (pc == 0u)
            led_put(nd, 14u + k, 1);                    /* the reference C: a glow */
        if (sgu.st == SGS_FEEDBACK && sgu.grade == SG_WRONG && pc == sgu.answered)
            led_put(nb, 14u + k, 1);                    /* what was played: breathing */
    }
    led_put(sgu.st == SGS_PROMPT ? nd : nl, panel.btn[B_PLAY], 1);
    led_put(nd, panel.btn[B_HOME], 1);
    led_put(nd, panel.btn[B_OCTDN], 1);
    led_put(nd, panel.btn[B_OCTUP], 1);
    fm1_led_dim_level(0);
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led_dim[c] = nd[c];
        fm1_led_breath[c] = nb[c];
        fm1_led_mid[c] = 0;
    }
    for (c = 0; c < FM1_NCOL; c++)
        fm1_led[c] = nl[c];
}

/* ---------------------------------------------------------- drawing --- */
/* the bitmap face of the focal glyph: 5 x 7, drawn in blocks (the oversized glyph of each screen) */
static const uint8_t *sg_glyph(char ch)
{
    static const uint8_t G[][7] = {
        {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},     /* C */
        {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},     /* D */
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},     /* E */
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},     /* F */
        {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},     /* G */
        {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},     /* A */
        {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},     /* B */
        {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A},     /* # */
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},     /* ? */
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},     /* 0 */
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},     /* 1 */
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},     /* 2 */
        {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},     /* 3 */
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},     /* 4 */
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},     /* 5 */
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},     /* 6 */
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},     /* 7 */
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},     /* 8 */
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},     /* 9 */
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},     /* . */
        {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10},     /* / */
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};    /* (space, and anything else) */
    static const char MAP[] = "CDEFGAB#?0123456789./";
    uint32_t i;
    for (i = 0; MAP[i] && MAP[i] != ch; i++)
        ;
    return G[i];
}

/* the oversized glyph: s centred on cx at y, blocks of px; glitch moves rows sideways */
static void sg_big(int32_t cx, int32_t y, const char *s, uint32_t px, uint16_t c, int32_t glitch)
{
    uint32_t n = 0, i, r, b;
    int32_t x;
    while (s[n])
        n++;
    x = cx - (int32_t)((n * 6u - 1u) * px) / 2;
    for (i = 0; i < n; i++, x += (int32_t)(6u * px)) {
        const uint8_t *g = sg_glyph(s[i]);
        for (r = 0; r < 7u; r++) {
            int32_t dx = glitch ? (int32_t)(((sgu.frame * 7u + r * 13u + i * 5u) % 5u) - 2u) * glitch : 0;
            for (b = 0; b < 5u; b++)
                if ((g[r] >> (4u - b)) & 1u)
                    cv_rect(x + (int32_t)(b * px) + dx, y + (int32_t)(r * px), (int32_t)px, (int32_t)px, c);
        }
    }
}

/* misregistered: a colour plane two pixels off, then the glyph in paper; the effects widen the offset */
static void sg_focal(int32_t cx, int32_t y, const char *s, uint32_t px, uint16_t plane)
{
    int32_t off = 2, gl = 0;
    if (sgu.fxn && sgu.fx == 1u)
        gl = 4, off = 4;
    else if (sgu.fxn && sgu.fx == 2u)
        off = 2 + (int32_t)sgu.fxn;
    sg_big(cx + off, y + (off > 2 ? 0 : 2), s, px, plane, gl);
    sg_big(cx, y, s, px, SG_PAPER, gl / 2);
}

static void sg_text_c(int32_t y, const aafont_t *f, const char *s, uint16_t c)
{
    cv_text_on(120 - text_w(f, s) / 2, y, f, s, c, SG_INK);
}

static void sg_u2(char *b, uint32_t v)                 /* 0..99 */
{
    if (v > 99u)
        v = 99u;
    if (v >= 10u)
        *b++ = (char)('0' + v / 10u);
    *b++ = (char)('0' + v % 10u);
    *b = 0;
}

static void sg_head(const char *right, uint16_t accent)
{
    cv_rect(0, 0, 240, 22, SG_INK);
    cv_text_on(8, 4, &AF_S, "88.1 NOTE ID", accent, SG_INK);
    if (right)
        cv_text_r(232, 4, &AF_S, right, SG_PAPER, SG_INK);
    cv_rect(8, 21, 224, 1, accent);
}

static void sg_foot(const char *s, uint16_t accent)
{
    cv_rect(0, 220, 240, 20, SG_INK);
    cv_rect(8, 220, 224, 1, accent);
    cv_text_on(8, 224, &AF_S, s, SG_PAPER, SG_INK);
}

/* the semitone ladder, C to C: the gap from C to the note lit */
static void sg_ladder(int32_t y, uint32_t pc, uint16_t lit)
{
    uint32_t i;
    for (i = 0; i <= 12u; i++) {
        int32_t x = 16 + (int32_t)i * 16;
        int black = i == 1u || i == 3u || i == 6u || i == 8u || i == 10u;
        uint16_t c = i <= pc && pc ? lit : (i == 0u || i == 12u) ? SG_PAPER : SG_SCAN;
        cv_rect(x, y + (black ? 0 : 6), 12, black ? 10 : 14, c);
    }
    if (!pc)
        cv_rect(16, y + 6, 12, 14, lit);
}

static void sg_progress(char *b)                        /* "3/10" */
{
    char t[4];
    b[0] = 0;
    sg_u2(t, sg_sess.pos + (sgu.st == SGS_FEEDBACK ? 0u : 1u));
    sg_cat(b, 8, t);
    sg_cat(b, 8, "/");
    sg_u2(t, sg_sess.n);
    sg_cat(b, 8, t);
}

/* the Garden: one plant a note, in the order notes open (seed, sprout, wilting, bloom) */
static void sg_garden(int32_t y)
{
    uint32_t i;
    for (i = 0; i < SG_NOTES; i++) {
        uint32_t pc = SG_ORDER[i];
        const sg_item_t *it = &sg_store.it[pc];
        int32_t x = 18 + (int32_t)i * 18;
        if (!it->seen) {
            cv_rect(x + 4, y + 22, 4, 2, SG_SCAN);      /* a seed */
            continue;
        }
        if (sg_wilting(&sg_store, pc)) {                /* due and not watered: drooping, pink */
            cv_rect(x + 5, y + 12, 2, 12, SG_PINK);
            cv_rect(x + 7, y + 12, 4, 2, SG_PINK);
            cv_rect(x + 9, y + 14, 2, 3, SG_PINK);
            continue;
        }
        cv_rect(x + 5, y + (sg_mastered(it) ? 6 : 12), 2, sg_mastered(it) ? 18 : 12, SG_GREEN);
        cv_rect(x + 1, y + 14, 4, 2, SG_GREEN);         /* a leaf */
        if (sg_mastered(it))
            cv_rect(x + 2, y, 8, 6, SG_PAPER);          /* in bloom */
    }
}

static void sg_scanlines(void)
{
    int32_t y;
    for (y = 24; y < 218; y += 3)
        cv_rect(0, y, 240, 1, SG_SCAN);
}

static void sg_scene(void)
{
    char b[24], t[4];
    cv_rect(0, 0, 240, 240, SG_INK);
    sg_scanlines();
    switch (sgu.st) {
    case SGS_TITLE:
        sg_head(SG_RANK[sg_rank(&sg_store)], SG_GREEN);   /* (green: the Garden's colour, pink its wilting) */
        sg_focal(120, 40, "88.1", 8, SG_GREEN);
        sg_text_c(110, &AF_L, "NOTE ID", SG_PAPER);
        sg_text_c(146, &AF_S, "A C, THEN A NOTE. PLAY THE NOTE", SG_GREEN);
        sg_garden(170);
        sg_foot("PLAY TUNE IN    HOME EXIT", SG_GREEN);
        break;
    case SGS_REVEAL:
        sg_head("NEW NOTE", SG_GREEN);
        sg_focal(120, 34, SG_NAME[sgu.target], 9, SG_GREEN);
        b[0] = 0;
        sg_cat(b, sizeof b, SG_IVL[sgu.target]);
        if (sgu.target)
            sg_cat(b, sizeof b, " ABOVE C");
        sg_text_c(112, &AF_M, b, SG_PAPER);
        sg_u2(t, sgu.target);
        b[0] = 0;
        sg_cat(b, sizeof b, t);
        sg_cat(b, sizeof b, sgu.target == 1u ? " HALF STEP FROM C" : " HALF STEPS FROM C");
        sg_text_c(134, &AF_S, b, SG_GREEN);
        sg_ladder(156, sgu.target, SG_GREEN);
        sg_text_c(186, &AF_S, "LIT KEYS: THIS NOTE. TRY THEM", SG_PAPER);
        sg_foot("PLAY QUIZ ME    K1 HEAR AGAIN", SG_GREEN);
        break;
    case SGS_PROMPT:
        sg_progress(b);
        sg_head(b, SG_CYAN);
        sg_focal(120, 44, "?", 10, SG_CYAN);
        sg_text_c(128, &AF_M, sgu.cue ? "PLAY THE NOTE" : "LISTEN", SG_PAPER);
        sg_text_c(152, &AF_S, "C FIRST, THEN THE NOTE", SG_CYAN);
        sg_text_c(170, &AF_S, "ANY OCTAVE COUNTS", SG_PAPER);
        sg_foot("PLAY OR K1 HEAR AGAIN", SG_CYAN);
        break;
    case SGS_FEEDBACK: {
        uint16_t a = sgu.grade == SG_WRONG ? SG_PINK : SG_GREEN;
        sg_progress(b);
        sg_head(b, a);
        sg_focal(120, 30, SG_NAME[sgu.target], 8, a);
        sg_text_c(92, &AF_M, sgu.why1, sgu.grade == SG_WRONG ? SG_PINK : SG_PAPER);
        sg_text_c(112, &AF_S, sgu.why2, SG_PAPER);
        sg_ladder(132, sgu.target, a);
        if (sgu.grade == SG_WRONG)                      /* what was played: a mark under its rung */
            cv_rect(16 + (int32_t)sgu.answered * 16, 154, 12, 3, SG_PAPER);
        cv_text_on(16, 166, &AF_S, "NEXT TRAP", a, SG_INK);
        cv_text_on(16, 184, &AF_S, sgu.trap, SG_PAPER, SG_INK);
        sg_foot(sg_sess.pos >= sg_sess.n ? "PLAY RESULTS    K1 HEAR AGAIN" : "PLAY NEXT    K1 HEAR AGAIN", a);
        break;
    }
    case SGS_SUMMARY: {
        uint8_t p[3];
        uint32_t n = sg_top_pairs(&sg_store, p, 3), i;
        sg_head("SESSION DONE", SG_GREEN);
        b[0] = 0;
        sg_u2(t, sg_sess.right + sg_sess.shaky);
        sg_cat(b, sizeof b, t);
        sg_cat(b, sizeof b, "/");
        sg_u2(t, sg_sess.right + sg_sess.shaky + sg_sess.wrong);
        sg_cat(b, sizeof b, t);
        sg_focal(120, 32, b, 6, SG_GREEN);
        b[0] = 0;
        sg_cat(b, sizeof b, "SHAKY ");
        sg_u2(t, sg_sess.shaky);
        sg_cat(b, sizeof b, t);
        sg_cat(b, sizeof b, "   MISSED ");
        sg_u2(t, sg_sess.wrong);
        sg_cat(b, sizeof b, t);
        sg_text_c(84, &AF_S, b, SG_PAPER);
        cv_text_on(16, 106, &AF_S, "WEAK SPOTS", SG_PINK, SG_INK);
        if (!n)
            cv_text_on(16, 124, &AF_S, "NONE YET. CLEAN SIGNAL", SG_PAPER, SG_INK);
        for (i = 0; i < n; i++) {
            b[0] = 0;
            sg_cat(b, sizeof b, SG_NAME[p[i] >> 4]);
            sg_cat(b, sizeof b, " HEARD AS ");
            sg_cat(b, sizeof b, SG_NAME[p[i] & 15u]);
            sg_cat(b, sizeof b, "  x");
            sg_u2(t, sg_store.conf[p[i] >> 4][p[i] & 15u]);
            sg_cat(b, sizeof b, t);
            cv_text_on(16, 124 + (int32_t)i * 16, &AF_S, b, SG_PAPER, SG_INK);
        }
        sg_garden(172);
        sg_foot("PLAY AGAIN   K4 REPORT   HOME", SG_GREEN);
        break;
    }
    case SGS_REPORT: {
        uint32_t i;
        sg_head("WEAK-SPOT REPORT", SG_PINK);
        for (i = 0; i < SG_NOTES; i++) {
            uint32_t pc = SG_ORDER[i];
            const sg_item_t *it = &sg_store.it[pc];
            int32_t x = 12 + (int32_t)(i % 2u) * 114, y = 30 + (int32_t)(i / 2u) * 30;
            cv_text_on(x, y, &AF_M, SG_NAME[pc], it->seen ? SG_PAPER : SG_SCAN, SG_INK);
            cv_rect(x + 30, y + 6, 70, 6, SG_SCAN);
            if (it->seen)
                cv_rect(x + 30, y + 6, (int32_t)it->acc * 70 / 100, 6, it->acc >= 70u ? SG_CYAN : SG_PINK);
        }
        sg_foot("BAR: ACCURACY   K4 BACK", SG_PINK);
        break;
    }
    default:
        sg_head(0, SG_PINK);
        sg_focal(120, 40, "?", 8, SG_PINK);
        sg_text_c(120, &AF_M, "NO PITCHED SOUND", SG_PAPER);
        sg_text_c(144, &AF_S, "LOAD A SYNTH SOUND ON A TRACK", SG_PAPER);
        sg_text_c(162, &AF_S, "(DRUM AND SLICE HAVE NO PITCH)", SG_PINK);
        sg_foot("PLAY TRY AGAIN    HOME EXIT", SG_PINK);
        break;
    }
}

static void sg_strips(void);
static void sg_draw(void)
{
    sgu.frame++;
    {
        uint8_t cue = sgu.st == SGS_PROMPT && sgu.t_target && (int32_t)(fm1_ms - sgu.t_target) >= 0;
        if (cue != sgu.cue) {                           /* LISTEN -> PLAY THE NOTE */
            sgu.cue = cue;
            sgu.dirty = 1;
        }
    }
    if (sgu.fxn) {                                      /* the effect's frames: one a drawn frame (~60 fps) */
        sgu.fxn--;
        sgu.dirty = 1;
    }
    if (!sgu.dirty && !ui.force)
        return;
    sgu.dirty = 0;
    {
        uint8_t mono = ux.mono, light = ux.light;   /* the Hidden Net's own colours, whatever MENU > COLOR is: */
        ux.mono = 0;                                /* (MONO greys by the red channel: cyan would go dark) */
        ux.light = 0;
        if (mono || light)
            ux.gen++;                               /* (the text ramps are built again for these colours) */
        sg_strips();
        ux.mono = mono;
        ux.light = light;
        if (mono || light)
            ux.gen++;
    }
}

static void sg_strips(void)
{
    uint32_t y0;
    for (y0 = 0; y0 < 240u; y0 += 120u) {               /* two strips of 240 x 120 (the canvas holds 124 rows) */
        cv_begin(240, 120, SG_INK);
        cv_oy = -(int32_t)y0;
        sg_scene();
        cv_oy = 0;
        cv_blit(0, y0);
    }
}
