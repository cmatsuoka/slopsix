/*
 * slopsix_algorithm.c - The 32 algorithms' connection data and the
 * operator-graph walk.
 *
 * The connection data is the OPS algorithm ROM as read from the die
 * (research A-01): 96 nine-bit entries, six per algorithm,
 * each entry holding a three-bit modulation selector, the feedback and
 * sum-register control bits and a three-bit carrier-count compensation
 * code.
 *
 * The ROM's field alignment (the one resolved from the die readout) is:
 *
 *   - the entry in the column labelled operator N holds the modulation
 *     selector and the compensation code of operator N-1, with the
 *     column labelled operator 1 wrapping around to operator 6;
 *   - the same entry's control bits belong to operator N itself: the
 *     first one stores the operator's signal into the feedback pair, the
 *     other two are the sum/delay register's control bits.
 *
 * That alignment reproduces the carrier set of all 32 algorithms against
 * the service-manual chart and the feedback operator of all 32
 * algorithms against the analysed engine's feedback structure; the
 * harness checks both (32/32).
 *
 * The walk follows the die's register flow (the algorithm's 8-cycle
 * walkthrough in research A-01): while operator N is computed, its ROM
 * column's selector produces the modulation value operator N-1
 * consumes, and the column's C/D bits update the one sum/delay register
 * with operator N's signal (C keeps the register, D loads or adds the
 * signal); the A bit pushes the signal into the feedback pair.  The
 * pending modulation value survives into the next sample, where the
 * wrapped first row consumes it - that is the column-1 selector's part
 * (typically the self-feedback average) reaching operator 6.
 *
 * The feedback pair is pushed only where a row's A bit says so - the
 * reading operator's own signal joins the path only when its own row
 * carries the bit (it does in the self-feedback loops; in algorithms 4
 * and 6 the pair holds operator 4's resp. operator 5's values instead).
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O3); the rest of the library is compiled size-first.
 */
#include <string.h>

#include "slopsix_internal.h"

/* The entry's fields. */
#define SLOPSIX_ROM_SEL_MASK 0x07u
#define SLOPSIX_ROM_FB_OUT 0x08u
#define SLOPSIX_ROM_CTRL_C 0x10u
#define SLOPSIX_ROM_CTRL_D 0x20u
#define SLOPSIX_ROM_COM_SHIFT 6
#define SLOPSIX_ROM_COM_MASK 0x07u

/* The algorithm ROM, six entries per algorithm, the columns in the
 * hardware's order: operator 6, 5, 4, 3, 2, 1. */
static const uint16_t slopsix_algorithm_rom[32][6] = {
    { 0x009, 0x001, 0x041, 0x020, 0x051, 0x035 },
    { 0x001, 0x001, 0x041, 0x025, 0x059, 0x030 },
    { 0x009, 0x041, 0x020, 0x011, 0x051, 0x035 },
    { 0x001, 0x041, 0x028, 0x011, 0x051, 0x035 },
    { 0x089, 0x020, 0x091, 0x030, 0x091, 0x035 },
    { 0x081, 0x028, 0x091, 0x030, 0x091, 0x035 },
    { 0x009, 0x020, 0x072, 0x020, 0x051, 0x035 },
    { 0x001, 0x025, 0x07A, 0x020, 0x051, 0x030 },
    { 0x001, 0x020, 0x072, 0x025, 0x059, 0x030 },
    { 0x020, 0x072, 0x025, 0x019, 0x051, 0x030 },
    { 0x028, 0x072, 0x020, 0x011, 0x051, 0x035 },
    { 0x020, 0x030, 0x072, 0x025, 0x059, 0x030 },
    { 0x028, 0x030, 0x072, 0x020, 0x051, 0x035 },
    { 0x028, 0x032, 0x041, 0x020, 0x051, 0x035 },
    { 0x020, 0x032, 0x041, 0x025, 0x059, 0x030 },
    { 0x009, 0x020, 0x011, 0x030, 0x032, 0x025 },
    { 0x001, 0x020, 0x011, 0x035, 0x03A, 0x020 },
    { 0x001, 0x001, 0x025, 0x038, 0x032, 0x020 },
    { 0x089, 0x0A4, 0x030, 0x011, 0x091, 0x035 },
    { 0x020, 0x0B2, 0x025, 0x099, 0x0B4, 0x030 },
    { 0x0E1, 0x0E3, 0x035, 0x0D9, 0x0F4, 0x030 },
    { 0x0C9, 0x0E4, 0x0F4, 0x030, 0x0D1, 0x035 },
    { 0x0C9, 0x0E4, 0x030, 0x0D1, 0x0F0, 0x035 },
    { 0x109, 0x124, 0x134, 0x130, 0x130, 0x035 },
    { 0x109, 0x124, 0x130, 0x130, 0x130, 0x035 },
    { 0x028, 0x0B2, 0x020, 0x091, 0x0B0, 0x035 },
    { 0x020, 0x0B2, 0x025, 0x099, 0x0B0, 0x030 },
    { 0x025, 0x019, 0x091, 0x030, 0x091, 0x0B0 },
    { 0x0C9, 0x020, 0x0D1, 0x0F0, 0x0F0, 0x035 },
    { 0x025, 0x019, 0x0D1, 0x0F0, 0x0F0, 0x0F0 },
    { 0x109, 0x120, 0x130, 0x130, 0x130, 0x035 },
    { 0x168, 0x170, 0x170, 0x170, 0x170, 0x175 }
};

/* The three algorithms whose single carrier carries no compensation code
 * (the carrier count of one, so the code is zero); their carrier is
 * operator 1 (research A-01, the one-carrier caveat).  The indices run
 * from zero, so the algorithms are 16, 17 and 18. */
#define SLOPSIX_SINGLE_CARRIER_FIRST 15u
#define SLOPSIX_SINGLE_CARRIER_LAST  17u

/* Decode the ROM into the graph.  The decoding runs once per algorithm
 * and is cheap, but the result is kept in the algorithm table so that the
 * render path only walks the graph. */
static slopsix_algorithm slopsix_algorithms[32];
static bool slopsix_algorithms_ready;

/* The operator that carries the self-feedback path, 1..6, or zero. */
static uint8_t slopsix_feedback_operator[32];

