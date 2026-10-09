/*
 * test_unidade.c: testes de unidade da biblioteca.
 *
 * Cobre o leitor e o escritor de bits, a tabela de Huffman, o package-merge,
 * o CRC-32, cada tipo de bloco DEFLATE (com vetores gerados pelo zlib) e os
 * erros que o decodificador precisa detectar.
 */
#include "teste.h"

#include "bitio.h"
#include "crc32.h"
#include "deflate.h"
#include "huffman.h"
#include "mgz.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ utilidades */

static uint64_t rng_state;

static void rng_seed(uint64_t seed)
{
    rng_state = seed;
}

/* splitmix64: pequeno, rápido e com boa distribuição. */
static uint64_t rng_next(void)
{
    uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static uint32_t rng_below(uint32_t n)
{
    return (uint32_t)(rng_next() % n);
}

typedef struct {
    const uint8_t *data;
    size_t len, pos, chunk;   /* chunk > 0 limita cada leitura */
} src_t;

static int src_read(void *ctx, uint8_t *buf, size_t cap, size_t *got)
{
    src_t *s = ctx;
    size_t n = s->len - s->pos;
    if (n > cap)
        n = cap;
    if (s->chunk && n > s->chunk)
        n = s->chunk;
    if (n > 0)
        memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    *got = n;
    return 0;
}

typedef struct {
    uint8_t *data;
    size_t len, cap;
} dst_t;

static int dst_write(void *ctx, const uint8_t *buf, size_t len)
{
    dst_t *d = ctx;
    if (d->len + len > d->cap) {
        size_t cap = d->cap ? d->cap : 1024;
        while (cap < d->len + len)
            cap *= 2;
        uint8_t *p = realloc(d->data, cap);
        if (p == NULL)
            return 1;
        d->data = p;
        d->cap = cap;
    }
    memcpy(d->data + d->len, buf, len);
    d->len += len;
    return 0;
}

static void dst_free(dst_t *d)
{
    free(d->data);
    d->data = NULL;
    d->len = d->cap = 0;
}

static void count_blocks(void *ctx, const mgz_block_info *b)
{
    unsigned *counts = ctx;
    counts[b->type]++;
}

static mgz_status run_gunzip(const uint8_t *in, size_t len, size_t chunk, dst_t *out, mgz_info *info)
{
    src_t src = {in, len, 0, chunk};
    mgz_stream io;
    memset(&io, 0, sizeof io);
    io.read = src_read;
    io.read_ctx = &src;
    io.write = dst_write;
    io.write_ctx = out;
    return mgz_gunzip(&io, info);
}

static mgz_status run_inflate(const uint8_t *in, size_t len, size_t chunk, dst_t *out, unsigned *counts)
{
    src_t src = {in, len, 0, chunk};
    mgz_stream io;
    memset(&io, 0, sizeof io);
    io.read = src_read;
    io.read_ctx = &src;
    io.write = dst_write;
    io.write_ctx = out;
    if (counts != NULL) {
        io.on_block = count_blocks;
        io.block_ctx = counts;
    }
    return mgz_inflate(&io);
}

/* Comprime com deflate_stream direto, para ler as estatísticas. */
static mgz_status run_deflate(const uint8_t *in, size_t len, int level, size_t chunk, dst_t *out,
                              deflate_stats *stats)
{
    src_t src = {in, len, 0, chunk};
    bit_writer bw;
    uint32_t crc;
    uint64_t total;
    if (bw_init(&bw, dst_write, out) != MGZ_OK)
        return MGZ_ERR_MEMORY;
    mgz_status st = deflate_stream(&bw, src_read, &src, level, &crc, &total, stats);
    bw_align(&bw);
    bw_flush(&bw);
    bw_free(&bw);
    if (st == MGZ_OK && (total != len || crc != mgz_crc32(0, in, len)))
        st = MGZ_ERR_CRC;
    return st;
}

/* Escreve um código de Huffman (MSB primeiro, como a RFC descreve). */
static void put_code(bit_writer *bw, unsigned code, unsigned len)
{
    bw_put(bw, huff_reverse(code, len), len);
}

/* Símbolo de literal/comprimento no código fixo. */
static void fixed_lit(bit_writer *bw, unsigned sym)
{
    if (sym < 144)
        put_code(bw, 0x30 + sym, 8);
    else if (sym < 256)
        put_code(bw, 0x190 + sym - 144, 9);
    else if (sym < 280)
        put_code(bw, sym - 256, 7);
    else
        put_code(bw, 0xC0 + sym - 280, 8);
}

/* ---------------------------------------------------------- geradores */

static void gen_random(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)rng_next();
}

static void gen_text(uint8_t *buf, size_t n)
{
    static const char *words[] = {"compressão", "dados", "bloco", "árvore", "Huffman", "janela",
                                  "distância", "o", "a", "de", "que", "não", "com", "para",
                                  "código", "bits", "byte", "arquivo", "gzip", "LZ77"};
    size_t i = 0;
    while (i < n) {
        const char *w = words[rng_below(20)];
        size_t wl = strlen(w);
        for (size_t k = 0; k < wl && i < n; k++)
            buf[i++] = (uint8_t)w[k];
        if (i < n)
            buf[i++] = rng_below(12) == 0 ? '\n' : ' ';
    }
}

static void gen_zeros(uint8_t *buf, size_t n)
{
    memset(buf, 0, n);
}

/* Bloco aleatório de 32 KiB repetido: força distâncias de exatamente 32768. */
static void gen_period(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = i < 32768 ? (uint8_t)rng_next() : buf[i - 32768];
}

/* Trechos alternados de texto, aleatório e zeros. */
static void gen_mixed(uint8_t *buf, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t len = 1000 + rng_below(20000);
        if (len > n - i)
            len = n - i;
        switch (rng_below(3)) {
        case 0: gen_text(buf + i, len); break;
        case 1: gen_random(buf + i, len); break;
        default: gen_zeros(buf + i, len); break;
        }
        i += len;
    }
}

/* ------------------------------------------------------------------ CRC */

static void test_crc32(void)
{
    SECTION("CRC-32");
    CHECK_EQ(mgz_crc32(0, "123456789", 9), 0xCBF43926u);
    CHECK_EQ(mgz_crc32(0, "", 0), 0);
    CHECK_EQ(mgz_crc32(0, "The quick brown fox jumps over the lazy dog", 43), 0x414FA339u);
    CHECK_EQ(crc32_bitwise(0, "123456789", 9), 0xCBF43926u);

    /* Cada entrada da tabela confere com a divisão bit a bit. */
    int all_entries = 1;
    for (unsigned n = 0; n < 256; n++) {
        uint8_t b = (uint8_t)n;
        if (mgz_crc32(0, &b, 1) != crc32_bitwise(0, &b, 1))
            all_entries = 0;
    }
    CHECK(all_entries);
    CHECK_EQ(crc32_table_entry(1), 0x77073096u);
    CHECK_EQ(crc32_table_entry(255), 0x2D02EF8Du);

    /* Incremental: CRC(a + b) = CRC(CRC(a), b). */
    uint8_t data[5000];
    rng_seed(1);
    gen_random(data, sizeof data);
    uint32_t whole = mgz_crc32(0, data, sizeof data);
    CHECK_EQ(mgz_crc32(mgz_crc32(0, data, 1234), data + 1234, sizeof data - 1234), whole);
    CHECK_EQ(crc32_bitwise(0, data, sizeof data), whole);
}

