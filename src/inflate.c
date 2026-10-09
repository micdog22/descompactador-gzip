/*
 * inflate.c: decodificador DEFLATE (RFC 1951).
 *
 * Um fluxo DEFLATE é uma sequência de blocos. Cada bloco começa com 3 bits:
 * BFINAL (este é o último?) e BTYPE (00 armazenado, 01 Huffman fixo,
 * 10 Huffman dinâmico, 11 reservado). Os blocos com Huffman codificam
 * literais (0-255), o fim de bloco (256) e pares (comprimento, distância)
 * que copiam bytes já produzidos.
 */
#include "inflate.h"
#include "tables.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- janela de saída */

mgz_status ow_init(out_window *ow, mgz_write_fn write, void *ctx)
{
    memset(ow, 0, sizeof *ow);
    ow->write = write;
    ow->ctx = ctx;
    ow->buf = malloc(OUT_BUFSZ);
    return ow->buf ? MGZ_OK : MGZ_ERR_MEMORY;
}

void ow_free(out_window *ow)
{
    free(ow->buf);
    ow->buf = NULL;
}

void ow_reset(out_window *ow)
{
    ow->total = 0;
    ow->crc = 0;
}

mgz_status ow_flush(out_window *ow)
{
    size_t n = ow->pos - ow->flushed;
    if (n > 0) {
        ow->crc = mgz_crc32(ow->crc, ow->buf + ow->flushed, n);
        if (ow->write != NULL && !ow->err &&
            ow->write(ow->ctx, ow->buf + ow->flushed, n) != 0)
            ow->err = 1;
        ow->flushed = ow->pos;
    }
    return ow->err ? MGZ_ERR_WRITE : MGZ_OK;
}

/* Garante espaço para need bytes (need <= OUT_BUFSZ - WINDOW_SIZE). */
static mgz_status ow_room(out_window *ow, size_t need)
{
    if (OUT_BUFSZ - ow->pos >= need)
        return MGZ_OK;
    mgz_status st = ow_flush(ow);
    if (st != MGZ_OK)
        return st;
    /* Mantém só os últimos 32 KiB: é o máximo que uma distância alcança. */
    size_t keep = ow->pos < WINDOW_SIZE ? ow->pos : WINDOW_SIZE;
    memmove(ow->buf, ow->buf + ow->pos - keep, keep);
    ow->pos = keep;
    ow->flushed = keep;
    return MGZ_OK;
}

/* ------------------------------------------------------------- blocos */

void inflater_init(inflater *inf, bit_reader *br, out_window *ow)
{
    memset(inf, 0, sizeof *inf);
    inf->br = br;
    inf->ow = ow;
}

/* Bloco armazenado: alinha no byte, lê LEN e NLEN (complemento) e copia. */
static mgz_status inflate_stored(inflater *inf)
{
    bit_reader *br = inf->br;
    out_window *ow = inf->ow;
    uint32_t len, nlen;
    mgz_status st;

    br_align(br);
    if ((st = br_get(br, 16, &len)) != MGZ_OK || (st = br_get(br, 16, &nlen)) != MGZ_OK)
        return st;
    if ((len ^ 0xFFFFu) != nlen)
        return MGZ_ERR_STORED_LEN;
    while (len > 0) {
        if ((st = ow_room(ow, 1)) != MGZ_OK)
            return st;
        size_t take = OUT_BUFSZ - ow->pos;
        if (take > len)
            take = len;
        if ((st = br_bytes(br, ow->buf + ow->pos, take)) != MGZ_OK)
            return st;
        ow->pos += take;
        ow->total += take;
        len -= (uint32_t)take;
    }
    return MGZ_OK;
}

/* Decodifica literais e referências até o símbolo 256 (fim do bloco). */
static mgz_status inflate_codes(inflater *inf, const huff_decoder *ll, const huff_decoder *dd)
{
    bit_reader *br = inf->br;
    out_window *ow = inf->ow;
    mgz_status st;

    for (;;) {
        unsigned sym;
        if ((st = huff_decode(br, ll, &sym)) != MGZ_OK)
            return st;
        if (sym < 256) {
            if (ow->pos == OUT_BUFSZ && (st = ow_room(ow, 1)) != MGZ_OK)
                return st;
            ow->buf[ow->pos++] = (uint8_t)sym;
            ow->total++;
            continue;
        }
        if (sym == 256)
            return MGZ_OK;

        sym -= 257;
        if (sym >= 29)
            return MGZ_ERR_CODE;     /* 286 e 287 existem no código fixo, mas não são válidos */
        uint32_t extra;
        if ((st = br_get(br, LEN_EXTRA[sym], &extra)) != MGZ_OK)
            return st;
        unsigned len = LEN_BASE[sym] + extra;

        unsigned dsym;
        if ((st = huff_decode(br, dd, &dsym)) != MGZ_OK)
            return st;
        if (dsym >= 30)
            return MGZ_ERR_CODE;     /* 30 e 31 também são reservados */
        if ((st = br_get(br, DIST_EXTRA[dsym], &extra)) != MGZ_OK)
            return st;
        unsigned dist = DIST_BASE[dsym] + extra;
        if (dist > ow->total)
            return MGZ_ERR_DISTANCE;

        if ((st = ow_room(ow, len)) != MGZ_OK)
            return st;
        /* Cópia byte a byte: se dist < len a origem cresce junto com o
           destino, o que repete o padrão (dist 1 repete o último byte). */
        uint8_t *dst = ow->buf + ow->pos;
        const uint8_t *src = dst - dist;
        for (unsigned i = 0; i < len; i++)
            dst[i] = src[i];
        ow->pos += len;
        ow->total += len;
    }
}

