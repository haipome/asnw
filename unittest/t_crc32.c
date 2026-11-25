/*
 * wrapper/ut_crc32: the 8 bytes at a time implementation gives the standard
 * CRC-32, for every length around the 8 byte steps and every alignment of
 * the buffer.
 */
# include "common.h"
# include "ut_crc32.h"

/* the definition of the checksum, bit by bit */
static uint32_t crc32_ref(const char *buffer, size_t length)
{
    uint32_t crc = ~(uint32_t)0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (unsigned char)buffer[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

int main(void)
{
    /* known values of the standard CRC-32 */
    CHECK_EQ(generate_crc32("", 0), 0);
    CHECK_EQ(generate_crc32("a", 1), 0xE8B7BE43u);
    CHECK_EQ(generate_crc32("123456789", 9), 0xCBF43926u);
    CHECK_EQ(generate_crc32("The quick brown fox jumps over the lazy dog", 43), 0x414FA339u);

    enum { SIZE = 1 << 20 };
    char *buf = malloc(SIZE + 16);
    CHECK(buf != NULL);
    uint32_t seed = 12345;
    for (size_t i = 0; i < SIZE + 16; ++i) {
        seed = seed * 1103515245 + 12345;
        buf[i] = seed >> 16;
    }

    /* every length up to a few steps, at every offset within a word */
    for (size_t off = 0; off < 16; ++off) {
        for (size_t len = 0; len <= 300; ++len)
            CHECK_EQ(generate_crc32(buf + off, len), crc32_ref(buf + off, len));
    }

    /* high bytes: nothing is sign extended */
    char ff[64];
    memset(ff, 0xFF, sizeof(ff));
    for (size_t len = 0; len <= sizeof(ff); ++len)
        CHECK_EQ(generate_crc32(ff, len), crc32_ref(ff, len));

    /* a large package, the case the 8 byte steps are for */
    CHECK_EQ(generate_crc32(buf, SIZE), crc32_ref(buf, SIZE));
    CHECK_EQ(generate_crc32(buf + 3, SIZE - 5), crc32_ref(buf + 3, SIZE - 5));

    /* any corrupted byte changes the checksum */
    uint32_t good = generate_crc32(buf, 4096);
    for (size_t i = 0; i < 4096; i += 37) {
        buf[i] ^= 0x10;
        CHECK(generate_crc32(buf, 4096) != good);
        buf[i] ^= 0x10;
    }

    free(buf);
    return 0;
}
