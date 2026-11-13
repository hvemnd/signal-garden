/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 hvemnd (SIGNAL GARDEN)
 * SIGNAL GARDEN: the learning engine (no hardware, no Felucca state: host-tested by tests/sg_test.c).
 *
 * Station 88.1, NOTE ID. A reference C plays, then a target note; the learner plays the target on the keys. The
 * answer is a pitch class (any octave counts), so the 27 keys always cover it. Note ID is taught relative to C, not
 * as absolute pitch: the Reveal shows the note's distance from C, and every prompt starts with that C.
 *
 * The clock is the practice session, not the calendar: the FM-1 has no real-time clock, so an item's interval and due
 * date are counted in sessions (a session is one run of SG_ROUNDS prompts). SM-2 style scheduling, small enough for a
 * few hundred bytes of RAM (sg_store_t is 296 bytes). */
#ifndef SG_CORE_C
#define SG_CORE_C
#include <stdint.h>

#define SG_NOTES 12u
#define SG_ROUNDS 10u                 /* prompts in a session (beginner length: ~3 minutes) */
#define SG_QMAX 16u                   /* a session's queue: the rounds plus missed items brought back */
#define SG_START_ITEMS 3u             /* introduced in the first session (C, G, E: far apart, easy to tell apart) */
#define SG_MAGIC 0x53474431u          /* "SGD1" */
#define SG_EASE0 250u                 /* ease x100 */
#define SG_EASE_MIN 130u

/* the order notes are introduced: the major triad first, then the other white keys, then the black keys */
static const uint8_t SG_ORDER[SG_NOTES] = {0, 7, 4, 5, 2, 9, 11, 6, 10, 1, 3, 8};
static const char *const SG_NAME[SG_NOTES] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const SG_IVL[SG_NOTES] = {"THE SAME AS C", "A HALF STEP", "A WHOLE STEP", "A MINOR 3RD",
                                              "A MAJOR 3RD", "A PERFECT 4TH", "A TRITONE", "A PERFECT 5TH",
                                              "A MINOR 6TH", "A MAJOR 6TH", "A MINOR 7TH", "A MAJOR 7TH"};

typedef struct {
    uint16_t ease;                    /* x100, SG_EASE_MIN .. 300 */
    uint16_t ivl;                     /* sessions until the next review */
    uint16_t due;                     /* the session it is due in */
    uint16_t med_ms;                  /* running median of the answer time (0: none yet) */
    uint8_t reps;                     /* correct reviews in a row */
    uint8_t lapses;
    uint8_t acc;                      /* rolling accuracy, % */
    uint8_t seen;                     /* introduced */
} sg_item_t;

typedef struct {
    uint32_t magic;
    uint16_t session;                 /* sessions finished: the clock */
    uint16_t answered;                /* answers ever (ranks) */
    sg_item_t it[SG_NOTES];
    uint8_t conf[SG_NOTES][SG_NOTES]; /* [target][answered]: wrong answers, saturating */
} sg_store_t;

typedef enum { SG_OK_FAST, SG_OK, SG_SHAKY, SG_WRONG } sg_grade_t;

typedef struct {
    uint8_t q[SG_QMAX];               /* the session's targets */
    uint8_t intro[SG_QMAX];           /* 1: a new item (a Reveal before its prompt) */
    uint8_t n, pos;
    uint8_t right, shaky, wrong;
    uint16_t requeued;                /* bit per pitch class: brought back once already this session */
    uint32_t rng;
} sg_session_t;

static void sg_store_init(sg_store_t *s)
{
    uint32_t i, j;
    s->magic = SG_MAGIC;
    s->session = 0;
    s->answered = 0;
    for (i = 0; i < SG_NOTES; i++) {
        sg_item_t *it = &s->it[i];
        it->ease = SG_EASE0;
        it->ivl = 0;
        it->due = 0;
        it->med_ms = 0;
        it->reps = 0;
        it->lapses = 0;
        it->acc = 0;
        it->seen = 0;
        for (j = 0; j < SG_NOTES; j++)
            s->conf[i][j] = 0;
    }
}

static int sg_store_ok(const sg_store_t *s) { return s->magic == SG_MAGIC; }

static uint32_t sg_rand(uint32_t *r)              /* xorshift32 */
{
    uint32_t x = *r ? *r : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *r = x;
}

static uint32_t sg_seen_count(const sg_store_t *s)
{
    uint32_t i, n = 0;
    for (i = 0; i < SG_NOTES; i++)
        n += s->it[i].seen;
    return n;
}

/* mastered: three correct reviews in a row and an interval of 4 sessions or more (a grown plant in the Garden) */
static int sg_mastered(const sg_item_t *it) { return it->seen && it->reps >= 3u && it->ivl >= 4u; }

