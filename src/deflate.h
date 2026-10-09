/* deflate.h: compressor DEFLATE (LZ77 com cadeias de hash + Huffman). */
#ifndef MGZ_DEFLATE_H
#define MGZ_DEFLATE_H

#include "bitio.h"
#include "mgz.h"

/* Estatísticas de uma compressão (úteis para estudo e testes). */
typedef struct {
    unsigned blocks[3];     /* quantos blocos de cada tipo (armazenado, fixo, dinâmico) */
    uint64_t literals;      /* literais emitidos */
    uint64_t matches;       /* referências (comprimento, distância) emitidas */
    unsigned max_distance;  /* maior distância usada */
} deflate_stats;

/*
 * Lê toda a entrada e escreve o fluxo DEFLATE em bw (sem alinhar no fim).
 * Devolve o CRC-32 e o tamanho da entrada, usados pelo trailer do gzip.
 * stats pode ser NULL.
 */
mgz_status deflate_stream(bit_writer *bw, mgz_read_fn read, void *rctx, int level,
                          uint32_t *crc, uint64_t *total_in, deflate_stats *stats);

#endif /* MGZ_DEFLATE_H */
