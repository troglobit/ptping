#ifndef PTPING_BE_H
#define PTPING_BE_H

#include <endian.h>
#include <stdint.h>
#include <string.h>

/* Big-endian field access for wire formats; unaligned safe. */

static inline void put16(uint8_t *p, uint16_t v)
{
	uint16_t be = htobe16(v);

	memcpy(p, &be, sizeof(be));
}

static inline void put32(uint8_t *p, uint32_t v)
{
	uint32_t be = htobe32(v);

	memcpy(p, &be, sizeof(be));
}

static inline void put64(uint8_t *p, uint64_t v)
{
	uint64_t be = htobe64(v);

	memcpy(p, &be, sizeof(be));
}

static inline uint16_t get16(const uint8_t *p)
{
	uint16_t be;

	memcpy(&be, p, sizeof(be));
	return be16toh(be);
}

static inline uint32_t get32(const uint8_t *p)
{
	uint32_t be;

	memcpy(&be, p, sizeof(be));
	return be32toh(be);
}

static inline uint64_t get64(const uint8_t *p)
{
	uint64_t be;

	memcpy(&be, p, sizeof(be));
	return be64toh(be);
}

#endif