/* --------------------------------------------------------------- bits */

static void test_bit_reader(void)
{
    SECTION("leitor de bits");
    const uint8_t data[] = {0xA5, 0x3C, 0xFF, 0x00, 0x81};
    src_t src = {data, sizeof data, 0, 1};   /* um byte por leitura */
    bit_reader br;
    uint32_t v = 0;
    uint8_t b = 0;

    CHECK_EQ(br_init(&br, src_read, &src), MGZ_OK);
    /* 0xA5 = 1010 0101: os bits saem do menos significativo. */
    CHECK_EQ(br_get(&br, 1, &v), MGZ_OK);
    CHECK_EQ(v, 1);
    CHECK_EQ(br_get(&br, 2, &v), MGZ_OK);
    CHECK_EQ(v, 2);
    CHECK_EQ(br_get(&br, 5, &v), MGZ_OK);
    CHECK_EQ(v, 0x14);
    CHECK_EQ(br_bit_position(&br), 8);
    /* Campo que atravessa a fronteira entre 0x3C e 0xFF. */
    CHECK_EQ(br_get(&br, 4, &v), MGZ_OK);
    CHECK_EQ(v, 0xC);
    CHECK_EQ(br_get(&br, 8, &v), MGZ_OK);
    CHECK_EQ(v, 0xF3);
    br_align(&br);
    CHECK_EQ(br_bit_position(&br), 24);
    CHECK_EQ(br_byte(&br, &b), MGZ_OK);
    CHECK_EQ(b, 0x00);
    CHECK_EQ(br_byte(&br, &b), MGZ_OK);
    CHECK_EQ(b, 0x81);
    CHECK_EQ(br_has_byte(&br), 0);
    CHECK_EQ(br_byte(&br, &b), MGZ_ERR_TRUNCATED);
    br_free(&br);

    /* br_bytes depois de alinhar devolve os bytes na ordem certa. */
    uint8_t seq[100], got[10];
    for (int i = 0; i < 100; i++)
        seq[i] = (uint8_t)i;
    src_t s2 = {seq, sizeof seq, 0, 3};
    CHECK_EQ(br_init(&br, src_read, &s2), MGZ_OK);
    CHECK_EQ(br_get(&br, 3, &v), MGZ_OK);
    br_align(&br);
    CHECK_EQ(br_bytes(&br, got, 10), MGZ_OK);
    CHECK_EQ(got[0], 1);
    CHECK_EQ(got[9], 10);
    CHECK_EQ(br_bit_position(&br), 88);
    CHECK_EQ(br_bytes(&br, got, 10), MGZ_OK);
    CHECK_EQ(got[0], 11);
    br_free(&br);

    /* No fim dos dados, olhar adiante completa com zeros. */
    const uint8_t one[] = {0x01};
    src_t s3 = {one, 1, 0, 0};
    CHECK_EQ(br_init(&br, src_read, &s3), MGZ_OK);
    br_fill(&br);
    CHECK_EQ(br.nbits, 8);
    CHECK_EQ(br_peek(&br, 16), 1);
    CHECK_EQ(br_need(&br, 9), MGZ_ERR_TRUNCATED);
    br_free(&br);
}

static void test_bit_writer(void)
{
    SECTION("escritor de bits (ida e volta)");
    enum { N = 20000 };
    static uint32_t values[N];
    static uint8_t widths[N];
    dst_t out = {NULL, 0, 0};
    bit_writer bw;

    rng_seed(2);
    CHECK_EQ(bw_init(&bw, dst_write, &out), MGZ_OK);
    for (int i = 0; i < N; i++) {
        widths[i] = (uint8_t)(1 + rng_below(32));
        values[i] = (uint32_t)rng_next() & (uint32_t)((UINT64_C(1) << widths[i]) - 1u);
        bw_put(&bw, values[i], widths[i]);
    }
    uint64_t bits = bw_bit_count(&bw);
    bw_align(&bw);
    const uint8_t tail[] = {'f', 'i', 'm'};
    bw_bytes(&bw, tail, 3);
    bw_flush(&bw);
    CHECK_EQ(out.len, (bits + 7) / 8 + 3);

    bit_reader br;
    src_t src = {out.data, out.len, 0, 0};
    CHECK_EQ(br_init(&br, src_read, &src), MGZ_OK);
    int ok = 1;
    for (int i = 0; i < N; i++) {
        uint32_t v;
        if (br_get(&br, widths[i], &v) != MGZ_OK || v != values[i])
            ok = 0;
    }
    CHECK(ok);
    br_align(&br);
    uint8_t got[3];
    CHECK_EQ(br_bytes(&br, got, 3), MGZ_OK);
    CHECK_MEM(got, tail, 3);
    br_free(&br);
    bw_free(&bw);
    dst_free(&out);
}

/* ------------------------------------------------------------ Huffman */