static void slopsix_algorithm_decode(unsigned index)
{
    const uint16_t *rom = slopsix_algorithm_rom[index];
    slopsix_algorithm *alg = &slopsix_algorithms[index];
    unsigned column;
    unsigned op;
    unsigned i;

    slopsix_feedback_operator[index] = 0u;

    for (op = 1; op <= SLOPSIX_OP_COUNT; op++) {
        uint16_t entry;
        unsigned consumer;
        unsigned sel;
        unsigned com;

        /* The column labelled operator N carries the selector and the
         * compensation of operator N-1; the column labelled operator 1
         * wraps around to operator 6. */
        column = (op == SLOPSIX_OP_COUNT) ? 0u : (SLOPSIX_OP_COUNT - op);
        consumer = (op == 1u) ? SLOPSIX_OP_COUNT : (op - 1u);

        entry = rom[column];
        sel = entry & SLOPSIX_ROM_SEL_MASK;
        com = (entry >> SLOPSIX_ROM_COM_SHIFT) & SLOPSIX_ROM_COM_MASK;

        alg->ops[consumer - 1u].com = (uint8_t)com;
        alg->ops[consumer - 1u].com_q8 = slopsix_com_q8((uint8_t)com);
        alg->ops[consumer - 1u].carrier = (uint8_t)(com != 0u ? 1u : 0u);
        alg->ops[op - 1u].source = SLOPSIX_MOD_NONE;
        alg->ops[op - 1u].source_op = 0u;

        switch (sel) {
        case 0:
            break;
        case 1:
            /* The unmodified signal of the operator computed just before
             * in the walk: the higher-numbered neighbour.  Operator 6,
             * reachable through the wrap-around column, has no
             * higher-numbered neighbour; the checked-in ROM never carries
             * this selector there, and the graph would render it
             * unmodulated if the data ever did. */
            if (op != 1u) {
                alg->ops[op - 1u].source = SLOPSIX_MOD_OPERATOR;
                alg->ops[op - 1u].source_op = (uint8_t)op;
            }
            break;
        case 2:
            alg->ops[op - 1u].source = SLOPSIX_MOD_REGISTER;
            break;
        case 3:
            alg->ops[op - 1u].source = SLOPSIX_MOD_REGISTER_DELAYED;
            break;
        case 4:
            alg->ops[op - 1u].source = SLOPSIX_MOD_FEEDBACK_DELAYED;
            break;
        default:
            /* The self-feedback path: the walk's pending value becomes
             * the feedback pair's average. */
            alg->ops[op - 1u].source = SLOPSIX_MOD_FEEDBACK;
            slopsix_feedback_operator[index] = (uint8_t)consumer;
            break;
        }
    }

    /* The control bits belong to the operator the column is labelled
     * with: the first one stores the operator's signal into the
     * feedback pair, the other two are the sum/delay register's
     * control bits.  The ROM's columns run from operator 6 to operator
     * 1, so the column at index i is labelled with operator 6 - i. */
    for (i = 0; i < SLOPSIX_OP_COUNT; i++) {
        uint16_t entry = rom[i];
        unsigned op_number = SLOPSIX_OP_COUNT - i;

        alg->ops[op_number - 1u].ctrl_c =
            (uint8_t)((entry & SLOPSIX_ROM_CTRL_C) ? 1u : 0u);
        alg->ops[op_number - 1u].ctrl_d =
            (uint8_t)((entry & SLOPSIX_ROM_CTRL_D) ? 1u : 0u);
        alg->ops[op_number - 1u].fb_out =
            (uint8_t)((entry & SLOPSIX_ROM_FB_OUT) ? 1u : 0u);
    }

    /* The single-carrier algorithms carry no compensation code. */
    if (index >= SLOPSIX_SINGLE_CARRIER_FIRST &&
        index <= SLOPSIX_SINGLE_CARRIER_LAST) {
        alg->ops[0].carrier = 1u;
    }
}

/*
 * The block renderer's schedule analysis.
 *
 * The dependency structure the decode produces is (verified across the
 * 32 connection tables against the die's register flow):
 *
 *   - within a sample, data only flows from higher- to lower-numbered
 *     operators (the walk order 6 -> 1), so an operator-major pass in
 *     that order satisfies every same-sample dependency;
 *   - across samples, an operator's input reads either its own previous
 *     outputs (the feedback self-loop's pair value, lags one and two)
 *     or, in algorithms 4 and 6, the feedback pair that another
 *     operator's row pushes - those operators form one
 *     sample-interleaved run;
 *   - the sum/delay register crosses the sample boundary in algs. 24,
 *     25 and 31, but no source reads it there and operator 6's row
 *     never keeps it (C clear), so the register is a per-sample
 *     timeline with no ordering constraint;
 *   - the register timelines only matter where a source or a C bit
 *     reads them (the REG/REGD sources and the C bits of the 32 tables).
 *
 * The analysis walks the decoded rows and pins the schedule; whatever
 * it cannot pin leaves `fast` cleared and the walk renders the
 * algorithm.
 */