/* wilting: due and not yet reviewed (overdue by a session or more) */
static int sg_wilting(const sg_store_t *s, uint32_t pc) { return s->it[pc].seen && s->it[pc].due < s->session; }

static int sg_queue_has(const sg_session_t *x, uint32_t pc, uint32_t from)
{
    uint32_t i;
    for (i = from; i < x->n; i++)
        if (x->q[i] == pc)
            return 1;
    return 0;
}

static void sg_push(sg_session_t *x, uint32_t pc, uint32_t intro)
{
    if (x->n < SG_QMAX) {
        x->intro[x->n] = (uint8_t)intro;
        x->q[x->n++] = (uint8_t)pc;
    }
}

/* the confusion pairs, most counted first: out[k] = target << 4 | answered; returns how many (at most max) */
static uint32_t sg_top_pairs(const sg_store_t *s, uint8_t *out, uint32_t max)
{
    uint32_t n = 0, a, b, k;
    uint8_t cnt[3] = {0, 0, 0};
    if (max > 3u)
        max = 3u;
    for (a = 0; a < SG_NOTES; a++)
        for (b = 0; b < SG_NOTES; b++) {
            uint32_t c = s->conf[a][b];
            if (!c)
                continue;
            for (k = 0; k < n && cnt[k] >= c; k++)
                ;
            if (k >= max)
                continue;
            if (n < max)
                n++;
            {
                uint32_t j;
                for (j = n - 1u; j > k; j--) {
                    out[j] = out[j - 1u];
                    cnt[j] = cnt[j - 1u];
                }
            }
            out[k] = (uint8_t)(a << 4 | b);
            cnt[k] = (uint8_t)c;
        }
    return n;
}

/* the most common wrong answer for target pc, or -1 */
static int32_t sg_worst_confusion(const sg_store_t *s, uint32_t pc)
{
    uint32_t b, best = 0;
    int32_t r = -1;
    for (b = 0; b < SG_NOTES; b++)
        if (s->conf[pc][b] > best) {
            best = s->conf[pc][b];
            r = (int32_t)b;
        }
    return r;
}

/* A session: ~60 % due items (the oldest first), ~25 % the weakest confusion pairs, ~15 % new material, and new
 * material only once nothing due is left out. Slots none of these fill go to the seen items with the lowest accuracy
 * (more review), so a session is always SG_ROUNDS long. The first session introduces SG_START_ITEMS notes. */
static void sg_session_build(const sg_store_t *s, sg_session_t *x, uint32_t seed)
{
    uint32_t i, j, n_due = 0, want_due = SG_ROUNDS * 6u / 10u, want_conf = SG_ROUNDS * 25u / 100u;
    uint8_t due[SG_NOTES], pairs[3];
    uint32_t np, left;
    x->n = x->pos = 0;
    x->right = x->shaky = x->wrong = 0;
    x->requeued = 0;
    x->rng = seed | 1u;
    if (!sg_seen_count(s)) {                            /* first session: each new note revealed, then drilled */
        for (i = 0; i < SG_START_ITEMS; i++)
            sg_push(x, SG_ORDER[i], 1);
        while (x->n < SG_ROUNDS) {
            uint32_t pc = SG_ORDER[sg_rand(&x->rng) % SG_START_ITEMS];
            if (x->q[x->n - 1u] != pc)
                sg_push(x, pc, 0);
        }
        return;
    }
    for (i = 0; i < SG_NOTES; i++)                      /* due: oldest due date first */
        if (s->it[i].seen && s->it[i].due <= s->session)
            due[n_due++] = (uint8_t)i;
    for (i = 1; i < n_due; i++)
        for (j = i; j > 0 && s->it[due[j]].due < s->it[due[j - 1u]].due; j--) {
            uint8_t t = due[j];
            due[j] = due[j - 1u];
            due[j - 1u] = t;
        }
    for (i = 0; i < n_due && i < want_due; i++)
        sg_push(x, due[i], 0);
    np = sg_top_pairs(s, pairs, 3);                     /* confusion: both sides of each pair, alternating */
    for (i = 0; i < np * 2u && x->n < want_due + want_conf; i++) {
        uint32_t pr = pairs[i >> 1], pc = (i & 1u) ? pr & 15u : pr >> 4;
        if (s->it[pc].seen)
            sg_push(x, pc, 0);
    }
    if (n_due <= want_due)                              /* new: only once the due queue is clear */
        for (i = 0; i < SG_NOTES && x->n < SG_ROUNDS; i++)
            if (!s->it[SG_ORDER[i]].seen) {
                sg_push(x, SG_ORDER[i], 1);
                break;                                  /* one new note a session */
            }
    left = SG_ROUNDS > x->n ? SG_ROUNDS - x->n : 0u;    /* the rest: the weakest seen items */
    while (left--) {
        uint32_t best = SG_NOTES, bscore = 0xFFFFu;
        for (i = 0; i < SG_NOTES; i++) {
            uint32_t sc;
            if (!s->it[i].seen)
                continue;
            sc = s->it[i].acc * 4u + (sg_rand(&x->rng) & 63u);
            if (x->n && x->q[x->n - 1u] == i)
                sc += 1000u;                            /* not twice in a row */
            if (sc < bscore) {
                bscore = sc;
                best = i;
            }
        }
        if (best == SG_NOTES)
            break;
        sg_push(x, best, 0);
    }
    for (i = x->n; i > 1u; i--) {                       /* shuffle, keeping the intros where they are */
        uint32_t a = i - 1u, b = sg_rand(&x->rng) % i;
        uint8_t t;
        if (x->intro[a] || x->intro[b])
            continue;
        t = x->q[a];
        x->q[a] = x->q[b];
        x->q[b] = t;
    }
    for (i = 1; i < x->n; i++)                          /* no repeats in a row where a swap fixes it */
        if (x->q[i] == x->q[i - 1u] && !x->intro[i])
            for (j = i + 1u; j < x->n; j++)
                if (x->q[j] != x->q[i] && !x->intro[j] && (j + 1u >= x->n || x->q[j + 1u] != x->q[i])) {
                    uint8_t t = x->q[i];
                    x->q[i] = x->q[j];
                    x->q[j] = t;
                    break;
                }
}