static void test_canonical_codes(void)
{
    SECTION("códigos canônicos (exemplo da RFC 1951)");
    /* A B C D E F G H com comprimentos (3, 3, 3, 3, 3, 2, 4, 4). */
    const uint8_t lens[8] = {3, 3, 3, 3, 3, 2, 4, 4};
    const uint16_t expected[8] = {2, 3, 4, 5, 6, 0, 14, 15};  /* 010 011 100 101 110 00 1110 1111 */
    uint16_t codes[8];
    huff_canonical_codes(lens, 8, codes);
    for (int i = 0; i < 8; i++)
        CHECK_EQ(codes[i], expected[i]);

    /* Código fixo: 0 -> 00110000, 144 -> 110010000, 256 -> 0000000, 280 -> 11000000. */
    uint8_t fl[288];
    uint16_t fc[288];
    for (int i = 0; i < 288; i++)
        fl[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    huff_canonical_codes(fl, 288, fc);
    CHECK_EQ(fc[0], 0x30);
    CHECK_EQ(fc[143], 0xBF);
    CHECK_EQ(fc[144], 0x190);
    CHECK_EQ(fc[255], 0x1FF);
    CHECK_EQ(fc[256], 0);
    CHECK_EQ(fc[279], 23);
    CHECK_EQ(fc[280], 0xC0);
    CHECK_EQ(fc[287], 0xC7);
    CHECK_EQ(huff_reverse(0x30, 8), 0x0C);
    CHECK_EQ(huff_reverse(1, 15), 0x4000);
}

static void test_huffman_rules(void)
{
    SECTION("validação de comprimentos de código");
    static huff_decoder d;
    const uint8_t over[3] = {1, 1, 1};        /* três códigos de 1 bit: impossível */
    const uint8_t incomplete[3] = {1, 2, 0};  /* sobra o código 11 */
    const uint8_t single[3] = {0, 1, 0};      /* um único código de 1 bit */
    const uint8_t none[3] = {0, 0, 0};
    const uint8_t complete[4] = {1, 2, 3, 3};
    const uint8_t too_long[2] = {16, 1};

    CHECK_EQ(huff_build(&d, over, 3, HUFF_COMPLETE), MGZ_ERR_CODE_LENGTHS);
    CHECK_EQ(huff_build(&d, over, 3, HUFF_LITLEN), MGZ_ERR_CODE_LENGTHS);
    CHECK_EQ(huff_build(&d, over, 3, HUFF_DISTANCE), MGZ_ERR_CODE_LENGTHS);
    CHECK_EQ(huff_build(&d, incomplete, 3, HUFF_COMPLETE), MGZ_ERR_CODE_LENGTHS);
    CHECK_EQ(huff_build(&d, incomplete, 3, HUFF_LITLEN), MGZ_ERR_CODE_LENGTHS);
    CHECK_EQ(huff_build(&d, incomplete, 3, HUFF_DISTANCE), MGZ_ERR_CODE_LENGTHS);
    CHECK_EQ(huff_build(&d, single, 3, HUFF_COMPLETE), MGZ_ERR_CODE_LENGTHS);
    CHECK_EQ(huff_build(&d, single, 3, HUFF_LITLEN), MGZ_OK);
    CHECK_EQ(huff_build(&d, single, 3, HUFF_DISTANCE), MGZ_OK);
    CHECK_EQ(huff_build(&d, none, 3, HUFF_COMPLETE), MGZ_ERR_CODE_LENGTHS);
    CHECK_EQ(huff_build(&d, none, 3, HUFF_LITLEN), MGZ_ERR_CODE_LENGTHS);
    CHECK_EQ(huff_build(&d, none, 3, HUFF_DISTANCE), MGZ_OK);
    CHECK_EQ(huff_build(&d, complete, 4, HUFF_COMPLETE), MGZ_OK);
    CHECK_EQ(huff_build(&d, too_long, 2, HUFF_LITLEN), MGZ_ERR_CODE_LENGTHS);

    /* Com um único código de 1 bit, o bit 1 não decodifica nada. */
    const uint8_t ones[2] = {0xFF, 0xFF};
    src_t src = {ones, 2, 0, 0};
    bit_reader br;
    unsigned sym = 0;
    CHECK_EQ(huff_build(&d, single, 3, HUFF_DISTANCE), MGZ_OK);
    CHECK_EQ(br_init(&br, src_read, &src), MGZ_OK);
    CHECK_EQ(huff_decode(&br, &d, &sym), MGZ_ERR_CODE);
    br_free(&br);
    /* Com o bit 0, sai o símbolo 1 (o único com código). */
    const uint8_t zeros[2] = {0x00, 0x00};
    src_t src0 = {zeros, 2, 0, 0};
    CHECK_EQ(br_init(&br, src_read, &src0), MGZ_OK);
    CHECK_EQ(huff_decode(&br, &d, &sym), MGZ_OK);
    CHECK_EQ(sym, 1);
    br_free(&br);
}

/* Custo do Huffman clássico (sem limite): soma dos nós internos. */
static uint64_t huffman_cost(const uint32_t *freq, unsigned n)
{
    uint64_t w[HUFF_MAX_SYMS], cost = 0;
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++)
        if (freq[i])
            w[m++] = freq[i];
    while (m > 1) {
        unsigned a = 0, b = 1;
        if (w[b] < w[a]) {
            a = 1;
            b = 0;
        }
        for (unsigned i = 2; i < m; i++) {
            if (w[i] < w[a]) {
                b = a;
                a = i;
            } else if (w[i] < w[b]) {
                b = i;
            }
        }
        uint64_t s = w[a] + w[b];
        cost += s;
        unsigned lo = a < b ? a : b, hi = a < b ? b : a;
        w[lo] = s;
        w[hi] = w[m - 1];
        m--;
    }
    return cost;
}

static uint64_t code_cost(const uint32_t *freq, const uint8_t *lens, unsigned n)
{
    uint64_t c = 0;
    for (unsigned i = 0; i < n; i++)
        c += (uint64_t)freq[i] * lens[i];
    return c;
}

/* Menor custo possível por força bruta (n pequeno, comprimentos 1..L). */
static uint64_t brute_force_cost(const uint32_t *freq, unsigned n, unsigned limit)
{
    uint8_t lens[8] = {0};
    uint64_t best = UINT64_MAX;
    for (unsigned i = 0; i < n; i++)
        lens[i] = 1;
    for (;;) {
        uint64_t kraft = 0;
        for (unsigned i = 0; i < n; i++)
            kraft += UINT64_C(1) << (limit - lens[i]);
        if (kraft == (UINT64_C(1) << limit)) {
            uint64_t c = code_cost(freq, lens, n);
            if (c < best)
                best = c;
        }
        unsigned k = 0;
        while (k < n && lens[k] == limit)
            lens[k++] = 1;
        if (k == n)
            break;
        lens[k]++;
    }
    return best;
}

static int kraft_complete(const uint8_t *lens, unsigned n, unsigned limit)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; i++)
        if (lens[i])
            sum += UINT64_C(1) << (limit - lens[i]);
    return sum == (UINT64_C(1) << limit);
}

static void test_package_merge(void)
{
    SECTION("package-merge: códigos ótimos limitados");
    uint32_t freq[HUFF_MAX_SYMS];
    uint8_t lens[HUFF_MAX_SYMS];

    /* Casos triviais. */
    memset(freq, 0, sizeof freq);
    huff_limited_lengths(freq, 10, 15, lens);
    CHECK_EQ(lens[0], 0);
    freq[7] = 99;
    huff_limited_lengths(freq, 10, 15, lens);
    CHECK_EQ(lens[7], 1);

    /* Sem limite ativo, o custo é igual ao do Huffman clássico. */
    rng_seed(3);
    int same_cost = 1, complete = 1, zero_ok = 1;
    for (int round = 0; round < 200; round++) {
        unsigned n = 2 + rng_below(287);
        for (unsigned i = 0; i < n; i++)
            freq[i] = rng_below(4) == 0 ? 0 : 1 + rng_below(1000);
        freq[0] = 1 + freq[0];
        freq[n - 1] = 1 + freq[n - 1];
        huff_limited_lengths(freq, n, 15, lens);
        if (code_cost(freq, lens, n) != huffman_cost(freq, n))
            same_cost = 0;
        if (!kraft_complete(lens, n, 15))
            complete = 0;
        for (unsigned i = 0; i < n; i++)
            if ((freq[i] == 0) != (lens[i] == 0))
                zero_ok = 0;
    }
    CHECK(same_cost);
    CHECK(complete);
    CHECK(zero_ok);

    /* Com limite apertado, confere com a força bruta. */
    int optimal = 1;
    for (int round = 0; round < 60; round++) {
        unsigned n = 4 + rng_below(4);          /* 4 a 7 símbolos */
        unsigned limit = n <= 4 ? 2 : 3;
        for (unsigned i = 0; i < n; i++)
            freq[i] = 1 + rng_below(round < 30 ? 10 : 100000);
        huff_limited_lengths(freq, n, limit, lens);
        int within = 1;
        for (unsigned i = 0; i < n; i++)
            if (lens[i] < 1 || lens[i] > limit)
                within = 0;
        if (!within || !kraft_complete(lens, n, limit) ||
            code_cost(freq, lens, n) != brute_force_cost(freq, n, limit))
            optimal = 0;
    }
    CHECK(optimal);

    /* Frequências de Fibonacci: o Huffman sem limite passaria de 15 bits. */
    uint32_t a = 1, b = 1;
    for (unsigned i = 0; i < 30; i++) {
        freq[i] = a;
        uint32_t c = a + b;
        a = b;
        b = c;
    }
    huff_limited_lengths(freq, 30, 15, lens);
    unsigned maxlen = 0;
    for (unsigned i = 0; i < 30; i++)
        if (lens[i] > maxlen)
            maxlen = lens[i];
    CHECK_EQ(maxlen, 15);
    CHECK(kraft_complete(lens, 30, 15));
    CHECK(code_cost(freq, lens, 30) >= huffman_cost(freq, 30));
    huff_limited_lengths(freq, 19, 7, lens);
    CHECK(kraft_complete(lens, 19, 7));
    for (unsigned i = 0; i < 19; i++)
        CHECK(lens[i] >= 1 && lens[i] <= 7);
}

