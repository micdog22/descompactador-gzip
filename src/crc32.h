/* crc32.h: funções internas de CRC-32 (a pública é mgz_crc32, em mgz.h). */
#ifndef MGZ_CRC32_H
#define MGZ_CRC32_H

#include "mgz.h"

/* Calcula uma entrada da tabela do zero (usada nos testes). */
uint32_t crc32_table_entry(unsigned n);

/* Versão didática, um bit por vez: lenta, mas igual à de tabela. */
uint32_t crc32_bitwise(uint32_t crc, const void *data, size_t len);

#endif