/* the grade of an answer: wrong; right but slower than the item's median (shaky); right and fast; right */
static sg_grade_t sg_grade_of(const sg_item_t *it, int correct, uint32_t ms)
{
    if (!correct)
        return SG_WRONG;
    if (it->med_ms && ms > it->med_ms + it->med_ms / 4u)
        return SG_SHAKY;
    if (it->med_ms && ms < it->med_ms - it->med_ms / 4u)
        return SG_OK_FAST;
    return SG_OK;
}

/* score one answer: the item's schedule, accuracy, median time, the confusion matrix; a miss comes back once later in
 * the session (a dead letter recovered). Returns the grade */
static sg_grade_t sg_answer(sg_store_t *s, sg_session_t *x, uint32_t target, uint32_t answered, uint32_t ms)
{
    sg_item_t *it = &s->it[target];
    int correct = target == answered;
    sg_grade_t g = sg_grade_of(it, correct, ms);
    it->seen = 1;
    if (s->answered < 0xFFFFu)
        s->answered++;
    it->acc = (uint8_t)(it->acc + ((correct ? 100 : 0) - (int32_t)it->acc) / 4);
    if (correct) {                                      /* median: a step towards this answer's time */
        if (!it->med_ms) {
            it->med_ms = (uint16_t)(ms > 60000u ? 60000u : ms);
        } else {
            uint32_t st = it->med_ms / 16u > 16u ? it->med_ms / 16u : 16u;
            if (ms > it->med_ms)
                it->med_ms = (uint16_t)(it->med_ms + st > 60000u ? 60000u : it->med_ms + st);
            else if (it->med_ms > st)
                it->med_ms = (uint16_t)(it->med_ms - st);
        }
    }
    switch (g) {
    case SG_WRONG:
        it->reps = 0;
        it->ivl = 1;
        if (it->lapses < 255u)
            it->lapses++;
        it->ease = it->ease > SG_EASE_MIN + 20u ? (uint16_t)(it->ease - 20u) : (uint16_t)SG_EASE_MIN;
        if (s->conf[target][answered] < 255u)
            s->conf[target][answered]++;
        x->wrong++;
        if (!((x->requeued >> target) & 1u) && x->n < SG_QMAX) {   /* back in 3 prompts (or at the end) */
            uint32_t at = x->pos + 4u < x->n ? x->pos + 4u : x->n, i;
            for (i = x->n; i > at; i--) {
                x->q[i] = x->q[i - 1u];
                x->intro[i] = x->intro[i - 1u];
            }
            x->q[at] = (uint8_t)target;
            x->intro[at] = 0;
            x->n++;
            x->requeued |= (uint16_t)(1u << target);
        }
        break;
    case SG_SHAKY:                                      /* right but slow: comes back next session */
        it->ivl = 1;
        it->ease = it->ease > SG_EASE_MIN + 14u ? (uint16_t)(it->ease - 14u) : (uint16_t)SG_EASE_MIN;
        if (it->reps < 255u)
            it->reps++;
        x->shaky++;
        break;
    default:
        it->ivl = it->reps == 0u ? 1u : it->reps == 1u ? 3u : (uint16_t)((it->ivl * it->ease + 50u) / 100u);
        if (it->ivl > 60u)
            it->ivl = 60u;
        if (g == SG_OK_FAST && it->ease < 300u)
            it->ease = (uint16_t)(it->ease + 10u);
        if (it->reps < 255u)
            it->reps++;
        x->right++;
        break;
    }
    it->due = (uint16_t)(s->session + it->ivl);
    return g;
}