static void test_huffman_roundtrip(void)
{
    SECTION("Huffman: codifica e decodifica (caminhos rápido e lento)");
    static huff_decoder d;
    uint32_t freq[HUFF_MAX_SYMS];
    uint8_t lens[HUFF_MAX_SYMS];
    uint16_t codes[HUFF_MAX_SYMS];
    enum { N = 6000 };
    static unsigned syms[N];

    rng_seed(4);
    int ok = 1;
    for (int round = 0; round < 6; round++) {
        unsigned n = 288;
        if (round < 3) {
            for (unsigned i = 0; i < n; i++)
                freq[i] = 1 + rng_below(500);
        } else {
            /* Pesos de Fibonacci em 40 símbolos: códigos de até 15 bits. */
            uint32_t a = 1, b = 1;
            memset(freq, 0, sizeof freq);
            for (unsigned i = 0; i < 40; i++) {
                freq[(i * 7) % n] = a;
                uint32_t c = a + b;
                a = b;
                b = c > 100000000u ? 100000000u : c;   /* evita estouro */
            }
        }
        huff_limited_lengths(freq, n, 15, lens);
        huff_canonical_codes(lens, n, codes);
        if (huff_build(&d, lens, n, HUFF_LITLEN) != MGZ_OK) {
            ok = 0;
            continue;
        }
        dst_t out = {NULL, 0, 0};
        bit_writer bw;
        bw_init(&bw, dst_write, &out);
        for (int i = 0; i < N; i++) {
            unsigned s;
            do
                s = rng_below(n);
            while (lens[s] == 0);
            syms[i] = s;
            put_code(&bw, codes[s], lens[s]);
        }
        bw_align(&bw);
        bw_flush(&bw);
        bit_reader br;
        src_t src = {out.data, out.len, 0, 5};
        br_init(&br, src_read, &src);
        for (int i = 0; i < N; i++) {
            unsigned s;
            if (huff_decode(&br, &d, &s) != MGZ_OK || s != syms[i]) {
                ok = 0;
                break;
            }
        }
        br_free(&br);
        bw_free(&bw);
        dst_free(&out);
    }
    CHECK(ok);
}

/* ------------------------------------------------------- blocos DEFLATE */

/* Texto e vetor dinâmico gerados com o zlib (nível 9, janela de 32 KiB). */
#define DYN_SENTENCE                                                                   \
    "Em um belo dia de sol, o programador abriu o editor e escreveu: a compressao " \
    "DEFLATE combina LZ77 com codigos de Huffman canonicos. "

static const uint8_t DYN_VECTOR[] = {
    0xed, 0x8e, 0x31, 0x0e, 0xc2, 0x30, 0x10, 0x04, 0xbf, 0xb2, 0x0f, 0x40, 0xb4, 0x91, 0xd2,
    0x21, 0x61, 0x94, 0x22, 0x25, 0x15, 0xdd, 0xd9, 0xbe, 0x44, 0x27, 0xc5, 0xbe, 0xe8, 0x0e,
    0xf3, 0x7e, 0x6c, 0xde, 0x41, 0xb1, 0xc5, 0x4c, 0xb1, 0x9a, 0x50, 0xd0, 0x0a, 0x22, 0x1f,
    0x8a, 0x2c, 0x84, 0xcc, 0x70, 0x3d, 0x2e, 0x50, 0x9c, 0xa6, 0xbb, 0x51, 0xa1, 0xac, 0x06,
    0x8a, 0x26, 0xad, 0x3b, 0xce, 0xf2, 0xee, 0xc8, 0x60, 0x4f, 0xc6, 0x1f, 0x6e, 0x33, 0x08,
    0x49, 0xcb, 0x69, 0xec, 0x4e, 0x8a, 0x7b, 0x78, 0xac, 0xb7, 0x67, 0x18, 0x2a, 0x4a, 0x25,
    0xac, 0xaf, 0x69, 0x1a, 0xd0, 0x97, 0x65, 0x57, 0x1f, 0xf7, 0x4b, 0xdb, 0xb6, 0x42, 0x15,
    0x89, 0xaa, 0x56, 0x49, 0xea, 0x57, 0x84, 0x7f, 0xc3, 0xaf, 0xe1, 0x0b};

