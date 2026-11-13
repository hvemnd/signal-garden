/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 hvemnd (SIGNAL GARDEN)
 * SIGNAL GARDEN's learning engine (firmware/src/sg_core.c) on the host:
 *   cc -O1 -Wall -Ifirmware/src -o build/host/sg_test tests/sg_test.c && build/host/sg_test */
#include <stdio.h>
#include <string.h>
#include "../firmware/src/sg_core.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void t_first_session(void)
{
    sg_store_t s;
    sg_session_t x;
    uint32_t i, intros = 0;
    sg_store_init(&s);
    sg_session_build(&s, &x, 1);
    CHECK(x.n == SG_ROUNDS, "first session has %u rounds", x.n);
    for (i = 0; i < SG_START_ITEMS; i++) {
        CHECK(x.intro[i] && x.q[i] == SG_ORDER[i], "intro %u is %s", i, SG_NAME[SG_ORDER[i]]);
        intros += x.intro[i];
    }
    for (i = 0; i < x.n; i++) {
        uint32_t k, ok = 0;
        for (k = 0; k < SG_START_ITEMS; k++)
            ok |= x.q[i] == SG_ORDER[k];
        CHECK(ok, "round %u is a starting note", i);
        if (i)
            CHECK(x.q[i] != x.q[i - 1u], "no repeat at %u", i);
    }
    CHECK(intros == SG_START_ITEMS, "3 intros");
}

static void t_sm2(void)
{
    sg_store_t s;
    sg_session_t x;
    sg_grade_t g;
    sg_store_init(&s);
    memset(&x, 0, sizeof x);
    x.n = 1;
    g = sg_answer(&s, &x, 7, 7, 1500);
    CHECK(g == SG_OK && s.it[7].ivl == 1 && s.it[7].due == 1, "first right: 1 session (ivl %u)", s.it[7].ivl);
    s.session = 1;
    g = sg_answer(&s, &x, 7, 7, 1500);
    CHECK(s.it[7].ivl == 3, "second right: 3 sessions (%u)", s.it[7].ivl);
    s.session = 4;
    g = sg_answer(&s, &x, 7, 7, 1500);
    CHECK(s.it[7].ivl >= 7 && s.it[7].ivl <= 8, "third right: ivl x ease (%u)", s.it[7].ivl);
    CHECK(sg_mastered(&s.it[7]), "G mastered");
    g = sg_answer(&s, &x, 7, 7, 4000);
    CHECK(g == SG_SHAKY && s.it[7].ivl == 1, "slow right: shaky, next session (g %d ivl %u)", g, s.it[7].ivl);
    g = sg_answer(&s, &x, 7, 5, 900);
    CHECK(g == SG_WRONG && s.it[7].reps == 0 && s.it[7].lapses == 1 && s.conf[7][5] == 1, "wrong: reset, counted");
    CHECK(x.n == 2 && x.q[1] == 7, "wrong: brought back in the session (n %u)", x.n);
    g = sg_answer(&s, &x, 7, 5, 900);
    CHECK(x.n == 2, "brought back once only");
    CHECK(s.it[7].ease >= SG_EASE_MIN, "ease floor");
}

