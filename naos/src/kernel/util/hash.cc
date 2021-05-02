#include "kernel/util/hash.hpp"

namespace util
{
u32 djb_hash(const char *str)
{
    u32 hash = 5381;

    while (*str)
    {
        hash = ((hash << 5) + hash) + (*str++); /* times 33 */
    }

    return hash;
}

//-----------------------------------------------------------------------------
// MurmurHash2 was written by Austin Appleby, and is placed in the public
// domain. The author hereby disclaims copyright to this source code.

// Note - This code makes a few assumptions about how your machine behaves -

// 1. We can read a 4-byte value from any address without crashing
// 2. sizeof(int) == 4

// And it has a few limitations -

// 1. It will not work incrementally.
// 2. It will not produce the same results on little-endian and big-endian
//    machines.

//-----------------------------------------------------------------------------
// MurmurHash2, 64-bit versions, by Austin Appleby

// The same caveats as 32-bit MurmurHash2 apply here - beware of alignment
// and endian-ness issues if used across multiple platforms.

// 64-bit hash for 64-bit platforms

uint64_t murmur_hash2_64(const void *key, u64 len, u64 seed)
{
    const uint64_t m = 0xc6a4a7935bd1e995;
    const int r = 47;

    uint64_t h = seed ^ (len * m);

    const uint64_t *data = (const uint64_t *)key;
    const uint64_t *end = data + (len / 8);

    while (data != end)
    {
        uint64_t k = *data++;

        k *= m;
        k ^= k >> r;
        k *= m;

        h ^= k;
        h *= m;
    }

    const unsigned char *data2 = (const unsigned char *)data;

    switch (len & 7)
    {
        case 7:
            h ^= uint64_t(data2[6]) << 48;
        case 6:
            h ^= uint64_t(data2[5]) << 40;
        case 5:
            h ^= uint64_t(data2[4]) << 32;
        case 4:
            h ^= uint64_t(data2[3]) << 24;
        case 3:
            h ^= uint64_t(data2[2]) << 16;
        case 2:
            h ^= uint64_t(data2[1]) << 8;
        case 1:
            h ^= uint64_t(data2[0]);
            h *= m;
    };

    h ^= h >> r;
    h *= m;
    h ^= h >> r;

    return h;
}

//-----------------------------------------------------------------------------
// MurmurHash3 was written by Austin Appleby, and is placed in the public
// domain. The author hereby disclaims copyright to this source code.

// Note - The x86 and x64 versions do _not_ produce the same results, as the
// algorithms are optimized for their respective platforms. You can still
// compile and run any of them on any platform, but your performance with the
// non-native version will be less than optimal.

inline uint32_t rotl32(uint32_t x, int8_t r) { return (x << r) | (x >> (32 - r)); }

inline uint64_t rotl64(uint64_t x, int8_t r) { return (x << r) | (x >> (64 - r)); }

inline uint32_t getblock32(const uint32_t *p, int i) { return p[i]; }

inline uint64_t getblock64(const uint64_t *p, int i) { return p[i]; }

//-----------------------------------------------------------------------------
// Finalization mix - force all bits of a hash block to avalanche

inline uint32_t fmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;

    return h;
}

//----------

inline uint64_t fmix64(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccd;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53;
    k ^= k >> 33;

    return k;
}

void murmur_hash3_128(const void *key, const u64 len, const uint32_t seed, void *out)
{
    const uint8_t *data = (const uint8_t *)key;
    const int nblocks = len / 16;

    uint64_t h1 = seed;
    uint64_t h2 = seed;

    const uint64_t c1 = 0x87c37b91114253d5;
    const uint64_t c2 = 0x4cf5ad432745937f;

    //----------
    // body

    const uint64_t *blocks = (const uint64_t *)(data);

    for (int i = 0; i < nblocks; i++)
    {
        uint64_t k1 = getblock64(blocks, i * 2 + 0);
        uint64_t k2 = getblock64(blocks, i * 2 + 1);

        k1 *= c1;
        k1 = rotl64(k1, 31);
        k1 *= c2;
        h1 ^= k1;

        h1 = rotl64(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729;

        k2 *= c2;
        k2 = rotl64(k2, 33);
        k2 *= c1;
        h2 ^= k2;

        h2 = rotl64(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5;
    }

    //----------
    // tail

    const uint8_t *tail = (const uint8_t *)(data + nblocks * 16);

    uint64_t k1 = 0;
    uint64_t k2 = 0;

    switch (len & 15)
    {
        case 15:
            k2 ^= ((uint64_t)tail[14]) << 48;
        case 14:
            k2 ^= ((uint64_t)tail[13]) << 40;
        case 13:
            k2 ^= ((uint64_t)tail[12]) << 32;
        case 12:
            k2 ^= ((uint64_t)tail[11]) << 24;
        case 11:
            k2 ^= ((uint64_t)tail[10]) << 16;
        case 10:
            k2 ^= ((uint64_t)tail[9]) << 8;
        case 9:
            k2 ^= ((uint64_t)tail[8]) << 0;
            k2 *= c2;
            k2 = rotl64(k2, 33);
            k2 *= c1;
            h2 ^= k2;

        case 8:
            k1 ^= ((uint64_t)tail[7]) << 56;
        case 7:
            k1 ^= ((uint64_t)tail[6]) << 48;
        case 6:
            k1 ^= ((uint64_t)tail[5]) << 40;
        case 5:
            k1 ^= ((uint64_t)tail[4]) << 32;
        case 4:
            k1 ^= ((uint64_t)tail[3]) << 24;
        case 3:
            k1 ^= ((uint64_t)tail[2]) << 16;
        case 2:
            k1 ^= ((uint64_t)tail[1]) << 8;
        case 1:
            k1 ^= ((uint64_t)tail[0]) << 0;
            k1 *= c1;
            k1 = rotl64(k1, 31);
            k1 *= c2;
            h1 ^= k1;
    };

    //----------
    // finalization

    h1 ^= len;
    h2 ^= len;

    h1 += h2;
    h2 += h1;

    h1 = fmix64(h1);
    h2 = fmix64(h2);

    h1 += h2;
    h2 += h1;

    ((uint64_t *)out)[0] = h1;
    ((uint64_t *)out)[1] = h2;
}
} // namespace util