static void slopsix_schedule_analyze(slopsix_algorithm *alg)
{
    slopsix_schedule *sch = &alg->schedule;
    const slopsix_op_conn *c = alg->ops;
    int pusher = -1;        /* the feedback pusher's op index, 0-based */
    int pushers = 0;
    int pair_consumers = 0; /* the rows resolved onto the feedback pair */
    uint8_t live[SLOPSIX_OP_COUNT] = { 0 };
    unsigned i;

    memset(sch, 0, sizeof(*sch));
    sch->solo = 0xFFu;
    sch->fast = 1u;

    for (i = 0; i < (unsigned)SLOPSIX_OP_COUNT; i++) {
        sch->in_kind[i] = (uint8_t)SCHED_IN_ZERO;
        sch->in_src[i] = 0u;
        sch->msum_mode[i] = (uint8_t)SCHED_M_DEAD;
    }

    for (i = 0; i < (unsigned)SLOPSIX_OP_COUNT; i++) {
        if (c[i].fb_out) {
            pusher = (int)i;
            pushers++;
        }
    }

    if (pushers != 1) {
        sch->fast = 0u;
        return;
    }

    /* The boundary reads.  Operator 6's row is the sample's first, so a
     * C bit there keeps the previous sample's final register value, and
     * the row-1 / row-6 register sources read the register's previous
     * sample state; the pair read in row 1 with the FBD selector reads
     * its previous sample value too.  The checked-in ROM performs none
     * of them; the walk covers them if a graph ever does. */
    if (c[SLOPSIX_OP_COUNT - 1u].ctrl_c ||
        c[0].source == SLOPSIX_MOD_REGISTER ||
        c[0].source == SLOPSIX_MOD_REGISTER_DELAYED ||
        c[0].source == SLOPSIX_MOD_FEEDBACK_DELAYED ||
        c[SLOPSIX_OP_COUNT - 1u].source ==
            SLOPSIX_MOD_REGISTER_DELAYED) {
        sch->fast = 0u;
        return;
    }

    /* Rows 2..6 (connection indices 1..5).  Connection index i is op
     * i+1's row; the modout it produces feeds op i, so the consumer's
     * index is i-1. */
    for (i = 1; i < (unsigned)SLOPSIX_OP_COUNT; i++) {
        unsigned consumer = i - 1u;

        switch (c[i].source) {
        case SLOPSIX_MOD_NONE:
            break;

        case SLOPSIX_MOD_OPERATOR:
            /* The row's own signal: op i+1's output, buffer index i. */
            sch->in_kind[consumer] = (uint8_t)SCHED_IN_OBUF;
            sch->in_src[consumer] = (uint8_t)i;
            break;

        case SLOPSIX_MOD_REGISTER:
            /* The row's own sum/delay value: op i+1's timeline. */
            sch->in_kind[consumer] = (uint8_t)SCHED_IN_MSUM;
            sch->in_src[consumer] = (uint8_t)i;
            break;

        case SLOPSIX_MOD_REGISTER_DELAYED:
            /* The register's value before the row's update: op i+2's
             * timeline. */
            sch->in_kind[consumer] = (uint8_t)SCHED_IN_MSUM;
            sch->in_src[consumer] = (uint8_t)(i + 1u);
            break;

        case SLOPSIX_MOD_FEEDBACK_DELAYED:
            if (pusher > (int)i) {
                /* The pair's last pushed value, which the pusher's row
                 * (higher than this one) has already rewritten this
                 * sample: the pusher's own output. */
                sch->in_kind[consumer] = (uint8_t)SCHED_IN_OBUF;
                sch->in_src[consumer] = (uint8_t)pusher;
            } else if (pusher == (int)i - 1) {
                /* The pusher is the consumer's own row: the pair holds
                 * the consumer's previous sample (a lag-one
                 * self-loop). */
                pair_consumers++;
                sch->in_kind[consumer] = (uint8_t)SCHED_IN_PAIR_LAG1;
                sch->solo = (uint8_t)consumer;
            } else {
                /* The pair's value from another operator, one sample
                 * behind: the walk's order of work. */
                sch->fast = 0u;
                return;
            }
            break;

        case SLOPSIX_MOD_FEEDBACK:
            if (pusher == (int)i - 1) {
                /* Self-feedback: the consumption reads the consumer's
                 * own previous two samples (lags one and two). */
                pair_consumers++;
                sch->in_kind[consumer] = (uint8_t)SCHED_IN_PAIR_AVG;
                sch->solo = (uint8_t)consumer;
            } else {
                /* The pusher's row is above or at this row: the read
                 * carries one sample of another operator, or the row's
                 * own previous sample - the walk's order of work
                 * covers those only when row 1 consumes the pair
                 * (below). */
                sch->fast = 0u;
                return;
            }
            break;

        default:
            sch->fast = 0u;
            return;
        }
    }

    /* Row 1 (connection index 0): its modout is op 6's input of the
     * next sample - the wrap the self-feedback loops travel through. */
    switch (c[0].source) {
    case SLOPSIX_MOD_NONE:
        break;

    case SLOPSIX_MOD_OPERATOR:
        /* The decode never places this selector in column 1 (research
         * A-01); a graph that does falls back to the walk. */
        sch->fast = 0u;
        return;

    case SLOPSIX_MOD_FEEDBACK:
        if (pusher == SLOPSIX_OP_COUNT - 1) {
            /* The pusher is operator 6's own row: the self-feedback
             * loop on op 6. */
            pair_consumers++;
            sch->in_kind[SLOPSIX_OP_COUNT - 1u] = (uint8_t)SCHED_IN_PAIR_AVG;
            sch->solo = (uint8_t)(SLOPSIX_OP_COUNT - 1u);
        } else {
            /* Algorithm 4's and 6's shape: op 6 reads the pair that op
             * 5 or op 4 pushes, one and two samples behind, while
             * those operators read op 6 within the sample - the three
             * (or two) operators form one sample-interleaved run. */
            unsigned lo = (unsigned)pusher;
            uint8_t member[SLOPSIX_OP_COUNT] = { 0 };
            unsigned changed;
            unsigned lowest;

            if (pusher < 3 || pushers != 1) {
                sch->fast = 0u;
                return;
            }

            pair_consumers++;
            sch->in_kind[SLOPSIX_OP_COUNT - 1u] = (uint8_t)SCHED_IN_PAIR_AVG;

            /* The run's members: op 6, the pusher, and everything
             * between them that their same-sample inputs pass
             * through. */
            member[SLOPSIX_OP_COUNT - 1u] = 1u;
            member[lo] = 1u;
            do {
                changed = 0u;
                for (i = SLOPSIX_OP_COUNT - 1u; i >= 1u; i--) {
                    unsigned k;

                    if (!member[i] || i + 1u >= (unsigned)SLOPSIX_OP_COUNT) {
                        continue;
                    }
                    k = i + 1u;    /* member i's source sits on op k+1's
                                    * row (connection index k); the row's
                                    * own signal and register value are
                                    * op k+1's = index k */
                    switch (c[k].source) {
                    case SLOPSIX_MOD_OPERATOR:
                        k = c[k].source_op ? (unsigned)c[k].source_op - 1u
                                           : k;
                        break;
                    case SLOPSIX_MOD_REGISTER_DELAYED:
                        k++;
                        break;
                    case SLOPSIX_MOD_FEEDBACK_DELAYED:
                        k = (unsigned)pusher;
                        break;
                    default:
                        /* NONE (and the pair read) add nothing; the
                         * REG source reads the row's own timeline,
                         * which k already names. */
                        k = i;
                        break;
                    }
                    if (!member[k]) {
                        member[k] = 1u;
                        changed = 1u;
                    }
                }
            } while (changed);

            /* The members must be one contiguous run of the walk
             * sequence, pushed last (the pusher is the run's lowest
             * operator), and the run is two or three operators wide;
             * op 6 is the run's only pair consumer. */
            lowest = SLOPSIX_OP_COUNT - 1u;
            while (lowest >= 1u && member[lowest - 1u]) {
                lowest--;
            }
            for (i = 0; i < (unsigned)SLOPSIX_OP_COUNT; i++) {
                if (member[i] != (uint8_t)(i >= lowest)) {
                    sch->fast = 0u;
                    return;
                }
            }
            if (lowest != lo || lowest < SLOPSIX_OP_COUNT - 3u ||
                sch->solo != 0xFFu || pair_consumers != 1) {
                sch->fast = 0u;
                return;
            }
            sch->group = (uint8_t)(SLOPSIX_OP_COUNT - lowest);
        }
        break;

    default:
        sch->fast = 0u;
        return;
    }

    if (pair_consumers != 1) {
        sch->fast = 0u;
        return;
    }

    /* The pusher's own row has to be inside the run that maintains the
     * pair: the solo run is the pair's consumer (and the pusher) and
     * the group run has the pusher as its last member.  A schedule
     * without either would leave the pair's pushes out of the block
     * loop, so the walk renders those graphs. */
    if (sch->solo == 0xFFu && sch->group == 0u) {
        sch->fast = 0u;
        return;
    }

    /* The sum/delay timelines: a REG source reads the row's own
     * timeline value, a REGD source the one of the row above, and a C
     * bit the value the row above has just written.  Everything else
     * the register carries is dead work the walk does for its final
     * state; the renderer reproduces that state once per call instead
     * (the epilogue in the block render). */
    for (i = 1; i < (unsigned)SLOPSIX_OP_COUNT; i++) {
        if (c[i].source == SLOPSIX_MOD_REGISTER) {
            live[i] = 1u;
        }
    }
    for (i = 1; i + 1u < (unsigned)SLOPSIX_OP_COUNT; i++) {
        if (c[i].source == SLOPSIX_MOD_REGISTER_DELAYED) {
            live[i + 1u] = 1u;
        }
    }
    for (i = 0; i + 1u < (unsigned)SLOPSIX_OP_COUNT; i++) {
        if (c[i].ctrl_c) {
            live[i + 1u] = 1u;
        }
    }

    for (i = 0; i < (unsigned)SLOPSIX_OP_COUNT; i++) {
        uint8_t keep = (i + 1u < (unsigned)SLOPSIX_OP_COUNT)
                           ? c[i].ctrl_c : 0u;
        uint8_t load = c[i].ctrl_d;

        if (!live[i]) {
            sch->msum_mode[i] = (uint8_t)SCHED_M_DEAD;
        } else if (keep && load) {
            sch->msum_mode[i] = (uint8_t)SCHED_M_PREV_OUT;
        } else if (keep) {
            sch->msum_mode[i] = (uint8_t)SCHED_M_PREV;
        } else if (load) {
            sch->msum_mode[i] = (uint8_t)SCHED_M_OUT;
        } else {
            sch->msum_mode[i] = (uint8_t)SCHED_M_ZERO;
        }
    }
}

