/* inflate.h: decodificador DEFLATE (RFC 1951) usado pelo contêiner gzip. */
#ifndef MGZ_INFLATE_H
#define MGZ_INFLATE_H

#include "bitio.h"
#include "huffman.h"
#include "mgz.h"

#define WINDOW_SIZE 32768u          /* distância máxima de uma referência */
#define OUT_BUFSZ (4u * WINDOW_SIZE) /* janela de saída + espaço para escrever */

/*
 * Janela de saída: guarda o que já foi produzido para que as referências
 * (comprimento, distância) possam copiar de até 32 KiB atrás. Quando o
 * buffer enche, o que ainda não foi entregue vai para a função de escrita e
 * os últimos 32 KiB são movidos para o começo.
 */
typedef struct {
    uint8_t *buf;
    size_t pos;          /* próxima posição de escrita */
    size_t flushed;      /* buf[0..flushed) já foi entregue */
    uint64_t total;      /* bytes produzidos no fluxo atual */
    uint32_t crc;        /* CRC-32 do que já foi entregue */
    mgz_write_fn write;  /* NULL: só verifica */
    void *ctx;
    int err;
} out_window;

mgz_status ow_init(out_window *ow, mgz_write_fn write, void *ctx);
void ow_free(out_window *ow);
void ow_reset(out_window *ow);      /* começa um novo fluxo (novo membro) */
mgz_status ow_flush(out_window *ow);

typedef struct {
    bit_reader *br;
    out_window *ow;
    huff_decoder litlen, dist, pre;
    huff_decoder fixed_litlen, fixed_dist;
    int fixed_ready;
    mgz_block_fn on_block;
    void *block_ctx;
    unsigned member;
} inflater;

void inflater_init(inflater *inf, bit_reader *br, out_window *ow);

/* Decodifica blocos até o bloco final (BFINAL = 1), inclusive. */
mgz_status inflate_stream(inflater *inf);

#endif /* MGZ_INFLATE_H */
