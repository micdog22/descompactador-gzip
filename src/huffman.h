/*
 * huffman.h: códigos de Huffman canônicos do DEFLATE.
 *
 * No DEFLATE só os COMPRIMENTOS dos códigos viajam no arquivo. Os códigos em
 * si são reconstruídos pela regra canônica (RFC 1951, seção 3.2.2): códigos
 * mais curtos vêm antes e, no mesmo comprimento, a ordem segue o símbolo.
 */
#ifndef MGZ_HUFFMAN_H
#define MGZ_HUFFMAN_H

#include <stdint.h>

#include "bitio.h"
#include "mgz.h"

#define HUFF_MAX_BITS 15
#define HUFF_MAX_SYMS 288
#define HUFF_FAST_BITS 9
#define HUFF_FAST_SIZE (1u << HUFF_FAST_BITS)

/* Regras de validação (as mesmas que o zlib aplica). */
typedef enum {
    HUFF_COMPLETE,     /* o código precisa ser completo (tabela de comprimentos) */
    HUFF_LITLEN,       /* aceita também um único código de 1 bit */
    HUFF_DISTANCE      /* aceita um único código de 1 bit ou nenhum código */
} huff_kind;

/*
 * Tabela de decodificação. A tabela rápida resolve de uma vez qualquer
 * código de até 9 bits; códigos mais longos usam a busca canônica, que
 * compara o código lido com o maior código de cada comprimento.
 */
typedef struct {
    uint16_t fast[HUFF_FAST_SIZE];          /* (comprimento << 9) | símbolo; 0 = lento */
    uint32_t limit[HUFF_MAX_BITS + 2];      /* fim dos códigos de cada comprimento, alinhado a 16 bits */
    uint16_t first_code[HUFF_MAX_BITS + 1]; /* primeiro código de cada comprimento */
    uint16_t first_index[HUFF_MAX_BITS + 1];/* posição do primeiro símbolo desse comprimento */
    uint8_t sorted_len[HUFF_MAX_SYMS];      /* símbolos em ordem canônica: comprimento */
    uint16_t sorted_sym[HUFF_MAX_SYMS];     /* e o símbolo */
    unsigned count;                         /* quantos símbolos têm código */
} huff_decoder;

/* Monta a tabela a partir dos comprimentos. Retorna MGZ_ERR_CODE_LENGTHS se inválidos. */
mgz_status huff_build(huff_decoder *d, const uint8_t *lens, unsigned n, huff_kind kind);

mgz_status huff_decode_slow(bit_reader *br, const huff_decoder *d, unsigned *sym);

/* Lê um símbolo da entrada. */
static inline mgz_status huff_decode(bit_reader *br, const huff_decoder *d, unsigned *sym)
{
    if (br->nbits < HUFF_MAX_BITS)
        br_fill(br);
    unsigned e = d->fast[br->bits & (HUFF_FAST_SIZE - 1u)];
    if (e != 0) {
        unsigned len = e >> 9;
        if (len > br->nbits)
            return br_short_status(br);
        *sym = e & 511u;
        br_drop(br, len);
        return MGZ_OK;
    }
    return huff_decode_slow(br, d, sym);
}

/* Inverte os len bits menos significativos de code. */
unsigned huff_reverse(unsigned code, unsigned len);

/* Atribui os códigos canônicos (bit mais significativo primeiro). */
void huff_canonical_codes(const uint8_t *lens, unsigned n, uint16_t *codes);

/*
 * Calcula comprimentos ótimos de Huffman limitados a max_bits com o
 * algoritmo package-merge. Símbolos com frequência zero ficam sem código.
 * Com um único símbolo usado, ele recebe comprimento 1.
 */
void huff_limited_lengths(const uint32_t *freq, unsigned n, unsigned max_bits, uint8_t *lens);

#endif /* MGZ_HUFFMAN_H */