static void slopsix_algorithms_init(void)
{
    unsigned i;

    if (slopsix_algorithms_ready) {
        return;
    }

    for (i = 0; i < 32u; i++) {
        slopsix_algorithm_decode(i);
        slopsix_schedule_analyze(&slopsix_algorithms[i]);
    }

    slopsix_algorithms_ready = true;
}

/* The algorithm's connection data. */
const slopsix_algorithm *slopsix_algorithm_get(unsigned index)
{
    if (index >= 32u) {
        index = 0u;
    }
    slopsix_algorithms_init();
    return &slopsix_algorithms[index];
}

/* The operator that carries the self-feedback path, 1..6, or zero. */
uint8_t slopsix_algorithm_feedback_operator(unsigned index)
{
    if (index >= 32u) {
        index = 0u;
    }
    slopsix_algorithms_init();
    return slopsix_feedback_operator[index];
}

/*
 * The graph walk (the die's register flow, research A-01).
 *
 * The operators run in the hardware's order op 6 -> op 1.  As each row's
 * operator is computed, the row's selector produces the modulation value
 * the next row's operator consumes - the pending register `modout`; the
 * row's C/D bits update the sum/delay register with that operator's own
 * signal (`msum = (C ? mren : 0) + (D ? signal : 0)`, so C keeps the
 * register and D loads it or adds to it), and its A bit pushes the
 * signal into the feedback pair.
 *
 * `modout` survives the sample: the last processed row (operator 1's,
 * whose ROM column is labelled operator 1) writes the value operator 6
 * consumes at the next sample's first row - the wrap that the
 * self-feedback loops' pending average travels through.  The carriers
 * sum into the voice output; the compensation (COM) applies to those
 * operators' levels only, as the sum/delay register's final content is
 * exactly the carrier sum of every checked-in algorithm.
 */