static void t_pairs_and_build(void)
{
    sg_store_t s;
    sg_session_t x;
    uint8_t p[3];
    uint32_t n, i, k, newc = 0;
    sg_store_init(&s);
    for (i = 0; i < 6; i++)
        s.it[SG_ORDER[i]].seen = 1, s.it[SG_ORDER[i]].due = 5, s.it[SG_ORDER[i]].acc = 80;
    s.session = 3;                                      /* nothing due */
    s.conf[4][5] = 5;                                   /* E heard as F, five times */
    s.conf[0][2] = 2;
    s.conf[7][5] = 3;
    n = sg_top_pairs(&s, p, 3);
    CHECK(n == 3 && p[0] == (4 << 4 | 5) && p[1] == (7 << 4 | 5) && p[2] == (0 << 4 | 2), "top pairs in order");
    sg_session_build(&s, &x, 42);
    CHECK(x.n == SG_ROUNDS, "built %u", x.n);
    for (i = 0; i < x.n; i++)
        newc += x.intro[i];
    CHECK(newc == 1, "nothing due: one new note (%u)", newc);
    for (i = 0; i < x.n; i++)
        if (x.intro[i])
            CHECK(x.q[i] == SG_ORDER[6], "the next new note is B (%s)", SG_NAME[x.q[i]]);
    for (k = i = 0; i < x.n; i++)
        k += x.q[i] == 4 || x.q[i] == 5;
    CHECK(k >= 2, "the E/F pair drilled (%u)", k);
    for (i = 0; i < 12; i++)                            /* everything due: no new note */
        if (s.it[i].seen) s.it[i].due = 0;
    for (i = 6; i < 12; i++) s.it[SG_ORDER[i]].seen = 1;
    s.session = 9;
    sg_session_build(&s, &x, 7);
    for (newc = i = 0; i < x.n; i++)
        newc += x.intro[i];
    CHECK(newc == 0, "a due backlog: no new note");
}

static void t_words(void)
{
    char a[48], b[48], c[48];
    uint32_t t, w;
    sg_store_t s;
    sg_store_init(&s);
    for (t = 0; t < 12; t++)
        for (w = 0; w < 12; w++) {
            sg_why(t, w, t == w ? SG_OK : SG_WRONG, a, b, sizeof a);
            sg_trap(&s, t, c, sizeof c);
            CHECK(strlen(a) <= 34 && strlen(b) <= 34 && strlen(c) <= 34, "lengths %s / %s / %s", a, b, c);
        }
    sg_why(0, 2, SG_WRONG, a, b, sizeof a);
    CHECK(!strcmp(a, "YOU PLAYED D. IT WAS C") && !strcmp(b, "D IS A WHOLE STEP TOO HIGH"), "%s / %s", a, b);
    sg_why(7, 7, SG_OK, a, b, sizeof a);
    CHECK(!strcmp(b, "G IS A PERFECT 5TH UP"), "%s", b);
    s.conf[4][5] = 2;
    sg_trap(&s, 4, c, sizeof c);
    CHECK(!strcmp(c, "YOU MIX E WITH F"), "%s", c);
}

/* a simulated learner who knows C G E well and mixes up neighbours sometimes: over 30 sessions every note opens */
static void t_long_run(void)
{
    sg_store_t s;
    sg_session_t x;
    uint32_t sess, r = 12345, opened;
    sg_store_init(&s);
    for (sess = 0; sess < 40; sess++) {
        sg_session_build(&s, &x, sess * 77u + 1u);
        for (x.pos = 0; x.pos < x.n; x.pos++) {
            uint32_t t = x.q[x.pos], a = t;
            if (sg_rand(&r) % 100u < 15u)
                a = (t + 1u) % 12u;
            (void)sg_answer(&s, &x, t, a, 800u + sg_rand(&r) % 1500u);
        }
        sg_session_end(&s);
    }
    opened = sg_seen_count(&s);
    CHECK(opened == 12, "all 12 notes opened after 40 sessions (%u)", opened);
    CHECK(sg_rank(&s) >= 1, "rank moved (%s)", SG_RANK[sg_rank(&s)]);
    printf("sg: 40 sessions: %u notes, rank %s, session %u\n", opened, SG_RANK[sg_rank(&s)], s.session);
}

int main(void)
{
    t_first_session();
    t_sm2();
    t_pairs_and_build();
    t_words();
    t_long_run();
    printf("sg: store %u bytes\n", (unsigned)sizeof(sg_store_t));
    printf(fails ? "sg: %d FAILED\n" : "sg: all passed\n", fails);
    return fails != 0;
}
