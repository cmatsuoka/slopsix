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

static void slopsix_algorithms_init(void)
{
    unsigned i;

    if (slopsix_algorithms_ready) {
        return;
    }

    for (i = 0; i < 32u; i++) {
        slopsix_algorithm_decode(i);
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
