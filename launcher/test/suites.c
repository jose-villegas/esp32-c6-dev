/*=============================================================================
 * The suite registry, shared by both runners.
 *
 * Deliberately free of Unity and of anything platform-specific: it is a sorted
 * list of function pointers filled in before main(), so it links identically
 * into the host runner and the firmware.
 *===========================================================================*/

#include "suites.h"

#include <stdio.h>
#include <string.h>

static struct {
    const char *name;
    suite_fn    fn;
} suites[SUITE_MAX];

static int registered;

void suite_register(const char *name, suite_fn fn)
{
    if (registered >= SUITE_MAX) {
        /* Silently dropping a suite would mean a green run that did not
         * actually test everything, which is worse than no tests at all. */
        printf("SUITE OVERFLOW: '%s' was not registered (max %d)\n",
               name, SUITE_MAX);
        return;
    }
    suites[registered].name = name;
    suites[registered].fn   = fn;
    registered++;
}

void suites_run_all(void)
{
    /* Link order decides constructor order, so sort to keep the output stable
     * between builds. */
    for (int i = 1; i < registered; i++) {
        const char *key_name = suites[i].name;
        suite_fn    key_fn   = suites[i].fn;
        int j = i - 1;
        while (j >= 0 && strcmp(suites[j].name, key_name) > 0) {
            suites[j + 1] = suites[j];
            j--;
        }
        suites[j + 1].name = key_name;
        suites[j + 1].fn   = key_fn;
    }

    for (int i = 0; i < registered; i++) {
        suites[i].fn();
    }
}
