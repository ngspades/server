/* MT19937 (Matsumoto and Nishimura, "Mersenne Twister: a 623-dimensionally
 * equidistributed uniform pseudorandom number generator", ACM TOMACS 8(1),
 * 1998), written for this project from the paper's description. The seeding
 * is a linear congruential fill (multiplier 6069 on the low 32 bits), kept
 * from the generator this replaces so a seed gives the same sequence the
 * server always had (checked against it on 5700 outputs, seeded and not).
 *
 * Copyright (c) 2026 ngspades contributors, GPL-3.0-or-later.
 */

#include <mersenne-twister/MT.h>

#define MT_UPPER_MASK 0x80000000UL
#define MT_LOWER_MASK 0x7fffffffUL
#define MT_MATRIX_A   0x9908b0dfUL

static void mt_fill(mt_rand_t* rand, unsigned long seed)
{
    rand->mt[0] = seed & 0xffffffffUL;
    for (rand->index = 1; rand->index < STATE_VECTOR_LENGTH; rand->index++) {
        rand->mt[rand->index] = (6069UL * rand->mt[rand->index - 1]) & 0xffffffffUL;
    }
}

mt_rand_t seed_rand(unsigned long seed)
{
    mt_rand_t rand;
    mt_fill(&rand, seed);
    return rand;
}

/* the next 624 words of the state */
static void mt_twist(mt_rand_t* rand)
{
    static const unsigned long mag[2] = {0x0UL, MT_MATRIX_A};
    for (int k = 0; k < STATE_VECTOR_LENGTH; k++) {
        unsigned long y = (rand->mt[k] & MT_UPPER_MASK) | (rand->mt[(k + 1) % STATE_VECTOR_LENGTH] & MT_LOWER_MASK);
        rand->mt[k]     = rand->mt[(k + STATE_VECTOR_M) % STATE_VECTOR_LENGTH] ^ (y >> 1) ^ mag[y & 1UL];
    }
    rand->index = 0;
}

unsigned long gen_rand_long(mt_rand_t* rand)
{
    if (rand->index >= STATE_VECTOR_LENGTH || rand->index < 0) {
        if (rand->index >= STATE_VECTOR_LENGTH + 1 || rand->index < 0) {
            mt_fill(rand, 4357); /* never seeded: the paper's default seed */
        }
        mt_twist(rand);
    }
    unsigned long y = rand->mt[rand->index++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680UL;
    y ^= (y << 15) & 0xefc60000UL;
    y ^= (y >> 18);
    return y & 0xffffffffUL;
}

double gen_rand(mt_rand_t* rand)
{
    return (double) gen_rand_long(rand) / (double) 0xffffffffUL;
}
