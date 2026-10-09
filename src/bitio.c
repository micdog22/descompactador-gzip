/* bitio.c: partes não críticas do leitor e do escritor de bits. */
#include "bitio.h"

#include <stdlib.h>
#include <string.h>

mgz_status br_init(bit_reader *br, mgz_read_fn read, void *ctx)
{
    memset(br, 0, sizeof *br);
    br->read = read;
    br->ctx = ctx;
    br->buf = malloc(BR_BUFSZ);
    return br->buf ? MGZ_OK : MGZ_ERR_MEMORY;
}

void br_free(bit_reader *br)
{
    free(br->buf);
    br->buf = NULL;
}

int br_refill(bit_reader *br)
{
    size_t got = 0;
    if (br->eof)
        return 0;
    if (br->read(br->ctx, br->buf, BR_BUFSZ, &got) != 0) {
        br->err = 1;
        br->eof = 1;
        return 0;
    }
    if (got == 0 || got > BR_BUFSZ) {
        br->eof = 1;
        return 0;
    }
    br->pos = 0;
    br->len = got;
    br->fetched += got;
    return 1;
}

mgz_status br_byte(bit_reader *br, uint8_t *out)
{
    uint32_t v;
    mgz_status st = br_get(br, 8, &v);
    if (st == MGZ_OK)
        *out = (uint8_t)v;
    return st;
}

mgz_status br_bytes(bit_reader *br, uint8_t *dst, size_t n)
{
    /* Primeiro os bytes que já estão no acumulador... */
    while (n > 0 && br->nbits >= 8) {
        *dst++ = (uint8_t)(br->bits & 0xFFu);
        br_drop(br, 8);
        n--;
    }
    /* ...depois cópia direta do bloco de entrada. */
    while (n > 0) {
        if (br->pos == br->len && !br_refill(br))
            return br_short_status(br);
        size_t take = br->len - br->pos;
        if (take > n)
            take = n;
        memcpy(dst, br->buf + br->pos, take);
        br->pos += take;
        dst += take;
        n -= take;
    }
    return MGZ_OK;
}

int br_has_byte(bit_reader *br)
{
    if (br->nbits >= 8)
        return 1;
    if (br->pos < br->len)
        return 1;
    return br_refill(br);
}

uint64_t br_bit_position(const bit_reader *br)
{
    uint64_t consumed_bytes = br->fetched - (uint64_t)(br->len - br->pos);
    return consumed_bytes * 8u - br->nbits;
}

mgz_status bw_init(bit_writer *bw, mgz_write_fn write, void *ctx)
{
    memset(bw, 0, sizeof *bw);
    bw->write = write;
    bw->ctx = ctx;
    bw->buf = malloc(BW_BUFSZ);
    return bw->buf ? MGZ_OK : MGZ_ERR_MEMORY;
}

void bw_free(bit_writer *bw)
{
    free(bw->buf);
    bw->buf = NULL;
}

void bw_flush(bit_writer *bw)
{
    if (bw->len > 0 && !bw->err && bw->write != NULL) {
        if (bw->write(bw->ctx, bw->buf, bw->len) != 0)
            bw->err = 1;
    }
    bw->len = 0;
}

void bw_bytes(bit_writer *bw, const uint8_t *data, size_t n)
{
    while (n > 0) {
        if (bw->len == BW_BUFSZ)
            bw_flush(bw);
        size_t take = BW_BUFSZ - bw->len;
        if (take > n)
            take = n;
        memcpy(bw->buf + bw->len, data, take);
        bw->len += take;
        bw->total += take;
        data += take;
        n -= take;
    }
}