void slopsix_voice_render_sample(slopsix_voice_engine *voice,
                                  const slopsix_algorithm *alg,
                                  int32_t *voice_out)
{
    int32_t voice_sum = 0;
    unsigned row;

    for (row = 0; row < SLOPSIX_OP_COUNT; row++) {
        /* The ROM row index: row 0 is operator 6, row 5 operator 1;
         * the connection's index runs the other way. */
        unsigned index = SLOPSIX_OP_COUNT - 1u - row;
        const slopsix_op_conn *conn = &alg->ops[index];
        int32_t msum;
        int32_t sample;
        int32_t log_amp_q8;
        int32_t log_amp_q10;

        /* The envelope: the operator's log amplitude is the envelope's
         * level, less the carrier-count compensation the algorithm
         * applies to this operator (Phase 3).  The compensation is zero
         * for a modulator, because the hardware applies it to the
         * carriers only. */
        if (voice->eg[index].active) {
            log_amp_q8 = slopsix_eg_step(&voice->eg[index]);
        } else {
            log_amp_q8 =
                voice->params[index].log_amp_q10 >> SLOPSIX_AMP_Q8_SHIFT;
        }

        log_amp_q8 += conn->com_q8;

        /* The amplitude modulation lowers the level of the operators
         * that are sensitive to it; it never raises it.  The register's
         * value arrives from the context's per-tick composition (the
         * LFO's push); the reduction it causes is composed there and
         * cached in the voice's operator parameters, and the shift is
         * the whole depth law. */
        log_amp_q8 -= voice->params[index].ams_reduction_q8;

        if (log_amp_q8 < 0) {
            /* The compensation can mute a carrier at full level. */
            log_amp_q8 = 0;
        }

        log_amp_q10 = log_amp_q8 << SLOPSIX_AMP_Q8_SHIFT;

        /* The operator's modulation is the value the previous row's
         * selector produced - operator 6's arrives from the previous
         * sample's last row (the wrap). */
        slopsix_op_render(&voice->ops[index], log_amp_q10, voice->modout,
            &sample);

        /* The sum/delay register's update: C keeps its content, D loads
         * or adds this operator's signal. */
        msum = (conn->ctrl_c ? voice->mren : 0) +
               (conn->ctrl_d ? sample : 0);

        /* The selector feeds the next row.  The register's and the
         * feedback pair's reads happen before the updates below, as the
         * die's own window does. */
        switch (conn->source) {
        case SLOPSIX_MOD_OPERATOR:
            voice->modout = sample;
            break;
        case SLOPSIX_MOD_REGISTER:
            voice->modout = msum;
            break;
        case SLOPSIX_MOD_REGISTER_DELAYED:
            voice->modout = voice->mren;
            break;
        case SLOPSIX_MOD_FEEDBACK_DELAYED:
            voice->modout = voice->fren1;
            break;
        case SLOPSIX_MOD_FEEDBACK:
            voice->modout = slopsix_feedback_scaled(voice->fren1,
                voice->fren2, voice->feedback_level);
            break;
        default:
            voice->modout = 0;
            break;
        }

        voice->mren = msum;

        if (conn->fb_out) {
            voice->fren2 = voice->fren1;
            voice->fren1 = sample;
        }

        if (conn->carrier) {
            voice_sum += sample;
        }
    }

    *voice_out = voice_sum;
}

/* ---------------------------------------------------- the block renderer */

/*
 * The per-sample walk's evaluation order, reorganized per algorithm
 * schedule: one operator renders a whole block of samples before the
 * next one starts, the envelopes fill one level buffer per operator,
 * and every routing decision the walk takes per sample is resolved
 * once per operator and block.  The same arithmetic runs per sample,
 * in the same order - the schedule only changes which computation
 * happens when - so the rendered samples and the engine state match
 * the walk bit for bit.
 *
 * Operators render in the walk order 6 -> 1 (connection indices 5 to
 * 0).  Three run shapes cover the checked-in connection tables:
 *
 *   - a tight run: the operator reads a same-sample buffer an earlier
 *     operator has filled (or silence) and renders the whole block in
 *     one loop;
 *   - the solo run: the feedback operator whose input is its own two
 *     previous samples; it renders a per-sample loop, as the
 *     reference's feedback does, with the pair in registers;
 *   - the group run: algorithms 4 and 6, where the feedback pair is
 *     pushed by the run's last operator and read by operator 6 one
 *     iteration later - the members render one sample at a time, in
 *     the walk's order, until the block is done.
 */

/* The level buffers: each operator's log amplitude for the whole
 * sub-block, the envelope's Q8 level (or the parameter level for an
 * idle envelope), before the compensation and the amplitude-modulation
 * reduction are applied. */
static void block_levels(slopsix_voice_engine *voice,
                         slopsix_block_scratch *scratch, unsigned count)
{
    unsigned i;
    unsigned s;

    for (i = 0; i < (unsigned)SLOPSIX_OP_COUNT; i++) {
        int16_t *restrict out = scratch->level[i];

        if (voice->eg[i].active) {
            slopsix_eg_fill(&voice->eg[i], out, count);
            continue;
        }

        /* A voice without a live envelope keeps the level the
         * parameters carry; the sample loop sees the same value every
         * sample. */
        int16_t value =
            (int16_t)(voice->params[i].log_amp_q10 >> SLOPSIX_AMP_Q8_SHIFT);
        for (s = 0; s < count; s++) {
            out[s] = value;
        }
    }
}

/* The level's composed word: the buffer value, the carrier-count
 * compensation and the amplitude-modulation reduction, floor-clamped
 * at the envelope's silence level, shifted into the log word's Q10
 * scale - the walk's own composition. */
static inline int32_t block_log_amp(const int16_t *level, int32_t base,
                                    unsigned s)
{
    int32_t log_amp_q8 = (int32_t)level[s] + base;

    if (log_amp_q8 < 0) {
        /* The compensation can mute a carrier at full level. */
        log_amp_q8 = 0;
    }

    return log_amp_q8 << SLOPSIX_AMP_Q8_SHIFT;
}

/* The sum/delay timeline value for one sample from the row's C/D bits. */
static inline int32_t block_msum(uint8_t mode, const int32_t *restrict prev,
                                 int32_t sample, unsigned s)
{
    int32_t msum = 0;

    if (mode == (uint8_t)SCHED_M_PREV || mode == (uint8_t)SCHED_M_PREV_OUT) {
        msum = prev[s];
    }
    if (mode == (uint8_t)SCHED_M_OUT || mode == (uint8_t)SCHED_M_PREV_OUT) {
        msum += sample;
    }

    return msum;
}

/* A tight run: the operator reads its input from a buffer an earlier
 * operator has filled. */