static void test_block_types(void)
{
    SECTION("os três tipos de bloco");
    dst_t out = {NULL, 0, 0};
    unsigned counts[3] = {0, 0, 0};

    /* Armazenado, montado à mão: BFINAL=1, BTYPE=00, LEN=5, NLEN=~5. */
    const uint8_t stored[] = {0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'};
    CHECK_EQ(run_inflate(stored, sizeof stored, 0, &out, counts), MGZ_OK);
    CHECK_EQ(out.len, 5);
    CHECK_MEM(out.data, "hello", 5);
    CHECK_EQ(counts[MGZ_BLOCK_STORED], 1);
    dst_free(&out);

    /* Huffman fixo, vetor do zlib para "hello". */
    const uint8_t fixed[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00};
    memset(counts, 0, sizeof counts);
    CHECK_EQ(run_inflate(fixed, sizeof fixed, 1, &out, counts), MGZ_OK);
    CHECK_EQ(out.len, 5);
    CHECK_MEM(out.data, "hello", 5);
    CHECK_EQ(counts[MGZ_BLOCK_FIXED], 1);
    dst_free(&out);

    /* Huffman fixo com referências (comprimento, distância). */
    const uint8_t fixed_ref[] = {0x4b, 0x4c, 0x4a, 0x4e, 0xc4, 0x86, 0x00};
    CHECK_EQ(run_inflate(fixed_ref, sizeof fixed_ref, 0, &out, NULL), MGZ_OK);
    CHECK_EQ(out.len, 24);
    CHECK_MEM(out.data, "abcabcabcabcabcabcabcabc", 24);
    dst_free(&out);

    /* Huffman dinâmico, vetor do zlib. */
    const char *text = DYN_SENTENCE DYN_SENTENCE DYN_SENTENCE;
    memset(counts, 0, sizeof counts);
    CHECK_EQ(run_inflate(DYN_VECTOR, sizeof DYN_VECTOR, 2, &out, counts), MGZ_OK);
    CHECK_EQ(out.len, strlen(text));
    CHECK_MEM(out.data, text, strlen(text));
    CHECK_EQ(counts[MGZ_BLOCK_DYNAMIC], 1);
    dst_free(&out);

    /* Vários blocos: armazenado (não final) seguido de fixo (final). */
    uint8_t multi[sizeof stored + sizeof fixed];
    memcpy(multi, stored, sizeof stored);
    multi[0] = 0x00;   /* BFINAL = 0 */
    memcpy(multi + sizeof stored, fixed, sizeof fixed);
    memset(counts, 0, sizeof counts);
    CHECK_EQ(run_inflate(multi, sizeof multi, 0, &out, counts), MGZ_OK);
    CHECK_EQ(out.len, 10);
    CHECK_MEM(out.data, "hellohello", 10);
    CHECK_EQ(counts[MGZ_BLOCK_STORED] + counts[MGZ_BLOCK_FIXED], 2);
    dst_free(&out);
}

/* Monta um fluxo DEFLATE de teste em memória. */
typedef struct {
    dst_t out;
    bit_writer bw;
} craft;

static void craft_begin(craft *c)
{
    memset(c, 0, sizeof *c);
    bw_init(&c->bw, dst_write, &c->out);
}

static void craft_end(craft *c)
{
    bw_align(&c->bw);
    bw_flush(&c->bw);
}

static void craft_free(craft *c)
{
    bw_free(&c->bw);
    dst_free(&c->out);
}

static mgz_status inflate_crafted(craft *c, dst_t *out)
{
    craft_end(c);
    return run_inflate(c->out.data, c->out.len, 0, out, NULL);
}

/* O mesmo, mas a fonte entrega um byte por leitura. */
static mgz_status inflate_crafted_slowly(craft *c, dst_t *out)
{
    craft_end(c);
    return run_inflate(c->out.data, c->out.len, 1, out, NULL);
}

static void test_inflate_errors(void)
{
    SECTION("erros do decodificador");
    dst_t out = {NULL, 0, 0};
    craft c;

    const uint8_t btype3[] = {0x07};
    CHECK_EQ(run_inflate(btype3, 1, 0, &out, NULL), MGZ_ERR_BLOCK_TYPE);
    dst_free(&out);

    const uint8_t badlen[] = {0x01, 0x05, 0x00, 0xFB, 0xFF, 'h', 'e', 'l', 'l', 'o'};
    CHECK_EQ(run_inflate(badlen, sizeof badlen, 0, &out, NULL), MGZ_ERR_STORED_LEN);
    dst_free(&out);

    const uint8_t short_stored[] = {0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e'};
    CHECK_EQ(run_inflate(short_stored, sizeof short_stored, 0, &out, NULL), MGZ_ERR_TRUNCATED);
    dst_free(&out);

    CHECK_EQ(run_inflate(NULL, 0, 0, &out, NULL), MGZ_ERR_TRUNCATED);
    dst_free(&out);

    /* Bloco não final e depois nada: falta o bloco final. */
    const uint8_t no_final[] = {0x00, 0x01, 0x00, 0xFE, 0xFF, 'x'};
    CHECK_EQ(run_inflate(no_final, sizeof no_final, 0, &out, NULL), MGZ_ERR_TRUNCATED);
    dst_free(&out);

    /* Distância 2 com só 1 byte produzido. */
    craft_begin(&c);
    bw_put(&c.bw, 1, 1);
    bw_put(&c.bw, 1, 2);
    fixed_lit(&c.bw, 'a');
    fixed_lit(&c.bw, 257);         /* comprimento 3 */
    put_code(&c.bw, 1, 5);         /* código de distância 1: distância 2 */
    fixed_lit(&c.bw, 256);
    CHECK_EQ(inflate_crafted(&c, &out), MGZ_ERR_DISTANCE);
    craft_free(&c);
    dst_free(&out);

    /* Código de distância 30 (reservado) no bloco fixo. */
    craft_begin(&c);
    bw_put(&c.bw, 1, 1);
    bw_put(&c.bw, 1, 2);
    fixed_lit(&c.bw, 'a');
    fixed_lit(&c.bw, 257);
    put_code(&c.bw, 30, 5);
    fixed_lit(&c.bw, 256);
    CHECK_EQ(inflate_crafted(&c, &out), MGZ_ERR_CODE);
    craft_free(&c);
    dst_free(&out);

    /* Símbolo 286 (reservado) no bloco fixo. */
    craft_begin(&c);
    bw_put(&c.bw, 1, 1);
    bw_put(&c.bw, 1, 2);
    fixed_lit(&c.bw, 'a');
    fixed_lit(&c.bw, 286);
    CHECK_EQ(inflate_crafted(&c, &out), MGZ_ERR_CODE);
    craft_free(&c);
    dst_free(&out);

    /* Dinâmico com código de comprimentos sobreposto (19 códigos de 1 bit). */
    craft_begin(&c);
    bw_put(&c.bw, 1, 1);
    bw_put(&c.bw, 2, 2);
    bw_put(&c.bw, 0, 5);
    bw_put(&c.bw, 0, 5);
    bw_put(&c.bw, 15, 4);
    for (int i = 0; i < 19; i++)
        bw_put(&c.bw, 1, 3);
    CHECK_EQ(inflate_crafted(&c, &out), MGZ_ERR_CODE_LENGTHS);
    craft_free(&c);
    dst_free(&out);

    /* HLIT acima de 286. */
    craft_begin(&c);
    bw_put(&c.bw, 1, 1);
    bw_put(&c.bw, 2, 2);
    bw_put(&c.bw, 30, 5);
    bw_put(&c.bw, 0, 5);
    bw_put(&c.bw, 0, 4);
    for (int i = 0; i < 4; i++)
        bw_put(&c.bw, 2, 3);
    CHECK_EQ(inflate_crafted(&c, &out), MGZ_ERR_CODE_LENGTHS);
    craft_free(&c);
    dst_free(&out);

    /*
     * Código de comprimentos com dois símbolos, 0 e 8, de 1 bit cada:
     * na ordem 16,17,18,0,8,... o 0 é o 4º e o 8 é o 5º (HCLEN = 5).
     * Código canônico: símbolo 0 -> bit 0, símbolo 8 -> bit 1.
     */
    for (int variant = 0; variant < 3; variant++) {
        craft_begin(&c);
        bw_put(&c.bw, 1, 1);
        bw_put(&c.bw, 2, 2);
        bw_put(&c.bw, 0, 5);      /* 257 códigos de literal/comprimento */
        bw_put(&c.bw, 0, 5);      /* 1 código de distância */
        bw_put(&c.bw, 1, 4);      /* HCLEN = 5 */
        bw_put(&c.bw, 0, 3);      /* 16 */
        bw_put(&c.bw, 0, 3);      /* 17 */
        bw_put(&c.bw, 0, 3);      /* 18 */
        bw_put(&c.bw, 1, 3);      /* 0 */
        bw_put(&c.bw, 1, 3);      /* 8 */
        if (variant == 0) {
            /* 258 comprimentos, todos 8 menos o do 256: falta o fim de bloco. */
            for (int i = 0; i < 258; i++)
                put_code(&c.bw, i == 256 ? 0 : 1, 1);
        } else if (variant == 1) {
            /* Só 100 comprimentos e acaba: truncado. */
            for (int i = 0; i < 100; i++)
                put_code(&c.bw, 1, 1);
        } else {
            /* Todos 8: 258 códigos de 8 bits sobrepõem (2^8 = 256 vagas). */
            for (int i = 0; i < 258; i++)
                put_code(&c.bw, 1, 1);
        }
        mgz_status expected = variant == 1 ? MGZ_ERR_TRUNCATED : MGZ_ERR_CODE_LENGTHS;
        CHECK_EQ(inflate_crafted(&c, &out), expected);
        craft_free(&c);
        dst_free(&out);
    }
}

static void test_max_distance(void)
{
    SECTION("distância máxima de 32768");
    enum { W = 32768 };
    static uint8_t data[W];
    dst_t out = {NULL, 0, 0};
    craft c;

    rng_seed(5);
    gen_random(data, W);
    for (int variant = 0; variant < 2; variant++) {
        craft_begin(&c);
        /* Bloco armazenado (não final) com 32 KiB aleatórios. */
        bw_put(&c.bw, 0, 1);
        bw_put(&c.bw, 0, 2);
        bw_align(&c.bw);
        bw_put(&c.bw, W, 16);
        bw_put(&c.bw, (uint32_t)(~W & 0xFFFF), 16);
        bw_bytes(&c.bw, data, W);
        /* Bloco fixo final: comprimento 258 a distância 32768. */
        bw_put(&c.bw, 1, 1);
        bw_put(&c.bw, 1, 2);
        fixed_lit(&c.bw, 285);
        put_code(&c.bw, 29, 5);
        bw_put(&c.bw, 32768 - 24577, 13);
        fixed_lit(&c.bw, 256);
        CHECK_EQ(variant == 0 ? inflate_crafted(&c, &out) : inflate_crafted_slowly(&c, &out), MGZ_OK);
        CHECK_EQ(out.len, W + 258);
        if (out.len == W + 258)
            CHECK_MEM(out.data + W, out.data, 258);
        craft_free(&c);
        dst_free(&out);
    }

    /* 100 bytes e distância 101: um byte antes do início. */
    craft_begin(&c);
    bw_put(&c.bw, 0, 1);
    bw_put(&c.bw, 0, 2);
    bw_align(&c.bw);
    bw_put(&c.bw, 100, 16);
    bw_put(&c.bw, (uint32_t)(~100 & 0xFFFF), 16);
    bw_bytes(&c.bw, data, 100);
    bw_put(&c.bw, 1, 1);
    bw_put(&c.bw, 1, 2);
    fixed_lit(&c.bw, 257);
    put_code(&c.bw, 13, 5);        /* base 97, 5 bits extras */
    bw_put(&c.bw, 101 - 97, 5);
    fixed_lit(&c.bw, 256);
    CHECK_EQ(inflate_crafted(&c, &out), MGZ_ERR_DISTANCE);
    craft_free(&c);
    dst_free(&out);
}

/* --------------------------------------------------------------- gzip */

static size_t build_gzip_all_flags(uint8_t *buf, int break_hcrc)
{
    size_t n = 0;
    const uint8_t start[10] = {0x1f, 0x8b, 8, 0x1f, 0x78, 0x56, 0x34, 0x12, 0, 3};
    memcpy(buf, start, 10);
    n = 10;
    const uint8_t extra[8] = {6, 0, 'A', 'P', 2, 0, 'o', 'i'};   /* XLEN=6, subcampo "AP" */
    memcpy(buf + n, extra, 8);
    n += 8;
    memcpy(buf + n, "teste.txt", 10);
    n += 10;
    memcpy(buf + n, "um comentario", 14);
    n += 14;
    uint32_t hcrc = mgz_crc32(0, buf, n);
    buf[n++] = (uint8_t)(hcrc ^ (break_hcrc ? 1u : 0u));
    buf[n++] = (uint8_t)(hcrc >> 8);
    const uint8_t fixed[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00};
    memcpy(buf + n, fixed, sizeof fixed);
    n += sizeof fixed;
    uint32_t crc = mgz_crc32(0, "hello", 5);
    for (int i = 0; i < 4; i++)
        buf[n++] = (uint8_t)(crc >> (8 * i));
    buf[n++] = 5;
    buf[n++] = 0;
    buf[n++] = 0;
    buf[n++] = 0;
    return n;
}

static void record_members(void *ctx, int stage, const mgz_member_info *m)
{
    char *log = ctx;
    size_t len = strlen(log);
    if (len + 4 < 64) {
        log[len] = stage == MGZ_MEMBER_START ? 'S' : 'E';
        log[len + 1] = (char)('0' + (char)m->number);
        log[len + 2] = '\0';
    }
}

static void test_gzip_header(void)
{
    SECTION("cabeçalho gzip: FTEXT, FHCRC, FEXTRA, FNAME, FCOMMENT");
    uint8_t buf[128];
    dst_t out = {NULL, 0, 0};
    mgz_info info;

    size_t n = build_gzip_all_flags(buf, 0);
    CHECK_EQ(run_gunzip(buf, n, 1, &out, &info), MGZ_OK);
    CHECK_EQ(out.len, 5);
    CHECK_MEM(out.data, "hello", 5);
    CHECK_EQ(info.members, 1);
    CHECK_EQ(info.first.flags, 0x1f);
    CHECK_EQ(info.first.mtime, 0x12345678u);
    CHECK_EQ(info.first.os, 3);
    CHECK_EQ(info.first.extra_len, 6);
    CHECK(info.first.has_name && strcmp(info.first.name, "teste.txt") == 0);
    CHECK(info.first.has_comment && strcmp(info.first.comment, "um comentario") == 0);
    CHECK(info.first.has_header_crc);
    CHECK_EQ(info.compressed_size, n);
    dst_free(&out);

    n = build_gzip_all_flags(buf, 1);
    CHECK_EQ(run_gunzip(buf, n, 0, &out, &info), MGZ_ERR_HEADER_CRC);
    dst_free(&out);

    /* Gravando com as opções da biblioteca e lendo de volta. */
    mgz_gzip_options opt = {9, "relatório.txt", "feito em casa", 1700000000u, 1};
    const char *msg = "abacaxi abacaxi abacaxi";
    src_t src = {(const uint8_t *)msg, strlen(msg), 0, 0};
    dst_t gz = {NULL, 0, 0};
    mgz_stream io;
    memset(&io, 0, sizeof io);
    io.read = src_read;
    io.read_ctx = &src;
    io.write = dst_write;
    io.write_ctx = &gz;
    CHECK_EQ(mgz_gzip(&io, &opt), MGZ_OK);
    CHECK_EQ(gz.data[3], 0x02 | 0x08 | 0x10);
    CHECK_EQ(gz.data[8], 2);   /* XFL = 2 no nível 9 */
    char log[64] = "";
    src_t back = {gz.data, gz.len, 0, 0};
    memset(&io, 0, sizeof io);
    io.read = src_read;
    io.read_ctx = &back;
    io.write = dst_write;
    io.write_ctx = &out;
    io.on_member = record_members;
    io.member_ctx = log;
    CHECK_EQ(mgz_gunzip(&io, &info), MGZ_OK);
    CHECK_EQ(out.len, strlen(msg));
    CHECK_MEM(out.data, msg, strlen(msg));
    CHECK(strcmp(info.first.name, "relatório.txt") == 0);
    CHECK(strcmp(info.first.comment, "feito em casa") == 0);
    CHECK_EQ(info.first.mtime, 1700000000u);
    CHECK(strcmp(log, "S1E1") == 0);
    dst_free(&out);
    dst_free(&gz);
}

static void test_gzip_errors(void)
{
    SECTION("erros do contêiner gzip");
    uint8_t text[3000];
    uint8_t *gz = NULL;
    size_t gzlen = 0;
    dst_t out = {NULL, 0, 0};
    mgz_info info;

    rng_seed(6);
    gen_text(text, sizeof text);
    CHECK_EQ(mgz_gzip_buffer(text, sizeof text, 6, &gz, &gzlen), MGZ_OK);
    uint8_t *bad = malloc(gzlen);

    /* Assinatura, método e flags. */
    memcpy(bad, gz, gzlen);
    bad[1] = 0x8c;
    CHECK_EQ(run_gunzip(bad, gzlen, 0, &out, &info), MGZ_ERR_HEADER);
    dst_free(&out);
    memcpy(bad, gz, gzlen);
    bad[2] = 7;
    CHECK_EQ(run_gunzip(bad, gzlen, 0, &out, &info), MGZ_ERR_METHOD);
    dst_free(&out);
    memcpy(bad, gz, gzlen);
    bad[3] = 0x20;
    CHECK_EQ(run_gunzip(bad, gzlen, 0, &out, &info), MGZ_ERR_FLAGS);
    dst_free(&out);

    /* Um byte trocado no CRC do trailer e no ISIZE. */
    memcpy(bad, gz, gzlen);
    bad[gzlen - 8] ^= 0x01;
    CHECK_EQ(run_gunzip(bad, gzlen, 0, &out, &info), MGZ_ERR_CRC);
    dst_free(&out);
    memcpy(bad, gz, gzlen);
    bad[gzlen - 1] ^= 0x80;
    CHECK_EQ(run_gunzip(bad, gzlen, 0, &out, &info), MGZ_ERR_SIZE);
    dst_free(&out);

    /* Todo prefixo do arquivo é truncado. */
    int all_truncated = 1;
    for (size_t cut = 0; cut < gzlen; cut++) {
        if (run_gunzip(gz, cut, 7, &out, &info) != MGZ_ERR_TRUNCATED)
            all_truncated = 0;
        dst_free(&out);
    }
    CHECK(all_truncated);

    /* Bits trocados ao acaso: nunca falha em silêncio com dados errados. */
    rng_seed(7);
    int silent = 0;
    for (int round = 0; round < 3000; round++) {
        memcpy(bad, gz, gzlen);
        size_t pos = rng_below((uint32_t)gzlen);
        bad[pos] ^= (uint8_t)(1u << rng_below(8));
        mgz_status st = run_gunzip(bad, gzlen, 0, &out, &info);
        if (st == MGZ_OK && (out.len != sizeof text || memcmp(out.data, text, sizeof text) != 0))
            silent++;
        dst_free(&out);
    }
    CHECK_EQ(silent, 0);
    free(bad);
    free(gz);
}

static void test_members(void)
{
    SECTION("membros concatenados e lixo no final");
    uint8_t *a = NULL, *b = NULL, *e = NULL;
    size_t alen, blen, elen;
    const char *ta = "primeiro membro, ";
    const char *tb = "segundo membro";
    dst_t out = {NULL, 0, 0};
    mgz_info info;

    CHECK_EQ(mgz_gzip_buffer((const uint8_t *)ta, strlen(ta), 6, &a, &alen), MGZ_OK);
    CHECK_EQ(mgz_gzip_buffer((const uint8_t *)tb, strlen(tb), 1, &b, &blen), MGZ_OK);
    CHECK_EQ(mgz_gzip_buffer(NULL, 0, 9, &e, &elen), MGZ_OK);

    uint8_t *cat = malloc(alen + elen + blen + 8);
    memcpy(cat, a, alen);
    memcpy(cat + alen, e, elen);
    memcpy(cat + alen + elen, b, blen);
    size_t catlen = alen + elen + blen;
    CHECK_EQ(run_gunzip(cat, catlen, 3, &out, &info), MGZ_OK);
    CHECK_EQ(info.members, 3);
    CHECK_EQ(info.compressed_size, catlen);
    CHECK_EQ(info.trailing_garbage, 0);
    CHECK_EQ(out.len, strlen(ta) + strlen(tb));
    CHECK_MEM(out.data, "primeiro membro, segundo membro", out.len);
    dst_free(&out);

    memcpy(cat + catlen, "lixo!", 5);
    CHECK_EQ(run_gunzip(cat, catlen + 5, 0, &out, &info), MGZ_OK);
    CHECK_EQ(info.trailing_garbage, 1);
    CHECK_EQ(info.members, 3);
    dst_free(&out);

    cat[catlen] = 0x1f;
    CHECK_EQ(run_gunzip(cat, catlen + 1, 0, &out, &info), MGZ_OK);
    CHECK_EQ(info.trailing_garbage, 1);
    dst_free(&out);

    /* Um novo membro que começa e para no meio do cabeçalho é erro. */
    cat[catlen + 1] = 0x8b;
    cat[catlen + 2] = 8;
    CHECK_EQ(run_gunzip(cat, catlen + 3, 0, &out, &info), MGZ_ERR_TRUNCATED);
    dst_free(&out);

    /* Uma referência não pode alcançar o membro anterior. */
    craft c;
    craft_begin(&c);
    const uint8_t head[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
    bw_bytes(&c.bw, head, 10);
    bw_put(&c.bw, 1, 1);
    bw_put(&c.bw, 1, 2);
    fixed_lit(&c.bw, 257);
    put_code(&c.bw, 0, 5);
    fixed_lit(&c.bw, 256);
    craft_end(&c);
    uint8_t *two = malloc(alen + c.out.len + 8);
    memcpy(two, a, alen);
    memcpy(two + alen, c.out.data, c.out.len);
    memset(two + alen + c.out.len, 0, 8);
    CHECK_EQ(run_gunzip(two, alen + c.out.len + 8, 0, &out, &info), MGZ_ERR_DISTANCE);
    dst_free(&out);
    craft_free(&c);

    free(two);
    free(cat);
    free(a);
    free(b);
    free(e);
}

/* ------------------------------------------------------ compressor */

static void test_roundtrip(void)
{
    SECTION("ida e volta em todos os níveis e tamanhos de borda");
    static const size_t sizes[] = {0, 1, 2, 3, 4, 257, 258, 259, 1000, 32767, 32768, 32769,
                                   65535, 65536, 65537, 131071, 131072, 131073, 200003};
    void (*gens[])(uint8_t *, size_t) = {gen_random, gen_text, gen_zeros, gen_period, gen_mixed};
    const size_t max = 200003;
    uint8_t *data = malloc(max);
    int failures = 0, runs = 0;

    rng_seed(8);
    for (size_t g = 0; g < sizeof gens / sizeof gens[0]; g++) {
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            size_t n = sizes[si];
            gens[g](data, n);
            for (int level = 0; level <= 9; level++) {
                /* Nos maiores, alguns níveis bastam para cobrir as bordas. */
                if (n > 70000 && level != 0 && level != 1 && level != 6 && level != 9)
                    continue;
                size_t chunk = (si % 3 == 0) ? 1 : (si % 3 == 1) ? 4093 : 0;
                if (chunk == 1 && n > 40000)
                    chunk = 13;
                dst_t z = {NULL, 0, 0}, back = {NULL, 0, 0};
                deflate_stats stats;
                mgz_status st = run_deflate(data, n, level, chunk, &z, &stats);
                if (st == MGZ_OK)
                    st = run_inflate(z.data, z.len, chunk, &back, NULL);
                runs++;
                if (st != MGZ_OK || back.len != n || (n > 0 && memcmp(back.data, data, n) != 0)) {
                    failures++;
                    fprintf(stderr, "  ida e volta falhou: gerador %zu, %zu bytes, nível %d (%s)\n",
                            g, n, level, mgz_strerror(st));
                }
                dst_free(&z);
                dst_free(&back);
            }
        }
    }
    CHECK_EQ(failures, 0);
    CHECK(runs > 500);
    free(data);
}

static void test_encoder_choices(void)
{
    SECTION("escolhas do compressor");
    const size_t n = 300000;
    uint8_t *data = malloc(n);
    dst_t z = {NULL, 0, 0};
    deflate_stats st;

    /* Dados aleatórios não comprimem: o melhor é guardar. */
    rng_seed(9);
    gen_random(data, n);
    CHECK_EQ(run_deflate(data, n, 6, 0, &z, &st), MGZ_OK);
    CHECK(st.blocks[MGZ_BLOCK_STORED] > 0);
    CHECK_EQ(st.blocks[MGZ_BLOCK_DYNAMIC], 0);
    CHECK(z.len < n + n / 1000 + 64);
    dst_free(&z);

    /* Pouca coisa: o código fixo ganha (não paga a tabela). */
    const char *small = "abracadabra, abracadabra!";
    CHECK_EQ(run_deflate((const uint8_t *)small, strlen(small), 6, 0, &z, &st), MGZ_OK);
    CHECK_EQ(st.blocks[MGZ_BLOCK_FIXED], 1);
    CHECK_EQ(st.blocks[MGZ_BLOCK_STORED] + st.blocks[MGZ_BLOCK_DYNAMIC], 0);
    dst_free(&z);

    /* Texto grande: tabelas dinâmicas. */
    gen_text(data, n);
    CHECK_EQ(run_deflate(data, n, 6, 0, &z, &st), MGZ_OK);
    CHECK(st.blocks[MGZ_BLOCK_DYNAMIC] > 0);
    size_t text6 = z.len;
    dst_free(&z);
    CHECK_EQ(run_deflate(data, n, 1, 0, &z, &st), MGZ_OK);
    size_t text1 = z.len;
    dst_free(&z);
    CHECK_EQ(run_deflate(data, n, 9, 0, &z, &st), MGZ_OK);
    size_t text9 = z.len;
    dst_free(&z);
    CHECK(text9 <= text6 && text6 < text1);
    CHECK(text6 < n / 2);

    /* Bloco de 32 KiB repetido: referências a exatamente 32768 bytes. */
    gen_period(data, n);
    CHECK_EQ(run_deflate(data, n, 6, 0, &z, &st), MGZ_OK);
    CHECK_EQ(st.max_distance, 32768);
    CHECK(z.len < 40000);
    dst_free(&z);

    /* Zeros: tudo vira referências de distância 1. */
    gen_zeros(data, n);
    CHECK_EQ(run_deflate(data, n, 9, 0, &z, &st), MGZ_OK);
    CHECK(z.len < 1000);
    CHECK_EQ(st.max_distance, 1);
    dst_free(&z);

    /* Distribuição de Fibonacci: exige o limite de 15 bits nas tabelas. */
    size_t pos = 0;
    uint32_t a = 1, b = 1;
    for (unsigned sym = 0; sym < 24 && pos < n; sym++) {
        for (uint32_t k = 0; k < a && pos < n; k++)
            data[pos++] = (uint8_t)sym;
        uint32_t c = a + b;
        a = b;
        b = c;
    }
    for (size_t i = pos; i > 1; i--) {   /* embaralha (Fisher-Yates) */
        size_t j = rng_below((uint32_t)i);
        uint8_t t = data[i - 1];
        data[i - 1] = data[j];
        data[j] = t;
    }
    dst_t back = {NULL, 0, 0};
    CHECK_EQ(run_deflate(data, pos, 4, 0, &z, &st), MGZ_OK);
    CHECK_EQ(run_inflate(z.data, z.len, 0, &back, NULL), MGZ_OK);
    CHECK_EQ(back.len, pos);
    CHECK(back.len == pos && memcmp(back.data, data, pos) == 0);
    dst_free(&z);
    dst_free(&back);
    free(data);
}

static void test_garbage(void)
{
    SECTION("entradas aleatórias e mutadas não derrubam o decodificador");
    uint8_t buf[300];
    uint8_t *out = NULL, *z = NULL, *m = NULL;
    size_t out_len, zlen;
    long decoded = 0, rejected = 0;

    rng_seed(10);
    for (int it = 0; it < 20000; it++) {
        size_t n = 1 + rng_below(sizeof buf);
        gen_random(buf, n);
        if (it % 3 == 0)
            buf[0] = (uint8_t)((buf[0] & ~6u) | 4u);   /* força BTYPE dinâmico */
        if (mgz_inflate_buffer(buf, n, &out, &out_len) == MGZ_OK) {
            decoded++;
            free(out);
        } else {
            rejected++;
        }
    }
    uint8_t *text = malloc(20000);
    gen_text(text, 20000);
    CHECK_EQ(mgz_deflate_buffer(text, 20000, 6, &z, &zlen), MGZ_OK);
    m = malloc(zlen);
    for (int it = 0; it < 20000; it++) {
        memcpy(m, z, zlen);
        for (int k = 1 + (int)rng_below(4); k > 0; k--)
            m[rng_below((uint32_t)zlen)] ^= (uint8_t)(1u << rng_below(8));
        size_t n = rng_below(4) == 0 ? rng_below((uint32_t)zlen) : zlen;
        if (mgz_inflate_buffer(m, n, &out, &out_len) == MGZ_OK) {
            decoded++;
            free(out);
        } else {
            rejected++;
        }
    }
    CHECK_EQ(decoded + rejected, 40000);
    CHECK(rejected > 0);
    free(m);
    free(z);
    free(text);
}

static void test_strerror(void)
{
    SECTION("mensagens de erro");
    int distinct = 1;
    for (int i = MGZ_OK; i <= MGZ_ERR_ARGUMENT; i++) {
        const char *m = mgz_strerror((mgz_status)i);
        if (m == NULL || m[0] == '\0')
            distinct = 0;
        for (int j = MGZ_OK; j < i; j++)
            if (strcmp(m, mgz_strerror((mgz_status)j)) == 0)
                distinct = 0;
    }
    CHECK(distinct);
    CHECK(strstr(mgz_strerror(MGZ_ERR_CRC), "CRC") != NULL);
    CHECK(strstr(mgz_strerror(MGZ_ERR_TRUNCATED), "truncados") != NULL);
}

int main(void)
{
    test_crc32();
    test_bit_reader();
    test_bit_writer();
    test_canonical_codes();
    test_huffman_rules();
    test_package_merge();
    test_huffman_roundtrip();
    test_block_types();
    test_inflate_errors();
    test_max_distance();
    test_gzip_header();
    test_gzip_errors();
    test_members();
    test_roundtrip();
    test_encoder_choices();
    test_garbage();
    test_strerror();
    return test_summary();
}
