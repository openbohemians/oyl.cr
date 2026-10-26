/*
 * oyl_hash.c — the secret key of the hash tables a document fills
 *
 * Each parser takes one from the system's randomness the first time a
 * table needs it (oyl_internal.h, "Hashing"): getrandom on Linux,
 * arc4random_buf on the BSDs, macOS and Android, rand_s on Windows.
 */

#if defined(_WIN32)
#  define _CRT_RAND_S   /* declares rand_s; before any header */
#endif

#include "oyl_internal.h"
#include <stdlib.h>
#include <time.h>

#if defined(__linux__) && !defined(__ANDROID__) && defined(__has_include)
#  if __has_include(<sys/random.h>)
#    include <sys/random.h>
#    define OYL_GETRANDOM 1
#  endif
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
    defined(__DragonFly__) || defined(__ANDROID__)
#  define OYL_ARC4RANDOM 1
#endif

static bool system_random(uint64_t k[2]) {
#if defined(OYL_GETRANDOM)
    /* not waiting: only very early at boot is there none yet */
    return getrandom(k, 2 * sizeof *k, GRND_NONBLOCK) == (ssize_t)(2 * sizeof *k);
#elif defined(OYL_ARC4RANDOM)
    arc4random_buf(k, 2 * sizeof *k);
    return true;
#elif defined(_WIN32)
    unsigned int r[4];
    for (int i = 0; i < 4; i++)
        if (rand_s(&r[i]) != 0) return false;
    k[0] = (uint64_t)r[0] << 32 | r[1];
    k[1] = (uint64_t)r[2] << 32 | r[3];
    return true;
#else
    (void)k;
    return false;
#endif
}

void oyl_hash_key_new(oyl_hash_key *key) {
    uint64_t k[2];
    if (!system_random(k)) {
        /* none to be had: what someone writing documents elsewhere can't
         * know, where the address space was laid out and when */
        static const char here = 0;
        const oyl_hash_key fixed = { 0x0123456789abcdefu, 0xfedcba9876543210u };
        uint64_t mix[4] = { (uint64_t)(uintptr_t)key, (uint64_t)(uintptr_t)&here,
                            (uint64_t)time(NULL), (uint64_t)clock() };
        k[0] = oyl_hash(&fixed, mix, sizeof mix);
        mix[0] = k[0];
        k[1] = oyl_hash(&fixed, mix, sizeof mix);
    }
    key->k0 = k[0];
    key->k1 = k[1];
}
