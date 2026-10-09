/*
 * gzip.c: o contêiner gzip (RFC 1952) em volta do DEFLATE.
 *
 *   +---+---+---+---+---+---+---+---+---+---+
 *   |ID1|ID2|CM |FLG|     MTIME     |XFL|OS |   10 bytes fixos
 *   +---+---+---+---+---+---+---+---+---+---+
 *   [FEXTRA: XLEN (2 bytes) + XLEN bytes]
 *   [FNAME: nome original terminado em zero]
 *   [FCOMMENT: comentário terminado em zero]
 *   [FHCRC: 2 bytes, CRC-32 do cabeçalho truncado em 16 bits]
 *   ... blocos DEFLATE ...
 *   +---+---+---+---+---+---+---+---+
 *   |     CRC32     |     ISIZE     |   CRC dos dados e tamanho mod 2^32
 *   +---+---+---+---+---+---+---+---+
 *
 * Todos os números são little-endian. Um arquivo pode ter vários membros
 * concatenados; o resultado é a concatenação das saídas.
 */
#include "crc32.h"
#include "deflate.h"
#include "inflate.h"
#include "mgz.h"

#include <stdlib.h>
#include <string.h>

#define ID1 0x1Fu
#define ID2 0x8Bu
#define CM_DEFLATE 8u

#define FTEXT 0x01u
#define FHCRC 0x02u
#define FEXTRA 0x04u
#define FNAME 0x08u
#define FCOMMENT 0x10u
#define FRESERVED 0xE0u

#define OS_UNKNOWN 255u

const char *mgz_strerror(mgz_status st)
{
    switch (st) {
    case MGZ_OK: return "sucesso";
    case MGZ_ERR_MEMORY: return "memória insuficiente";
    case MGZ_ERR_READ: return "erro de leitura";
    case MGZ_ERR_WRITE: return "erro de escrita";
    case MGZ_ERR_TRUNCATED: return "dados truncados: o arquivo terminou antes do esperado";
    case MGZ_ERR_HEADER: return "cabeçalho inválido: não é um arquivo gzip";
    case MGZ_ERR_METHOD: return "método de compressão desconhecido (o gzip só define o 8, deflate)";
    case MGZ_ERR_FLAGS: return "cabeçalho inválido: bits reservados de FLG ligados";
    case MGZ_ERR_HEADER_CRC: return "CRC do cabeçalho (FHCRC) não confere";
    case MGZ_ERR_CRC: return "CRC-32 não confere: os dados estão corrompidos";
    case MGZ_ERR_SIZE: return "tamanho original (ISIZE) não confere: os dados estão corrompidos";
    case MGZ_ERR_BLOCK_TYPE: return "tipo de bloco inválido (BTYPE 11 é reservado)";
    case MGZ_ERR_STORED_LEN: return "bloco armazenado inválido: LEN e NLEN não são complementares";
    case MGZ_ERR_CODE_LENGTHS: return "comprimentos de código inválidos na tabela Huffman";
    case MGZ_ERR_CODE: return "código Huffman inválido nos dados comprimidos";
    case MGZ_ERR_DISTANCE: return "distância muito para trás: aponta para antes do início dos dados";
    case MGZ_ERR_ARGUMENT: return "argumento inválido";
    }
    return "erro desconhecido";
}

/* ------------------------------------------------------------ compressão */

static void put_le32(bit_writer *bw, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    bw_bytes(bw, b, 4);
}