/* the session is over: the clock moves on */
static void sg_session_end(sg_store_t *s)
{
    if (s->session < 0xFFFEu)
        s->session++;
}

/* rank by answers and mastered notes: ANTENNA RECEIVER OPERATOR ARCHITECT */
static const char *const SG_RANK[4] = {"ANTENNA", "RECEIVER", "OPERATOR", "ARCHITECT"};
static uint32_t sg_rank(const sg_store_t *s)
{
    uint32_t i, m = 0;
    for (i = 0; i < SG_NOTES; i++)
        m += (uint32_t)sg_mastered(&s->it[i]);
    return m >= 12u ? 3u : m >= 7u ? 2u : m >= 3u ? 1u : 0u;
}

/* ------------------------------------------------------- the words --- */
static void sg_cat(char *d, uint32_t n, const char *a)
{
    uint32_t i = 0;
    while (d[i] && i + 1u < n)
        i++;
    while (*a && i + 1u < n)
        d[i++] = *a++;
    d[i] = 0;
}

/* signed distance a - t, -6 .. +6 (the nearer way round) */
static int32_t sg_dist(uint32_t t, uint32_t a)
{
    int32_t d = ((int32_t)a - (int32_t)t + 12) % 12;
    return d > 6 ? d - 12 : d;
}

static const char *sg_steps_word(uint32_t d)          /* 1..6 semitones */
{
    static const char *const W[7] = {"", "A HALF STEP", "A WHOLE STEP", "3 HALF STEPS", "4 HALF STEPS",
                                     "5 HALF STEPS", "6 HALF STEPS"};
    return W[d <= 6u ? d : 6u];
}

/* why: line 1 the verdict, line 2 the reason (each at most ~34 characters: AF_S across 220 px) */
static void sg_why(uint32_t target, uint32_t answered, sg_grade_t g, char *l1, char *l2, uint32_t n)
{
    l1[0] = l2[0] = 0;
    if (g != SG_WRONG) {
        sg_cat(l1, n, g == SG_SHAKY ? "RIGHT, BUT SLOW: " : "RIGHT: ");
        sg_cat(l1, n, SG_NAME[target]);
        sg_cat(l2, n, SG_NAME[target]);
        sg_cat(l2, n, " IS ");
        sg_cat(l2, n, SG_IVL[target]);
        if (target)
            sg_cat(l2, n, " UP");
        return;
    }
    {
        int32_t d = sg_dist(target, answered);
        sg_cat(l1, n, "YOU PLAYED ");
        sg_cat(l1, n, SG_NAME[answered]);
        sg_cat(l1, n, ". IT WAS ");
        sg_cat(l1, n, SG_NAME[target]);
        sg_cat(l2, n, SG_NAME[answered]);
        sg_cat(l2, n, " IS ");
        sg_cat(l2, n, sg_steps_word((uint32_t)(d < 0 ? -d : d)));
        sg_cat(l2, n, d < 0 ? " TOO LOW" : " TOO HIGH");
    }
}

/* the next mistake most people make (the screen labels it NEXT TRAP): from the learner's own confusions when there are any, else from the interval */
static void sg_trap(const sg_store_t *s, uint32_t target, char *l, uint32_t n)
{
    int32_t c = sg_worst_confusion(s, target);
    uint32_t d;
    l[0] = 0;
    if (c >= 0) {
        sg_cat(l, n, "YOU MIX ");
        sg_cat(l, n, SG_NAME[target]);
        sg_cat(l, n, " WITH ");
        sg_cat(l, n, SG_NAME[c]);
        return;
    }
    d = target;                                         /* the distance from the reference C */
    switch (d) {
    case 0: sg_cat(l, n, "C SOUNDS LIKE HOME. TRUST IT"); break;
    case 1: case 11: sg_cat(l, n, "HALF STEPS FROM C RUB. HEAR IT"); break;
    case 2: case 10: sg_cat(l, n, "D AND A# HIDE NEAR C. COUNT"); break;
    case 3: case 4: sg_cat(l, n, "MAJOR 3RD BRIGHT, MINOR DARK"); break;
    case 5: sg_cat(l, n, "4TH VS 5TH: THE 5TH IS WIDER"); break;
    case 6: sg_cat(l, n, "THE TRITONE IS TENSE. IT LEANS"); break;
    case 7: sg_cat(l, n, "5TH VS 4TH: THE 4TH IS NARROWER"); break;
    default: sg_cat(l, n, "6THS: MAJOR BRIGHT, MINOR SAD"); break;
    }
}
#endif
