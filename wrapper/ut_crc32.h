/*
 * Description: checksum used by ut_rpc
 *     History: yang@haipo.me, 2016/03/29, create
 */

# ifndef _UT_CRC32_H_
# define _UT_CRC32_H_

# include <stddef.h>
# include <stdint.h>

/* checksum of length bytes: the standard CRC-32 (IEEE 802.3, the one of
 * zlib and zip), so a peer written in another language can use its
 * ordinary crc32 function. not CRC-32C (Castagnoli) */
uint32_t generate_crc32(const char *buffer, size_t length);

# endif