mgz_status mgz_gzip(const mgz_stream *io, const mgz_gzip_options *opt)
{
    mgz_gzip_options def = {6, NULL, NULL, 0, 0};
    bit_writer bw;
    uint32_t crc = 0, hcrc = 0;
    uint64_t total = 0;

    if (io == NULL || io->read == NULL)
        return MGZ_ERR_ARGUMENT;
    if (opt == NULL)
        opt = &def;
    if (opt->level < 0 || opt->level > 9)
        return MGZ_ERR_ARGUMENT;
    mgz_status st = bw_init(&bw, io->write, io->write_ctx);
    if (st != MGZ_OK)
        return st;

    uint8_t h[10];
    h[0] = ID1;
    h[1] = ID2;
    h[2] = CM_DEFLATE;
    h[3] = (uint8_t)((opt->name ? FNAME : 0u) | (opt->comment ? FCOMMENT : 0u) |
                     (opt->header_crc ? FHCRC : 0u));
    h[4] = (uint8_t)opt->mtime;
    h[5] = (uint8_t)(opt->mtime >> 8);
    h[6] = (uint8_t)(opt->mtime >> 16);
    h[7] = (uint8_t)(opt->mtime >> 24);
    h[8] = opt->level == 9 ? 2u : opt->level == 1 ? 4u : 0u;  /* XFL */
    h[9] = OS_UNKNOWN;
    bw_bytes(&bw, h, sizeof h);
    hcrc = mgz_crc32(hcrc, h, sizeof h);
    if (opt->name) {
        size_t n = strlen(opt->name) + 1;   /* inclui o zero final */
        bw_bytes(&bw, (const uint8_t *)opt->name, n);
        hcrc = mgz_crc32(hcrc, opt->name, n);
    }
    if (opt->comment) {
        size_t n = strlen(opt->comment) + 1;
        bw_bytes(&bw, (const uint8_t *)opt->comment, n);
        hcrc = mgz_crc32(hcrc, opt->comment, n);
    }
    if (opt->header_crc) {
        uint8_t c[2] = {(uint8_t)hcrc, (uint8_t)(hcrc >> 8)};
        bw_bytes(&bw, c, 2);
    }

    st = deflate_stream(&bw, io->read, io->read_ctx, opt->level, &crc, &total, NULL);
    if (st == MGZ_OK) {
        bw_align(&bw);
        put_le32(&bw, crc);
        put_le32(&bw, (uint32_t)total);   /* ISIZE: tamanho módulo 2^32 */
        bw_flush(&bw);
        if (bw.err)
            st = MGZ_ERR_WRITE;
    }
    bw_free(&bw);
    return st;
}

mgz_status mgz_deflate(const mgz_stream *io, int level)
{
    bit_writer bw;
    uint32_t crc;
    uint64_t total;

    if (io == NULL || io->read == NULL)
        return MGZ_ERR_ARGUMENT;
    mgz_status st = bw_init(&bw, io->write, io->write_ctx);
    if (st != MGZ_OK)
        return st;
    st = deflate_stream(&bw, io->read, io->read_ctx, level, &crc, &total, NULL);
    if (st == MGZ_OK) {
        bw_align(&bw);
        bw_flush(&bw);
        if (bw.err)
            st = MGZ_ERR_WRITE;
    }
    bw_free(&bw);
    return st;
}

/* ---------------------------------------------------------- descompressão */

typedef struct {
    bit_reader br;
    out_window ow;
    inflater inf;
} decoder;

static decoder *decoder_new(const mgz_stream *io)
{
    decoder *d = calloc(1, sizeof *d);
    if (d == NULL)
        return NULL;
    if (br_init(&d->br, io->read, io->read_ctx) != MGZ_OK ||
        ow_init(&d->ow, io->write, io->write_ctx) != MGZ_OK) {
        br_free(&d->br);
        ow_free(&d->ow);
        free(d);
        return NULL;
    }
    inflater_init(&d->inf, &d->br, &d->ow);
    d->inf.on_block = io->on_block;
    d->inf.block_ctx = io->block_ctx;
    return d;
}

static void decoder_free(decoder *d)
{
    if (d != NULL) {
        br_free(&d->br);
        ow_free(&d->ow);
        free(d);
    }
}

mgz_status mgz_inflate(const mgz_stream *io)
{
    if (io == NULL || io->read == NULL)
        return MGZ_ERR_ARGUMENT;
    decoder *d = decoder_new(io);
    if (d == NULL)
        return MGZ_ERR_MEMORY;
    mgz_status st = inflate_stream(&d->inf);
    if (st == MGZ_OK)
        st = ow_flush(&d->ow);
    decoder_free(d);
    return st;
}

/* Lê um byte do cabeçalho, acumulando o CRC do cabeçalho. */
static mgz_status header_byte(bit_reader *br, uint32_t *hcrc, uint8_t *out)
{
    mgz_status st = br_byte(br, out);
    if (st == MGZ_OK)
        *hcrc = mgz_crc32(*hcrc, out, 1);
    return st;
}

/* Lê uma string terminada em zero, guardando até cap - 1 bytes. */
static mgz_status header_string(bit_reader *br, uint32_t *hcrc, char *dst, size_t cap)
{
    size_t n = 0;
    for (;;) {
        uint8_t c;
        mgz_status st = header_byte(br, hcrc, &c);
        if (st != MGZ_OK)
            return st;
        if (c == 0)
            break;
        if (n + 1 < cap)
            dst[n++] = (char)c;
    }
    dst[n] = '\0';
    return MGZ_OK;
}

/*
 * Lê o restante do cabeçalho de um membro (ID1 e ID2 já foram lidos e
 * estão em hcrc).
 */
