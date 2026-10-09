/*
 * bitio.h: leitura e escrita de bits no estilo DEFLATE.
 *
 * O DEFLATE empacota os bits a partir do bit menos significativo de cada
 * byte. Exemplo: os campos BFINAL (1 bit) e BTYPE (2 bits) do primeiro bloco
 * ficam nos bits 0, 1 e 2 do primeiro byte:
 *
 *     byte 0:  b7 b6 b5 b4 b3 | b2 b1 | b0
 *              (próximos)       BTYPE   BFINAL
 *
 * O leitor guarda até 64 bits num acumulador: o próximo bit a consumir é
 * sempre o bit 0 do acumulador, e bytes novos entram pela esquerda.
 */
#ifndef MGZ_BITIO_H
#define MGZ_BITIO_H

#include <stddef.h>
#include <stdint.h>

#include "mgz.h"

#define BR_BUFSZ 65536u
#define BW_BUFSZ 65536u

/* ---------------------------------------------------------------- leitor */

typedef struct {
    mgz_read_fn read;
    void *ctx;
    uint8_t *buf;       /* bloco de entrada lido da fonte */
    size_t pos, len;    /* próximo byte de buf e quantidade válida */
    uint64_t bits;      /* acumulador: o próximo bit é o bit 0 */
    unsigned nbits;     /* quantos bits válidos há no acumulador */
    int eof;            /* a fonte acabou */
    int err;            /* a fonte informou erro */
    uint64_t fetched;   /* total de bytes recebidos da fonte */
} bit_reader;

mgz_status br_init(bit_reader *br, mgz_read_fn read, void *ctx);
void br_free(bit_reader *br);

/* Busca mais dados na fonte. Retorna 1 se chegou algo, 0 no fim ou erro. */
int br_refill(bit_reader *br);

/* Status adequado quando faltam dados: erro de leitura ou truncamento. */
static inline mgz_status br_short_status(const bit_reader *br)
{
    return br->err ? MGZ_ERR_READ : MGZ_ERR_TRUNCATED;
}

/* Completa o acumulador com bytes inteiros até ter pelo menos 57 bits. */
static inline void br_fill(bit_reader *br)
{
    while (br->nbits <= 56) {
        if (br->pos == br->len && !br_refill(br))
            break;
        br->bits |= (uint64_t)br->buf[br->pos++] << br->nbits;
        br->nbits += 8;
    }
}

/* Garante n bits (n <= 57) no acumulador. */
static inline mgz_status br_need(bit_reader *br, unsigned n)
{
    if (br->nbits < n) {
        br_fill(br);
        if (br->nbits < n)
            return br_short_status(br);
    }
    return MGZ_OK;
}

/* Olha os n próximos bits sem consumir (completa com zeros no fim). */
static inline uint32_t br_peek(const bit_reader *br, unsigned n)
{
    return (uint32_t)(br->bits & ((UINT64_C(1) << n) - 1u));
}

static inline void br_drop(bit_reader *br, unsigned n)
{
    br->bits >>= n;
    br->nbits -= n;
}

/* Lê um campo de n bits (n <= 32), menos significativo primeiro. */
static inline mgz_status br_get(bit_reader *br, unsigned n, uint32_t *out)
{
    mgz_status st = br_need(br, n);
    if (st != MGZ_OK)
        return st;
    *out = br_peek(br, n);
    br_drop(br, n);
    return MGZ_OK;
}

/* Descarta os bits até o próximo limite de byte. */
static inline void br_align(bit_reader *br)
{
    br_drop(br, br->nbits & 7u);
}

/* Lê um byte inteiro (o leitor deve estar alinhado). */
mgz_status br_byte(bit_reader *br, uint8_t *out);

/* Copia n bytes alinhados para dst. */
mgz_status br_bytes(bit_reader *br, uint8_t *dst, size_t n);

/* Diz se ainda existe pelo menos um byte (alinhado) para ler. */
int br_has_byte(bit_reader *br);

/* Posição atual na entrada, em bits a partir do começo. */
uint64_t br_bit_position(const bit_reader *br);

/* -------------------------------------------------------------- escritor */

typedef struct {
    uint64_t bits;      /* bits pendentes, o mais antigo no bit 0 */
    unsigned nbits;     /* sempre menor que 8 depois de cada escrita */
    uint8_t *buf;       /* bytes prontos, esperando para ir à saída */
    size_t len;
    mgz_write_fn write;
    void *ctx;
    int err;
    uint64_t total;     /* bytes completos produzidos até agora */
} bit_writer;

mgz_status bw_init(bit_writer *bw, mgz_write_fn write, void *ctx);
void bw_free(bit_writer *bw);

/* Entrega à saída os bytes acumulados (não mexe nos bits pendentes). */
void bw_flush(bit_writer *bw);

static inline void bw_byte(bit_writer *bw, uint8_t b)
{
    if (bw->len == BW_BUFSZ)
        bw_flush(bw);
    bw->buf[bw->len++] = b;
    bw->total++;
}

/* Escreve os n bits menos significativos de value (n <= 32). */
static inline void bw_put(bit_writer *bw, uint32_t value, unsigned n)
{
    bw->bits |= ((uint64_t)value & ((UINT64_C(1) << n) - 1u)) << bw->nbits;
    bw->nbits += n;
    while (bw->nbits >= 8) {
        bw_byte(bw, (uint8_t)(bw->bits & 0xFFu));
        bw->bits >>= 8;
        bw->nbits -= 8;
    }
}

/* Completa o byte atual com zeros. */
static inline void bw_align(bit_writer *bw)
{
    if (bw->nbits > 0)
        bw_put(bw, 0, 8 - bw->nbits);
}

/* Escreve bytes inteiros (o escritor deve estar alinhado). */
void bw_bytes(bit_writer *bw, const uint8_t *data, size_t n);

/* Total de bits escritos até agora. */
static inline uint64_t bw_bit_count(const bit_writer *bw)
{
    return bw->total * 8u + bw->nbits;
}

#endif /* MGZ_BITIO_H */