static void block_tight(slopsix_voice_engine *voice,
                        const slopsix_algorithm *alg,
                        const slopsix_schedule *sch,
                        slopsix_block_scratch *scratch, unsigned index,
                        unsigned count)
{
    const slopsix_op_conn *conn = &alg->ops[index];
    int32_t *restrict out = scratch->opout[index];
    const int16_t *restrict level = scratch->level[index];
    int32_t base = conn->com_q8 - voice->params[index].ams_reduction_q8;
    const int32_t *restrict in = NULL;
    int32_t *restrict mbuf = scratch->msum[index];
    const int32_t *restrict prev = (index + 1u < (unsigned)SLOPSIX_OP_COUNT)
                                       ? scratch->msum[index + 1u]
                                       : NULL;
    uint8_t mode = sch->msum_mode[index];
    uint32_t acc = voice->ops[index].phase;
    uint32_t increment = voice->ops[index].increment;
    unsigned s;

    switch (sch->in_kind[index]) {
    case SCHED_IN_OBUF:
        in = scratch->opout[sch->in_src[index]];
        break;
    case SCHED_IN_MSUM:
        in = scratch->msum[sch->in_src[index]];
        break;
    default:
        break;
    }

    if (index == SLOPSIX_OP_COUNT - 1u) {
        /* Operator 6's row is the sample's first and reads the pending
         * modulation value carried across the sample boundary: the
         * block's first sample is the voice's field, the rest are the
         * row-1 selector's zero (a fast schedule's row-1 selector is
         * the feedback pair - a solo or group run - or silence). */
        int32_t *restrict mwrap = scratch->msum[index];

        for (s = 0; s < count; s++) {
            int32_t sample;
            int32_t mod_in = (s == 0u) ? voice->modout : 0;

            slopsix_op_stage(acc, increment,
                             block_log_amp(level, base, s), mod_in,
                             &acc, &sample);
            out[s] = sample;
            if (mode != (uint8_t)SCHED_M_DEAD) {
                mwrap[s] = block_msum(mode, prev, sample, s);
            }
        }
    } else if (in == NULL) {
        for (s = 0; s < count; s++) {
            int32_t sample;

            slopsix_op_stage(acc, increment,
                             block_log_amp(level, base, s), 0,
                             &acc, &sample);
            out[s] = sample;
            if (mode != (uint8_t)SCHED_M_DEAD) {
                mbuf[s] = block_msum(mode, prev, sample, s);
            }
        }
    } else {
        for (s = 0; s < count; s++) {
            int32_t sample;

            slopsix_op_stage(acc, increment,
                             block_log_amp(level, base, s), in[s],
                             &acc, &sample);
            out[s] = sample;
            if (mode != (uint8_t)SCHED_M_DEAD) {
                mbuf[s] = block_msum(mode, prev, sample, s);
            }
        }
    }

    voice->ops[index].phase = acc;
}

/* The solo run: the feedback operator's input is its own previous two
 * samples (the pair's scaled average) or its previous sample (a
 * lag-one self-loop); the loop keeps the pair and the phase in
 * registers and pushes the pair after every sample, as the walk's row
 * does. */
static void block_solo(slopsix_voice_engine *voice,
                       const slopsix_algorithm *alg,
                       const slopsix_schedule *sch,
                       slopsix_block_scratch *scratch, unsigned index,
                       unsigned count)
{
    const slopsix_op_conn *conn = &alg->ops[index];
    int32_t *restrict out = scratch->opout[index];
    const int16_t *restrict level = scratch->level[index];
    int32_t base = conn->com_q8 - voice->params[index].ams_reduction_q8;
    int32_t *restrict mbuf = scratch->msum[index];
    const int32_t *restrict prev = (index + 1u < (unsigned)SLOPSIX_OP_COUNT)
                                       ? scratch->msum[index + 1u]
                                       : NULL;
    uint8_t mode = sch->msum_mode[index];
    int32_t fren1 = voice->fren1;
    int32_t fren2 = voice->fren2;
    uint8_t feedback = voice->feedback_level;
    uint32_t acc = voice->ops[index].phase;
    uint32_t increment = voice->ops[index].increment;
    unsigned s;

    for (s = 0; s < count; s++) {
        int32_t mod_in;
        int32_t sample;

        /* Operator 6's row reads the pending modulation value carried
         * across the sample boundary: the wrap consumer's first sample
         * of the block is the voice's field, and its later samples are
         * the row-1 selector's pair value one sample back - the solo
         * loop's own pair, pushed one iteration earlier.  A self-loop
         * on a lower operator consumes the pair it carries itself,
         * starting from the carried pair on the block's first sample. */
        if (s == 0u && index == SLOPSIX_OP_COUNT - 1u) {
            mod_in = voice->modout;
        } else if (sch->in_kind[index] == (uint8_t)SCHED_IN_PAIR_LAG1) {
            mod_in = fren1;
        } else {
            mod_in = slopsix_feedback_scaled(fren1, fren2, feedback);
        }

        slopsix_op_stage(acc, increment, block_log_amp(level, base, s),
                         mod_in, &acc, &sample);
        out[s] = sample;

        /* The row's own feedback-store bit pushes its signal into the
         * pair for the next read: the pair follows the walk's state. */
        fren2 = fren1;
        fren1 = sample;

        if (mode != (uint8_t)SCHED_M_DEAD) {
            mbuf[s] = block_msum(mode, prev, sample, s);
        }
    }

    voice->ops[index].phase = acc;
    voice->fren1 = fren1;
    voice->fren2 = fren2;
}

/* The group run, two members: op 6 reads the pair and writes its
 * sample for op 5, whose row pushes the pair for the next iteration -
 * algorithms 6's shape.  The members render one sample at a time, in
 * the walk's order. */