static mgz_status read_header(bit_reader *br, uint32_t hcrc, mgz_member_info *m)
{
    uint8_t b[8];
    mgz_status st;

    for (int i = 0; i < 8; i++)
        if ((st = header_byte(br, &hcrc, &b[i])) != MGZ_OK)
            return st;
    if (b[0] != CM_DEFLATE)
        return MGZ_ERR_METHOD;
    if (b[1] & FRESERVED)
        return MGZ_ERR_FLAGS;
    m->flags = b[1];
    m->mtime = (uint32_t)b[2] | ((uint32_t)b[3] << 8) | ((uint32_t)b[4] << 16) |
               ((uint32_t)b[5] << 24);
    m->xfl = b[6];
    m->os = b[7];

    if (m->flags & FEXTRA) {
        uint8_t lo, hi, skip;
        if ((st = header_byte(br, &hcrc, &lo)) != MGZ_OK ||
            (st = header_byte(br, &hcrc, &hi)) != MGZ_OK)
            return st;
        m->extra_len = (unsigned)lo | ((unsigned)hi << 8);
        for (unsigned i = 0; i < m->extra_len; i++)
            if ((st = header_byte(br, &hcrc, &skip)) != MGZ_OK)
                return st;
    }
    if (m->flags & FNAME) {
        if ((st = header_string(br, &hcrc, m->name, sizeof m->name)) != MGZ_OK)
            return st;
        m->has_name = 1;
    }
    if (m->flags & FCOMMENT) {
        if ((st = header_string(br, &hcrc, m->comment, sizeof m->comment)) != MGZ_OK)
            return st;
        m->has_comment = 1;
    }
    if (m->flags & FHCRC) {
        uint8_t lo, hi;
        if ((st = br_byte(br, &lo)) != MGZ_OK || (st = br_byte(br, &hi)) != MGZ_OK)
            return st;
        if ((((unsigned)hi << 8) | lo) != (hcrc & 0xFFFFu))
            return MGZ_ERR_HEADER_CRC;
        m->has_header_crc = 1;
    }
    return MGZ_OK;
}

static mgz_status read_le32(bit_reader *br, uint32_t *out)
{
    uint8_t b[4];
    mgz_status st = br_bytes(br, b, 4);
    if (st == MGZ_OK)
        *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
               ((uint32_t)b[3] << 24);
    return st;
}

static mgz_status gunzip_members(decoder *d, const mgz_stream *io, mgz_info *info)
{
    bit_reader *br = &d->br;
    out_window *ow = &d->ow;
    mgz_status st;

    for (unsigned number = 1;; number++) {
        mgz_member_info m;
        uint8_t id1, id2;
        uint32_t hcrc = 0;

        memset(&m, 0, sizeof m);
        m.number = number;
        if (number > 1) {
            /* Depois de um membro válido: fim, outro membro ou lixo. */
            if (!br_has_byte(br))
                return br->err ? MGZ_ERR_READ : MGZ_OK;
            if ((st = header_byte(br, &hcrc, &id1)) != MGZ_OK)
                return st;
            if (id1 != ID1 || !br_has_byte(br) ||
                (st = header_byte(br, &hcrc, &id2)) != MGZ_OK || id2 != ID2) {
                info->trailing_garbage = 1;
                return br->err ? MGZ_ERR_READ : MGZ_OK;
            }
        } else {
            if ((st = header_byte(br, &hcrc, &id1)) != MGZ_OK)
                return st;
            if (id1 != ID1)
                return MGZ_ERR_HEADER;
            if ((st = header_byte(br, &hcrc, &id2)) != MGZ_OK)
                return st;
            if (id2 != ID2)
                return MGZ_ERR_HEADER;
        }
        if ((st = read_header(br, hcrc, &m)) != MGZ_OK)
            return st;
        if (number == 1)
            info->first = m;
        if (io->on_member != NULL)
            io->on_member(io->member_ctx, MGZ_MEMBER_START, &m);

        /* Os dados comprimidos. */
        ow_reset(ow);
        d->inf.member = number;
        if ((st = inflate_stream(&d->inf)) != MGZ_OK)
            return st;
        if ((st = ow_flush(ow)) != MGZ_OK)
            return st;

        /* O trailer começa no próximo byte inteiro. */
        uint32_t crc, isize;
        br_align(br);
        if ((st = read_le32(br, &crc)) != MGZ_OK || (st = read_le32(br, &isize)) != MGZ_OK)
            return st;
        if (crc != ow->crc)
            return MGZ_ERR_CRC;
        if (isize != (uint32_t)ow->total)
            return MGZ_ERR_SIZE;

        m.crc32 = crc;
        m.isize = isize;
        m.out_bytes = ow->total;
        info->members = number;
        info->uncompressed_size += ow->total;
        info->compressed_size = br_bit_position(br) / 8u;
        if (io->on_member != NULL)
            io->on_member(io->member_ctx, MGZ_MEMBER_END, &m);
    }
}

