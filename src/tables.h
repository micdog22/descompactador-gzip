/* tables.h: tabelas fixas do DEFLATE (RFC 1951, seções 3.2.5 e 3.2.7). */
#ifndef MGZ_TABLES_H
#define MGZ_TABLES_H

#include <stdint.h>

/* Símbolos 257 a 285: comprimento base e quantos bits extras seguem. */
extern const uint16_t LEN_BASE[29];
extern const uint8_t LEN_EXTRA[29];

/* Códigos de distância 0 a 29: distância base e bits extras. */
extern const uint16_t DIST_BASE[30];
extern const uint8_t DIST_EXTRA[30];

/* Ordem em que chegam os comprimentos do código de comprimentos. */
extern const uint8_t CL_ORDER[19];

#endif
