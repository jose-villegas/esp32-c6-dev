/*=============================================================================
 * Portable suite: the pseudo-random generator.
 *
 * Small, but everything above it leans on the two properties here - that the
 * same seed replays exactly, and that the probability boundaries mean what
 * they say. A test like "shaking flattens a pile" is only reproducible because
 * of the first, and the friction model expresses "never" and "always" as 0 and
 * 256 and needs those to be exact.
 *===========================================================================*/

#include "unity.h"
#include "suites.h"

#include "rng.h"

static void test_the_same_seed_replays_exactly(void)
{
    rng_t a, b;
    rng_seed(&a, 12345u);
    rng_seed(&b, 12345u);

    for (int i = 0; i < 1000; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(rng_next(&a), rng_next(&b),
            "the same seed must give the same sequence - every reproducible "
            "test above this one depends on it");
    }
}

static void test_different_seeds_diverge(void)
{
    rng_t a, b;
    rng_seed(&a, 1u);
    rng_seed(&b, 2u);

    int same = 0;
    for (int i = 0; i < 100; i++) {
        if (rng_next(&a) == rng_next(&b)) {
            same++;
        }
    }
    TEST_ASSERT_LESS_THAN_MESSAGE(2, same,
        "two seeds must not produce the same stream, or seeding per trial "
        "buys nothing");
}

static void test_a_zero_seed_does_not_stick(void)
{
    /* Zero is a fixed point of xorshift: left alone, the state stays zero and
     * every draw is zero for ever. A caller seeding from a timer that happened
     * to read zero would get a generator that silently does nothing. */
    rng_t r;
    rng_seed(&r, 0u);

    uint32_t combined = 0;
    for (int i = 0; i < 20; i++) {
        combined |= rng_next(&r);
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, combined,
        "a zero seed must be substituted, not accepted");
}

static void test_the_state_never_reaches_zero(void)
{
    /* The same fixed point, reached from the other direction: if any draw ever
     * left the state at zero the generator would die mid-run. */
    rng_t r;
    rng_seed(&r, 987654321u);

    for (int i = 0; i < 100000; i++) {
        (void)rng_next(&r);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, r.state,
            "the generator must not be able to fall into its fixed point");
    }
}

static void test_below_stays_inside_its_bound(void)
{
    rng_t r;
    rng_seed(&r, 42u);

    for (int i = 0; i < 5000; i++) {
        const int v = rng_below(&r, 6);
        TEST_ASSERT_TRUE_MESSAGE(v >= 0 && v < 6,
            "rng_below must never return its own bound - an off-by-one here "
            "indexes one past a palette");
    }
}

static void test_below_reaches_every_value(void)
{
    rng_t r;
    rng_seed(&r, 7u);

    bool seen[6] = { false };
    for (int i = 0; i < 500; i++) {
        seen[rng_below(&r, 6)] = true;
    }
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[i],
            "every value in range must actually come up, or a pile ends up "
            "using half its shades");
    }
}

static void test_below_handles_a_useless_bound(void)
{
    rng_t r;
    rng_seed(&r, 3u);

    TEST_ASSERT_EQUAL_INT(0, rng_below(&r, 0));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rng_below(&r, -5),
        "a bound of zero or less must be answered, not divided by");
}

static void test_never_and_always_are_exact(void)
{
    rng_t r;
    rng_seed(&r, 99u);

    for (int i = 0; i < 2000; i++) {
        TEST_ASSERT_FALSE_MESSAGE(rng_chance(&r, 0),
            "a chance of zero must be impossible, not merely unlikely");
        TEST_ASSERT_TRUE_MESSAGE(rng_chance(&r, 256),
            "and a chance of 256 must be certain - written naively as "
            "(draw & 0xFF) < 256 it is, but 255 out of 256 is not, and the "
            "friction model expresses 'always' this way");
    }
}

static void test_a_chance_lands_near_its_stated_odds(void)
{
    rng_t r;
    rng_seed(&r, 2024u);

    int hits = 0;
    const int trials = 20000;
    for (int i = 0; i < trials; i++) {
        if (rng_chance(&r, 64)) {     /* a quarter */
            hits++;
        }
    }

    TEST_ASSERT_INT_WITHIN_MESSAGE(trials / 20, trials / 4, hits,
        "a stated one-in-four must actually come up about a quarter of the "
        "time, or every probability tuned against it is meaningless");
}

void run_rng_suite(void)
{
    RUN_TEST(test_the_same_seed_replays_exactly);
    RUN_TEST(test_different_seeds_diverge);
    RUN_TEST(test_a_zero_seed_does_not_stick);
    RUN_TEST(test_the_state_never_reaches_zero);
    RUN_TEST(test_below_stays_inside_its_bound);
    RUN_TEST(test_below_reaches_every_value);
    RUN_TEST(test_below_handles_a_useless_bound);
    RUN_TEST(test_never_and_always_are_exact);
    RUN_TEST(test_a_chance_lands_near_its_stated_odds);
}

SUITE_REGISTER(run_rng_suite);