static void block_group2(slopsix_voice_engine *voice,
                         const slopsix_algorithm *alg,
                         const slopsix_schedule *sch,
                         slopsix_block_scratch *scratch, unsigned count)
{
    int32_t *restrict out6 = scratch->opout[5];
    int32_t *restrict out5 = scratch->opout[4];
    int32_t *restrict mbuf6 = scratch->msum[5];
    int32_t *restrict mbuf5 = scratch->msum[4];
    const int16_t *restrict level6 = scratch->level[5];
    const int16_t *restrict level5 = scratch->level[4];
    int32_t base6 = alg->ops[5].com_q8 - voice->params[5].ams_reduction_q8;
    int32_t base5 = alg->ops[4].com_q8 - voice->params[4].ams_reduction_q8;
    /* The members' timeline modes: op 6's row is the sample's first, so
     * its mode carries no C bit. */
    uint8_t mode6 = sch->msum_mode[5];
    uint8_t mode5 = sch->msum_mode[4];
    uint32_t acc6 = voice->ops[5].phase;
    uint32_t acc5 = voice->ops[4].phase;
    uint32_t increment6 = voice->ops[5].increment;
    uint32_t increment5 = voice->ops[4].increment;
    int32_t fren1 = voice->fren1;
    int32_t fren2 = voice->fren2;
    uint8_t feedback = voice->feedback_level;
    /* Op 5's input, resolved from its row-6 source: op 6's own sample
     * of the iteration (the OP selector), op 6's timeline value (the
     * REG selector) or silence. */
    uint8_t in5 = sch->in_kind[4];
    uint8_t use6 = (in5 == (uint8_t)SCHED_IN_OBUF);
    uint8_t use6m = (in5 == (uint8_t)SCHED_IN_MSUM);
    unsigned s;

    for (s = 0; s < count; s++) {
        int32_t sample6;
        int32_t sample5;
        int32_t m6 = 0;

        slopsix_op_stage(acc6, increment6,
                         block_log_amp(level6, base6, s),
                         (s == 0u) ? voice->modout
                                   : slopsix_feedback_scaled(fren1, fren2,
                                                             feedback),
                         &acc6, &sample6);
        out6[s] = sample6;
        if (mode6 == (uint8_t)SCHED_M_OUT) {
            m6 = sample6;
        }
        if (mode6 != (uint8_t)SCHED_M_DEAD) {
            mbuf6[s] = m6;
        }

        slopsix_op_stage(acc5, increment5,
                         block_log_amp(level5, base5, s),
                         use6 ? sample6 : (use6m ? m6 : 0),
                         &acc5, &sample5);
        out5[s] = sample5;

        if (mode5 != (uint8_t)SCHED_M_DEAD) {
            /* C keeps the register op 6's row has just written; D
             * loads the signal. */
            int32_t m5 = 0;

            if (mode5 == (uint8_t)SCHED_M_PREV ||
                mode5 == (uint8_t)SCHED_M_PREV_OUT) {
                m5 = m6;
            }
            if (mode5 == (uint8_t)SCHED_M_OUT ||
                mode5 == (uint8_t)SCHED_M_PREV_OUT) {
                m5 += sample5;
            }
            mbuf5[s] = m5;
        }

        /* The pusher is the run's last member: the pair carries this
         * sample into the next iteration's read. */
        fren2 = fren1;
        fren1 = sample5;
    }

    voice->ops[5].phase = acc6;
    voice->ops[4].phase = acc5;
    voice->fren1 = fren1;
    voice->fren2 = fren2;
}

/* The group run, three members: algorithms 4's shape - op 6 reads the
 * pair, op 5 reads op 6's sample, op 4 reads op 5's and pushes the
 * pair. */
static void block_group3(slopsix_voice_engine *voice,
                         const slopsix_algorithm *alg,
                         const slopsix_schedule *sch,
                         slopsix_block_scratch *scratch, unsigned count)
{
    int32_t *restrict out6 = scratch->opout[5];
    int32_t *restrict out5 = scratch->opout[4];
    int32_t *restrict out4 = scratch->opout[3];
    int32_t *restrict mbuf6 = scratch->msum[5];
    int32_t *restrict mbuf5 = scratch->msum[4];
    int32_t *restrict mbuf4 = scratch->msum[3];
    const int16_t *restrict level6 = scratch->level[5];
    const int16_t *restrict level5 = scratch->level[4];
    const int16_t *restrict level4 = scratch->level[3];
    int32_t base6 = alg->ops[5].com_q8 - voice->params[5].ams_reduction_q8;
    int32_t base5 = alg->ops[4].com_q8 - voice->params[4].ams_reduction_q8;
    int32_t base4 = alg->ops[3].com_q8 - voice->params[3].ams_reduction_q8;
    uint8_t mode6 = sch->msum_mode[5];
    uint8_t mode5 = sch->msum_mode[4];
    uint8_t mode4 = sch->msum_mode[3];
    uint32_t acc6 = voice->ops[5].phase;
    uint32_t acc5 = voice->ops[4].phase;
    uint32_t acc4 = voice->ops[3].phase;
    uint32_t increment6 = voice->ops[5].increment;
    uint32_t increment5 = voice->ops[4].increment;
    uint32_t increment4 = voice->ops[3].increment;
    int32_t fren1 = voice->fren1;
    int32_t fren2 = voice->fren2;
    uint8_t feedback = voice->feedback_level;
    /* Op 5's input, from row 6's source (op 6's own sample or op 6's
     * timeline value); op 4's input, from row 5's source (op 5's own
     * sample, op 5's timeline value, or - the REGD selector - op 6's
     * timeline value one row up). */
    uint8_t in5 = sch->in_kind[4];
    uint8_t in4 = sch->in_kind[3];
    uint8_t f56 = (in5 == (uint8_t)SCHED_IN_OBUF);
    uint8_t f56m = (in5 == (uint8_t)SCHED_IN_MSUM);
    uint8_t f45 = (in4 == (uint8_t)SCHED_IN_OBUF);
    uint8_t f45m = (in4 == (uint8_t)SCHED_IN_MSUM);
    uint8_t f46m = f45m && sch->in_src[3] == 5u;
    unsigned s;

    for (s = 0; s < count; s++) {
        int32_t sample6;
        int32_t sample5;
        int32_t sample4;
        int32_t m6 = 0;
        int32_t m5 = 0;

        slopsix_op_stage(acc6, increment6,
                         block_log_amp(level6, base6, s),
                         (s == 0u) ? voice->modout
                                   : slopsix_feedback_scaled(fren1, fren2,
                                                             feedback),
                         &acc6, &sample6);
        out6[s] = sample6;
        if (mode6 == (uint8_t)SCHED_M_OUT) {
            m6 = sample6;
        }
        if (mode6 != (uint8_t)SCHED_M_DEAD) {
            mbuf6[s] = m6;
        }

        slopsix_op_stage(acc5, increment5,
                         block_log_amp(level5, base5, s),
                         f56 ? sample6 : (f56m ? m6 : 0),
                         &acc5, &sample5);
        out5[s] = sample5;
        if (mode5 != (uint8_t)SCHED_M_DEAD) {
            if (mode5 == (uint8_t)SCHED_M_PREV ||
                mode5 == (uint8_t)SCHED_M_PREV_OUT) {
                m5 = m6;
            }
            if (mode5 == (uint8_t)SCHED_M_OUT ||
                mode5 == (uint8_t)SCHED_M_PREV_OUT) {
                m5 += sample5;
            }
            mbuf5[s] = m5;
        }

        slopsix_op_stage(acc4, increment4,
                         block_log_amp(level4, base4, s),
                         f45 ? sample5 : (f45m ? (f46m ? m6 : m5) : 0),
                         &acc4, &sample4);
        out4[s] = sample4;
        if (mode4 != (uint8_t)SCHED_M_DEAD) {
            int32_t m4 = 0;

            if (mode4 == (uint8_t)SCHED_M_PREV ||
                mode4 == (uint8_t)SCHED_M_PREV_OUT) {
                m4 = m5;
            }
            if (mode4 == (uint8_t)SCHED_M_OUT ||
                mode4 == (uint8_t)SCHED_M_PREV_OUT) {
                m4 += sample4;
            }
            mbuf4[s] = m4;
        }

        /* The pusher is the run's last member: the pair carries this
         * sample into the next iteration's read. */
        fren2 = fren1;
        fren1 = sample4;
    }

    voice->ops[5].phase = acc6;
    voice->ops[4].phase = acc5;
    voice->ops[3].phase = acc4;
    voice->fren1 = fren1;
    voice->fren2 = fren2;
}

