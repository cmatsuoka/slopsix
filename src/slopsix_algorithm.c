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
 *     first one adds the operator's output to the feedback path, the
 *     other two are the sum/delay register's control bits.
 *
 * That alignment reproduces the carrier set of all 32 algorithms against
 * the service-manual chart and the feedback operator of all 32
 * algorithms against the analysed engine's feedback structure; the
 * harness checks both (32/32).
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O2); the rest of the library is compiled size-first.
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
    { 0x001, 0x001, 0x025, 0x038, 0x032, 0x030 },
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
 * operator 1 (research A-01, the one-carrier caveat). */
static const uint8_t slopsix_single_carrier_algorithms[3] = { 16, 17, 18 };

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
        alg->ops[consumer - 1u].carrier = (uint8_t)(com != 0u ? 1u : 0u);

        switch (sel) {
        case 0:
            alg->ops[consumer - 1u].source = SLOPSIX_MOD_NONE;
            break;
        case 1:
            /* The unmodified signal of the operator computed just before
             * in the walk: the higher-numbered neighbour.  Operator 6,
             * reachable through the wrap-around column, has no
             * higher-numbered neighbour; the checked-in ROM never carries
             * this selector there, and the graph would render it
             * unmodulated if the data ever did. */
            if (consumer < SLOPSIX_OP_COUNT) {
                alg->ops[consumer - 1u].source = SLOPSIX_MOD_OPERATOR;
                alg->ops[consumer - 1u].source_op = (uint8_t)(consumer + 1u);
            }
            break;
        case 2:
            alg->ops[consumer - 1u].source = SLOPSIX_MOD_REGISTER;
            break;
        case 3:
            alg->ops[consumer - 1u].source = SLOPSIX_MOD_REGISTER_DELAYED;
            break;
        case 4:
            alg->ops[consumer - 1u].source = SLOPSIX_MOD_FEEDBACK_DELAYED;
            break;
        default:
            /* The self-feedback path: the operator reads its own output
             * back from the feedback path. */
            alg->ops[consumer - 1u].source = SLOPSIX_MOD_FEEDBACK;
            slopsix_feedback_operator[index] = (uint8_t)consumer;
            break;
        }
    }

    /* The control bits belong to the operator the column is labelled
     * with: the first one adds the operator's output to the feedback
     * path, the other two are the sum/delay register's control bits.
     * The ROM's columns run from operator 6 to operator 1, so the column
     * at index i is labelled with operator 6 - i. */
    for (i = 0; i < SLOPSIX_OP_COUNT; i++) {
        uint16_t entry = rom[i];
        unsigned op_number = SLOPSIX_OP_COUNT - i;
        unsigned ctrl_c = (entry & SLOPSIX_ROM_CTRL_C) ? 1u : 0u;
        unsigned ctrl_d = (entry & SLOPSIX_ROM_CTRL_D) ? 1u : 0u;

        alg->ops[op_number - 1u].buses =
            (uint8_t)((ctrl_d ? SLOPSIX_BUS_FIRST : 0u) |
                      (ctrl_c ? SLOPSIX_BUS_SECOND : 0u));

        alg->ops[op_number - 1u].fb_out =
            (uint8_t)((entry & SLOPSIX_ROM_FB_OUT) ? 1u : 0u);
    }

    /* The feedback operator's own output joins the feedback path. */
    for (i = 0; i < SLOPSIX_OP_COUNT; i++) {
        if (alg->ops[i].source == SLOPSIX_MOD_FEEDBACK) {
            alg->ops[i].fb_out = 1u;
        }
    }

    /* The single-carrier algorithms carry no compensation code. */
    for (i = 0; i < 3u; i++) {
        if (slopsix_single_carrier_algorithms[i] == (index + 1u)) {
            alg->ops[0].carrier = 1u;
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
        unsigned op;

        for (op = 0; op < SLOPSIX_OP_COUNT; op++) {
            slopsix_algorithms[i].ops[op].source = SLOPSIX_MOD_NONE;
            slopsix_algorithms[i].ops[op].source_op = 0u;
            slopsix_algorithms[i].ops[op].carrier = 0u;
            slopsix_algorithms[i].ops[op].com = 0u;
            slopsix_algorithms[i].ops[op].buses = 0u;
            slopsix_algorithms[i].ops[op].fb_out = 0u;
        }
        slopsix_feedback_operator[i] = 0u;
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
 * The graph walk.
 *
 * The feedback path holds the sum of the outputs that the algorithm's
 * control bits add to it.  The feedback operator itself is part of that
 * sum, so its own output reaches the path even when no other operator
 * contributes to it.
 *
 * The hardware computes the operators in the order op6 -> op1 and an
 * operator is only ever modulated by a higher-numbered operator, so the
 * walk order is not a free choice (research A-01).  The
 * scratch buses carry the sum/delay register's content: an operator
 * whose control bits select a bus replaces that bus' content, and the
 * register reads the sum of the buses.
 */
void slopsix_voice_render_sample(slopsix_voice_engine *voice,
                                  const slopsix_algorithm *alg,
                                  int32_t *voice_out)
{
    int32_t bus[2];
    int32_t bus_writer[2];
    int32_t register_value;
    int32_t voice_sum;
    int32_t feedback_path;
    int op;

    bus[0] = 0;
    bus[1] = 0;
    bus_writer[0] = -1;
    bus_writer[1] = -1;
    voice_sum = 0;
    feedback_path = 0;

    for (op = SLOPSIX_OP_COUNT - 1; op >= 0; op--) {
        const slopsix_op_conn *conn = &alg->ops[op];
        int32_t mod_in = 0;
        int32_t sample;

        switch (conn->source) {
        case SLOPSIX_MOD_OPERATOR:
            mod_in = voice->out[conn->source_op - 1u];
            break;
        case SLOPSIX_MOD_REGISTER:
            if (bus_writer[0] == bus_writer[1]) {
                mod_in = bus[0];
            } else {
                mod_in = bus[0] + bus[1];
            }
            break;
        case SLOPSIX_MOD_REGISTER_DELAYED:
            mod_in = voice->reg_delay;
            break;
        case SLOPSIX_MOD_FEEDBACK:
            /* The feedback path's value: the average of its last two
             * samples, divided by the feedback shifter. */
            mod_in = slopsix_feedback_scaled(voice->fb_prev,
                                             voice->fb_prev2,
                                             voice->feedback_level);
            break;
        case SLOPSIX_MOD_FEEDBACK_DELAYED:
            mod_in = voice->fb_delay;
            break;
        default:
            /* No modulation input, and the self-feedback path is added
             * inside the operator itself. */
            break;
        }

        slopsix_op_render(&voice->ops[op], &voice->params[op], mod_in,
                          &sample);

        voice->out[op] = sample;

        if (conn->carrier) {
            voice_sum += sample;
        }
        if (conn->buses & SLOPSIX_BUS_FIRST) {
            bus[0] = sample;
            bus_writer[0] = op;
        }
        if (conn->buses & SLOPSIX_BUS_SECOND) {
            bus[1] = sample;
            bus_writer[1] = op;
        }
        if (conn->fb_out) {
            feedback_path += sample;
        }
    }

    /* The register's delayed value and the feedback path's delayed value
     * are the ones of this sample, for the next one. */
    if (bus_writer[0] == bus_writer[1]) {
        register_value = bus[0];
    } else {
        register_value = bus[0] + bus[1];
    }
    voice->reg_delay = register_value;

    /* The feedback path's registers: the two last sums, so that the
     * feedback operator reads their average on the next sample. */
    voice->fb_delay = feedback_path;
    voice->fb_prev2 = voice->fb_prev;
    voice->fb_prev = feedback_path;

    *voice_out = voice_sum;
}