/* Lê as tabelas de um bloco dinâmico (RFC 1951, seção 3.2.7). */
static mgz_status read_dynamic_tables(inflater *inf, mgz_block_info *info)
{
    bit_reader *br = inf->br;
    uint8_t lens[286 + 30];
    uint8_t cl_lens[19] = {0};
    uint32_t v;
    mgz_status st;

    if ((st = br_get(br, 5, &v)) != MGZ_OK)
        return st;
    unsigned nlen = v + 257;
    if ((st = br_get(br, 5, &v)) != MGZ_OK)
        return st;
    unsigned ndist = v + 1;
    if ((st = br_get(br, 4, &v)) != MGZ_OK)
        return st;
    unsigned ncode = v + 4;
    info->hlit = nlen;
    info->hdist = ndist;
    info->hclen = ncode;
    if (nlen > 286 || ndist > 30)
        return MGZ_ERR_CODE_LENGTHS;

    /* 1) Comprimentos (3 bits cada) do código que codifica os comprimentos. */
    for (unsigned i = 0; i < ncode; i++) {
        if ((st = br_get(br, 3, &v)) != MGZ_OK)
            return st;
        cl_lens[CL_ORDER[i]] = (uint8_t)v;
    }
    if ((st = huff_build(&inf->pre, cl_lens, 19, HUFF_COMPLETE)) != MGZ_OK)
        return st;

    /* 2) Comprimentos dos códigos de literal/comprimento e de distância,
          numa sequência única com repetições:
          0-15: comprimento literal; 16: repete o anterior 3-6 vezes;
          17: repete zero 3-10 vezes; 18: repete zero 11-138 vezes. */
    unsigned total = nlen + ndist, i = 0;
    while (i < total) {
        unsigned sym;
        if ((st = huff_decode(br, &inf->pre, &sym)) != MGZ_OK)
            return st;
        if (sym < 16) {
            lens[i++] = (uint8_t)sym;
            continue;
        }
        uint8_t value = 0;
        unsigned rep;
        if (sym == 16) {
            if (i == 0)
                return MGZ_ERR_CODE_LENGTHS;   /* não há anterior para repetir */
            value = lens[i - 1];
            if ((st = br_get(br, 2, &v)) != MGZ_OK)
                return st;
            rep = 3 + v;
        } else if (sym == 17) {
            if ((st = br_get(br, 3, &v)) != MGZ_OK)
                return st;
            rep = 3 + v;
        } else {
            if ((st = br_get(br, 7, &v)) != MGZ_OK)
                return st;
            rep = 11 + v;
        }
        if (i + rep > total)
            return MGZ_ERR_CODE_LENGTHS;
        while (rep-- > 0)
            lens[i++] = value;
    }
    if (lens[256] == 0)
        return MGZ_ERR_CODE_LENGTHS;           /* sem código para o fim do bloco */

    if ((st = huff_build(&inf->litlen, lens, nlen, HUFF_LITLEN)) != MGZ_OK)
        return st;
    return huff_build(&inf->dist, lens + nlen, ndist, HUFF_DISTANCE);
}

/* Código fixo (RFC 1951, 3.2.6): literais 0-143 com 8 bits, 144-255 com 9,
   256-279 com 7, 280-287 com 8; distâncias com 5 bits. */
static void build_fixed_tables(inflater *inf)
{
    uint8_t lens[288];
    unsigned i;
    for (i = 0; i < 144; i++)
        lens[i] = 8;
    for (; i < 256; i++)
        lens[i] = 9;
    for (; i < 280; i++)
        lens[i] = 7;
    for (; i < 288; i++)
        lens[i] = 8;
    huff_build(&inf->fixed_litlen, lens, 288, HUFF_LITLEN);
    for (i = 0; i < 32; i++)
        lens[i] = 5;
    huff_build(&inf->fixed_dist, lens, 32, HUFF_DISTANCE);
    inf->fixed_ready = 1;
}

mgz_status inflate_stream(inflater *inf)
{
    bit_reader *br = inf->br;
    unsigned index = 0;
    int final;

    do {
        mgz_block_info info;
        uint32_t header;
        mgz_status st;

        memset(&info, 0, sizeof info);
        info.member = inf->member;
        info.index = ++index;
        info.bit_start = br_bit_position(br);
        uint64_t out_start = inf->ow->total;

        if ((st = br_get(br, 3, &header)) != MGZ_OK)
            return st;
        final = (int)(header & 1u);
        info.final = final;
        info.type = (int)(header >> 1);

        switch (header >> 1) {
        case 0:
            st = inflate_stored(inf);
            break;
        case 1:
            if (!inf->fixed_ready)
                build_fixed_tables(inf);
            st = inflate_codes(inf, &inf->fixed_litlen, &inf->fixed_dist);
            break;
        case 2:
            st = read_dynamic_tables(inf, &info);
            if (st == MGZ_OK)
                st = inflate_codes(inf, &inf->litlen, &inf->dist);
            break;
        default:
            return MGZ_ERR_BLOCK_TYPE;
        }
        if (st != MGZ_OK)
            return st;

        info.bit_end = br_bit_position(br);
        info.out_bytes = inf->ow->total - out_start;
        if (inf->on_block != NULL)
            inf->on_block(inf->block_ctx, &info);
    } while (!final);
    return MGZ_OK;
}