/* The carrier sum: the carriers' rendered samples, as the walk adds
 * them row by row. */
static void block_carriers(const slopsix_algorithm *alg,
                           slopsix_block_scratch *scratch,
                           int32_t *restrict voice_out, unsigned count)
{
    unsigned i;
    unsigned s;

    for (s = 0; s < count; s++) {
        voice_out[s] = 0;
    }
    for (i = 0; i < (unsigned)SLOPSIX_OP_COUNT; i++) {
        const int32_t *restrict out = scratch->opout[i];

        if (!alg->ops[i].carrier) {
            continue;
        }
        for (s = 0; s < count; s++) {
            voice_out[s] += out[s];
        }
    }
}

/* The engine state the walk leaves behind the register chain: the
 * walk's sum/delay register crosses the sample boundary as the value
 * after operator 1's row, and the block renderer reproduces it from
 * the last sample's operator outputs once per sub-block.  Operator 6's
 * row never keeps the register in a fast schedule (the analysis
 * rejects a C bit there), so the chain starts at zero. */
static void block_state(slopsix_voice_engine *voice,
                        const slopsix_algorithm *alg,
                        slopsix_block_scratch *scratch, unsigned count)
{
    int32_t msum = 0;
    unsigned i;

    for (i = SLOPSIX_OP_COUNT - 1u;; i--) {
        msum = alg->ops[i].ctrl_c ? msum : 0;
        if (alg->ops[i].ctrl_d) {
            msum += scratch->opout[i][count - 1u];
        }
        if (i == 0u) {
            break;
        }
    }

    voice->mren = msum;

    /* The pending modulation value the walk's last row writes: op 6's
     * input of the next sample.  The fast schedules' row-1 selectors
     * are the feedback pair (which the solo and group runs have left
     * at the walk's final state) or silence. */
    switch (alg->ops[0].source) {
    case SLOPSIX_MOD_FEEDBACK:
        voice->modout = slopsix_feedback_scaled(voice->fren1, voice->fren2,
            voice->feedback_level);
        break;
    default:
        voice->modout = 0;
        break;
    }
}

void slopsix_voice_render_block(slopsix_voice_engine *voice,
                                 const slopsix_algorithm *alg,
                                 slopsix_block_scratch *scratch,
                                 int32_t *restrict voice_out, unsigned count)
{
    const slopsix_schedule *sch;

    if (count == 0u) {
        return;
    }

    sch = &alg->schedule;

    if (!sch->fast) {
        /* The analysis could not pin the graph: the walk renders it,
         * exactly as the extension personalities do until they get
         * schedules of their own. */
        unsigned s;

        for (s = 0; s < count; s++) {
            slopsix_voice_render_sample(voice, alg, &voice_out[s]);
        }
        return;
    }

    while (count > SLOPSIX_RENDER_BLOCK) {
        /* Sub-blocks longer than one block render as consecutive
         * blocks; the voice state carries between them as it does
         * between render calls. */
        slopsix_voice_render_block(voice, alg, scratch, voice_out,
                                   SLOPSIX_RENDER_BLOCK);
        voice_out += SLOPSIX_RENDER_BLOCK;
        count -= SLOPSIX_RENDER_BLOCK;
    }

    block_levels(voice, scratch, count);

    if (sch->group != 0u) {
        if (sch->group == 2u) {
            block_group2(voice, alg, sch, scratch, count);
        } else {
            block_group3(voice, alg, sch, scratch, count);
        }
    } else if (sch->solo == SLOPSIX_OP_COUNT - 1u) {
        block_solo(voice, alg, sch, scratch, SLOPSIX_OP_COUNT - 1u, count);
    } else if (sch->solo != 0xFFu) {
        block_tight(voice, alg, sch, scratch, SLOPSIX_OP_COUNT - 1u, count);
    }

    {
        unsigned lo = (sch->group != 0u)
                          ? (unsigned)(SLOPSIX_OP_COUNT - sch->group)
                          : SLOPSIX_OP_COUNT - 1u;
        unsigned index;

        for (index = lo - 1u;; index--) {
            if (index == sch->solo) {
                block_solo(voice, alg, sch, scratch, index, count);
            } else {
                block_tight(voice, alg, sch, scratch, index, count);
            }
            if (index == 0u) {
                break;
            }
        }
    }

    block_carriers(alg, scratch, voice_out, count);
    block_state(voice, alg, scratch, count);
}