mgz_status mgz_gunzip(const mgz_stream *io, mgz_info *info)
{
    mgz_info local;
    if (info == NULL)
        info = &local;
    memset(info, 0, sizeof *info);
    if (io == NULL || io->read == NULL)
        return MGZ_ERR_ARGUMENT;
    decoder *d = decoder_new(io);
    if (d == NULL)
        return MGZ_ERR_MEMORY;
    mgz_status st = gunzip_members(d, io, info);
    decoder_free(d);
    return st;
}

/* -------------------------------------------------------- atalhos em memória */

typedef struct {
    const uint8_t *data;
    size_t len, pos;
} mem_src;

typedef struct {
    uint8_t *data;
    size_t len, cap;
    int failed;
} mem_dst;

static int mem_read(void *ctx, uint8_t *buf, size_t cap, size_t *got)
{
    mem_src *s = ctx;
    size_t n = s->len - s->pos;
    if (n > cap)
        n = cap;
    if (n > 0)
        memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    *got = n;
    return 0;
}

static int mem_write(void *ctx, const uint8_t *buf, size_t len)
{
    mem_dst *d = ctx;
    if (len == 0)
        return 0;
    if (len > SIZE_MAX - d->len) {
        d->failed = 1;
        return 1;
    }
    if (d->len + len > d->cap) {
        size_t cap = d->cap ? d->cap : 4096;
        while (cap < d->len + len)
            cap = cap > SIZE_MAX / 2 ? d->len + len : cap * 2;
        uint8_t *p = realloc(d->data, cap);
        if (p == NULL) {
            d->failed = 1;
            return 1;
        }
        d->data = p;
        d->cap = cap;
    }
    memcpy(d->data + d->len, buf, len);
    d->len += len;
    return 0;
}

typedef enum { OP_GZIP, OP_GUNZIP, OP_DEFLATE, OP_INFLATE } mem_op;

static mgz_status run_buffer(mem_op op, const uint8_t *in, size_t len, int level,
                             uint8_t **out, size_t *out_len, mgz_info *info)
{
    mem_src src = {in, len, 0};
    mem_dst dst = {NULL, 0, 0, 0};
    mgz_stream io;
    mgz_status st;

    if ((in == NULL && len > 0) || out == NULL || out_len == NULL)
        return MGZ_ERR_ARGUMENT;
    memset(&io, 0, sizeof io);
    io.read = mem_read;
    io.read_ctx = &src;
    io.write = mem_write;
    io.write_ctx = &dst;

    switch (op) {
    case OP_GZIP: {
        mgz_gzip_options opt = {level, NULL, NULL, 0, 0};
        st = mgz_gzip(&io, &opt);
        break;
    }
    case OP_GUNZIP:
        st = mgz_gunzip(&io, info);
        break;
    case OP_DEFLATE:
        st = mgz_deflate(&io, level);
        break;
    default:
        st = mgz_inflate(&io);
        break;
    }
    if (st == MGZ_OK && dst.failed)
        st = MGZ_ERR_MEMORY;
    if (st == MGZ_ERR_WRITE && dst.failed)
        st = MGZ_ERR_MEMORY;
    if (st != MGZ_OK) {
        free(dst.data);
        *out = NULL;
        *out_len = 0;
        return st;
    }
    if (dst.data == NULL) {
        dst.data = malloc(1);   /* saída vazia, mas um ponteiro válido */
        if (dst.data == NULL)
            return MGZ_ERR_MEMORY;
    }
    *out = dst.data;
    *out_len = dst.len;
    return MGZ_OK;
}

mgz_status mgz_gzip_buffer(const uint8_t *in, size_t len, int level, uint8_t **out, size_t *out_len)
{
    return run_buffer(OP_GZIP, in, len, level, out, out_len, NULL);
}

mgz_status mgz_gunzip_buffer(const uint8_t *in, size_t len, uint8_t **out, size_t *out_len,
                             mgz_info *info)
{
    return run_buffer(OP_GUNZIP, in, len, 0, out, out_len, info);
}

mgz_status mgz_deflate_buffer(const uint8_t *in, size_t len, int level, uint8_t **out,
                              size_t *out_len)
{
    return run_buffer(OP_DEFLATE, in, len, level, out, out_len, NULL);
}

mgz_status mgz_inflate_buffer(const uint8_t *in, size_t len, uint8_t **out, size_t *out_len)
{
    return run_buffer(OP_INFLATE, in, len, 0, out, out_len, NULL);
}
