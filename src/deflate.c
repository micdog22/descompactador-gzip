/*
 * deflate.c: compressor DEFLATE.
 *
 * Etapa 1, LZ77: percorremos a entrada procurando, nos últimos 32 KiB, a
 * repetição mais longa do que vem a seguir. Para achar candidatos rápido,
 * cada posição entra numa "cadeia de hash" indexada pelos 3 bytes que
 * começam nela: head[h] guarda a posição mais recente com hash h e
 * prev[pos] aponta para a anterior com o mesmo hash.
 *
 * Etapa 2, Huffman: os símbolos produzidos (literais e pares
 * comprimento/distância) são agrupados em blocos. Para cada bloco
 * calculamos o custo exato em bits das três codificações possíveis
 * (armazenado, Huffman fixo e Huffman dinâmico) e escolhemos a menor.
 */
#include "deflate.h"
#include "huffman.h"
#include "tables.h"

#include <stdlib.h>
#include <string.h>

#define WSIZE 32768u                          /* janela do DEFLATE */
#define MAX_DIST WSIZE                        /* maior distância permitida */
#define MIN_MATCH 3u
#define MAX_MATCH 258u
#define MIN_LOOKAHEAD (MAX_MATCH + MIN_MATCH + 1u)
#define WIN_BUFSZ (4u * WSIZE)                /* janela + dados ainda não vistos */
#define HASH_BITS 15u
#define HASH_SIZE (1u << HASH_BITS)
#define PREV_SIZE (2u * WSIZE)                /* 2x a janela: elos nunca são sobrescritos antes da hora */
#define SYM_BUFSZ 16384u                      /* símbolos por bloco */
#define TOO_FAR 4096u                         /* match de 3 bytes mais longe que isso não compensa */
#define MAX_STORED 65535u                     /* limite de LEN num bloco armazenado */

typedef struct {
    uint16_t good;   /* com um match deste tamanho, procura menos (chain / 4) */
    uint16_t lazy;   /* níveis 4-9: só tenta melhorar matches menores que isso;
                        níveis 1-3: só indexa as posições de matches até esse tamanho */
    uint16_t nice;   /* para de procurar ao achar um match deste tamanho */
    uint16_t chain;  /* quantos candidatos visitar no máximo */
} level_config;

/* Os mesmos parâmetros do zlib, para comparação justa. */
static const level_config CONFIG[10] = {
    {0, 0, 0, 0},          /* 0: só blocos armazenados */
    {4, 4, 8, 4},          /* 1: guloso, o mais rápido */
    {4, 5, 16, 8},
    {4, 6, 32, 32},
    {4, 4, 16, 16},        /* 4: avaliação preguiçosa */
    {8, 16, 32, 32},
    {8, 16, 128, 128},     /* 6: padrão */
    {8, 32, 128, 256},
    {32, 128, 258, 1024},
    {32, 258, 258, 4096},  /* 9: o que mais comprime */
};

typedef struct {
    bit_writer *bw;
    mgz_read_fn read;
    void *rctx;
    int eof;
    uint32_t crc;
    uint64_t total_in;

    /* Janela: win[0] corresponde à posição absoluta base da entrada. */
    uint8_t *win;
    uint64_t base;
    size_t strstart;        /* posição atual em win */
    size_t lookahead;       /* bytes válidos a partir de strstart */
    uint64_t *head;         /* posição absoluta + 1 (0 = cadeia vazia) */
    uint64_t *prev;

    level_config cfg;
    size_t match_length, prev_length;
    uint64_t match_start, prev_match;   /* posições absolutas */
    int match_available;

    /* Bloco atual. */
    uint64_t block_start;   /* posição absoluta do primeiro byte do bloco */
    uint16_t *sym_dist;     /* 0 = literal */
    uint8_t *sym_lc;        /* literal, ou comprimento - 3 */
    size_t nsyms;
    uint32_t freq_ll[286];
    uint32_t freq_d[30];

    /* Código fixo, já invertido para escrita. */
    uint8_t fixed_ll_len[288];
    uint16_t fixed_ll_code[288];
    uint8_t fixed_d_len[30];
    uint16_t fixed_d_code[30];

    deflate_stats stats;
} deflate_state;

/* ------------------------------------------------------------ símbolos */

static unsigned len_index(unsigned len)
{
    unsigned lo = 0, hi = 28;
    while (lo < hi) {
        unsigned mid = (lo + hi + 1) / 2;
        if (LEN_BASE[mid] <= len)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static unsigned dist_index(unsigned dist)
{
    unsigned lo = 0, hi = 29;
    while (lo < hi) {
        unsigned mid = (lo + hi + 1) / 2;
        if (DIST_BASE[mid] <= dist)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static void tally_literal(deflate_state *s, uint8_t c)
{
    s->sym_dist[s->nsyms] = 0;
    s->sym_lc[s->nsyms] = c;
    s->nsyms++;
    s->freq_ll[c]++;
    s->stats.literals++;
}

static void tally_match(deflate_state *s, size_t dist, size_t len)
{
    s->sym_dist[s->nsyms] = (uint16_t)dist;
    s->sym_lc[s->nsyms] = (uint8_t)(len - MIN_MATCH);
    s->nsyms++;
    s->freq_ll[257 + len_index((unsigned)len)]++;
    s->freq_d[dist_index((unsigned)dist)]++;
    s->stats.matches++;
    if (dist > s->stats.max_distance)
        s->stats.max_distance = (unsigned)dist;
}

/* -------------------------------------------------------- códigos */

/* Códigos canônicos invertidos, prontos para o escritor LSB primeiro. */
static void make_codes(const uint8_t *lens, unsigned n, uint16_t *codes)
{
    huff_canonical_codes(lens, n, codes);
    for (unsigned i = 0; i < n; i++)
        codes[i] = (uint16_t)huff_reverse(codes[i], lens[i]);
}

/* Garante pelo menos dois símbolos com frequência (código completo). */
static void ensure_two(uint32_t *freq, unsigned n)
{
    unsigned used = 0;
    for (unsigned i = 0; i < n; i++)
        used += freq[i] != 0;
    for (unsigned i = 0; i < n && used < 2; i++) {
        if (freq[i] == 0) {
            freq[i] = 1;
            used++;
        }
    }
}

typedef struct {
    uint8_t ll_len[286];
    uint16_t ll_code[286];
    uint8_t d_len[30];
    uint16_t d_code[30];
    unsigned hlit, hdist, hclen;
    uint8_t rle_sym[286 + 30];
    uint8_t rle_extra[286 + 30];
    size_t nrle;
    uint8_t cl_len[19];
    uint16_t cl_code[19];
    uint64_t header_bits;   /* HLIT, HDIST, HCLEN e as tabelas */
} dyn_tree;

static void rle_push(dyn_tree *t, unsigned sym, unsigned extra)
{
    t->rle_sym[t->nrle] = (uint8_t)sym;
    t->rle_extra[t->nrle] = (uint8_t)extra;
    t->nrle++;
}

/* Comprime a sequência de comprimentos com os símbolos 16, 17 e 18. */
static void rle_lengths(dyn_tree *t, const uint8_t *lens, size_t n)
{
    size_t i = 0;
    t->nrle = 0;
    while (i < n) {
        uint8_t cur = lens[i];
        size_t run = 1;
        while (i + run < n && lens[i + run] == cur)
            run++;
        i += run;
        if (cur == 0) {
            while (run >= 11) {
                size_t k = run > 138 ? 138 : run;
                rle_push(t, 18, (unsigned)(k - 11));
                run -= k;
            }
            if (run >= 3) {
                rle_push(t, 17, (unsigned)(run - 3));
                run = 0;
            }
        } else {
            rle_push(t, cur, 0);    /* o 16 repete o anterior: o primeiro vai literal */
            run--;
            while (run >= 3) {
                size_t k = run > 6 ? 6 : run;
                rle_push(t, 16, (unsigned)(k - 3));
                run -= k;
            }
        }
        while (run-- > 0)
            rle_push(t, cur, 0);
    }
}

static unsigned rle_extra_bits(unsigned sym)
{
    return sym == 16 ? 2u : sym == 17 ? 3u : sym == 18 ? 7u : 0u;
}

static void build_dynamic(const deflate_state *s, dyn_tree *t)
{
    uint32_t fl[286], fd[30], fc[19] = {0};
    uint8_t all[286 + 30];

    memcpy(fl, s->freq_ll, sizeof fl);
    memcpy(fd, s->freq_d, sizeof fd);
    ensure_two(fl, 286);
    ensure_two(fd, 30);
    huff_limited_lengths(fl, 286, 15, t->ll_len);
    huff_limited_lengths(fd, 30, 15, t->d_len);

    t->hlit = 286;
    while (t->hlit > 257 && t->ll_len[t->hlit - 1] == 0)
        t->hlit--;
    t->hdist = 30;
    while (t->hdist > 1 && t->d_len[t->hdist - 1] == 0)
        t->hdist--;

    memcpy(all, t->ll_len, t->hlit);
    memcpy(all + t->hlit, t->d_len, t->hdist);
    rle_lengths(t, all, t->hlit + t->hdist);
    for (size_t i = 0; i < t->nrle; i++)
        fc[t->rle_sym[i]]++;
    ensure_two(fc, 19);
    huff_limited_lengths(fc, 19, 7, t->cl_len);

    t->hclen = 19;
    while (t->hclen > 4 && t->cl_len[CL_ORDER[t->hclen - 1]] == 0)
        t->hclen--;

    t->header_bits = 5 + 5 + 4 + 3u * t->hclen;
    for (size_t i = 0; i < t->nrle; i++)
        t->header_bits += t->cl_len[t->rle_sym[i]] + rle_extra_bits(t->rle_sym[i]);

    make_codes(t->ll_len, 286, t->ll_code);
    make_codes(t->d_len, 30, t->d_code);
    make_codes(t->cl_len, 19, t->cl_code);
}

/* Bits dos símbolos do bloco com os comprimentos dados (sem bits extras). */
static uint64_t symbol_bits(const deflate_state *s, const uint8_t *ll_len, const uint8_t *d_len)
{
    uint64_t bits = 0;
    for (unsigned i = 0; i < 286; i++)
        bits += (uint64_t)s->freq_ll[i] * ll_len[i];
    for (unsigned i = 0; i < 30; i++)
        bits += (uint64_t)s->freq_d[i] * d_len[i];
    return bits;
}

/* Bits extras de comprimentos e distâncias (iguais no fixo e no dinâmico). */
static uint64_t extra_bits(const deflate_state *s)
{
    uint64_t bits = 0;
    for (unsigned i = 0; i < 29; i++)
        bits += (uint64_t)s->freq_ll[257 + i] * LEN_EXTRA[i];
    for (unsigned i = 0; i < 30; i++)
        bits += (uint64_t)s->freq_d[i] * DIST_EXTRA[i];
    return bits;
}

/* Custo exato de guardar len bytes em blocos armazenados a partir de bitpos. */
static uint64_t stored_bits(uint64_t bitpos, size_t len)
{
    uint64_t bits = 0;
    unsigned pos = (unsigned)(bitpos & 7u);
    do {
        size_t chunk = len > MAX_STORED ? MAX_STORED : len;
        pos = (pos + 3) & 7u;
        bits += 3 + ((8 - pos) & 7u) + 32 + 8u * (uint64_t)chunk;
        pos = 0;
        len -= chunk;
    } while (len > 0);
    return bits;
}

/* --------------------------------------------------------- emissão */

static void send_symbols(deflate_state *s, const uint16_t *ll_code, const uint8_t *ll_len,
                         const uint16_t *d_code, const uint8_t *d_len)
{
    bit_writer *bw = s->bw;
    for (size_t i = 0; i < s->nsyms; i++) {
        unsigned dist = s->sym_dist[i];
        if (dist == 0) {
            unsigned lit = s->sym_lc[i];
            bw_put(bw, ll_code[lit], ll_len[lit]);
            continue;
        }
        unsigned len = s->sym_lc[i] + MIN_MATCH;
        unsigned li = len_index(len);
        bw_put(bw, ll_code[257 + li], ll_len[257 + li]);
        bw_put(bw, len - LEN_BASE[li], LEN_EXTRA[li]);
        unsigned di = dist_index(dist);
        bw_put(bw, d_code[di], d_len[di]);
        bw_put(bw, dist - DIST_BASE[di], DIST_EXTRA[di]);
    }
    bw_put(bw, ll_code[256], ll_len[256]);   /* fim do bloco */
}

static void emit_stored(deflate_state *s, const uint8_t *data, size_t len, int last)
{
    bit_writer *bw = s->bw;
    do {
        size_t chunk = len > MAX_STORED ? MAX_STORED : len;
        int fin = last && chunk == len;
        bw_put(bw, (uint32_t)fin, 1);
        bw_put(bw, 0, 2);
        bw_align(bw);
        bw_put(bw, (uint32_t)chunk, 16);
        bw_put(bw, (uint32_t)(~chunk & 0xFFFFu), 16);
        bw_bytes(bw, data, chunk);
        data += chunk;
        len -= chunk;
        s->stats.blocks[MGZ_BLOCK_STORED]++;
    } while (len > 0);
}

static void emit_fixed(deflate_state *s, int last)
{
    bw_put(s->bw, (uint32_t)last, 1);
    bw_put(s->bw, 1, 2);
    send_symbols(s, s->fixed_ll_code, s->fixed_ll_len, s->fixed_d_code, s->fixed_d_len);
    s->stats.blocks[MGZ_BLOCK_FIXED]++;
}

static void emit_dynamic(deflate_state *s, const dyn_tree *t, int last)
{
    bit_writer *bw = s->bw;
    bw_put(bw, (uint32_t)last, 1);
    bw_put(bw, 2, 2);
    bw_put(bw, t->hlit - 257, 5);
    bw_put(bw, t->hdist - 1, 5);
    bw_put(bw, t->hclen - 4, 4);
    for (unsigned i = 0; i < t->hclen; i++)
        bw_put(bw, t->cl_len[CL_ORDER[i]], 3);
    for (size_t i = 0; i < t->nrle; i++) {
        unsigned sym = t->rle_sym[i];
        bw_put(bw, t->cl_code[sym], t->cl_len[sym]);
        bw_put(bw, t->rle_extra[i], rle_extra_bits(sym));
    }
    send_symbols(s, t->ll_code, t->ll_len, t->d_code, t->d_len);
    s->stats.blocks[MGZ_BLOCK_DYNAMIC]++;
}

/* Fecha o bloco atual escolhendo a codificação mais barata. */
static mgz_status flush_block(deflate_state *s, int last)
{
    uint64_t end = s->base + s->strstart;
    size_t raw_len = (size_t)(end - s->block_start);
    /* O armazenado precisa dos bytes originais, que podem já ter saído da janela. */
    const uint8_t *raw = s->block_start >= s->base ? s->win + (s->block_start - s->base) : NULL;
    dyn_tree t;

    s->freq_ll[256] = 1;
    build_dynamic(s, &t);
    uint64_t extra = extra_bits(s);
    uint64_t dyn_cost = 3 + t.header_bits + symbol_bits(s, t.ll_len, t.d_len) + extra;
    uint64_t fixed_cost = 3 + symbol_bits(s, s->fixed_ll_len, s->fixed_d_len) + extra;
    uint64_t stored_cost = raw ? stored_bits(bw_bit_count(s->bw), raw_len) : UINT64_MAX;

    if (stored_cost <= fixed_cost && stored_cost <= dyn_cost)
        emit_stored(s, raw, raw_len, last);
    else if (fixed_cost <= dyn_cost)
        emit_fixed(s, last);
    else
        emit_dynamic(s, &t, last);

    s->block_start = end;
    s->nsyms = 0;
    memset(s->freq_ll, 0, sizeof s->freq_ll);
    memset(s->freq_d, 0, sizeof s->freq_d);
    return s->bw->err ? MGZ_ERR_WRITE : MGZ_OK;
}

/* ------------------------------------------------------------ LZ77 */

static mgz_status read_input(deflate_state *s, uint8_t *dst, size_t cap, size_t *got)
{
    size_t n = 0;
    *got = 0;
    if (s->eof || cap == 0)
        return MGZ_OK;
    if (s->read(s->rctx, dst, cap, &n) != 0)
        return MGZ_ERR_READ;
    if (n == 0) {
        s->eof = 1;
        return MGZ_OK;
    }
    if (n > cap)
        return MGZ_ERR_READ;
    s->crc = mgz_crc32(s->crc, dst, n);
    s->total_in += n;
    *got = n;
    return MGZ_OK;
}

/* Lê mais entrada; se o buffer acabou, desliza mantendo 32 KiB de histórico. */
static mgz_status fill_window(deflate_state *s)
{
    while (s->lookahead < MIN_LOOKAHEAD && !s->eof) {
        size_t end = s->strstart + s->lookahead;
        if (end == WIN_BUFSZ) {
            size_t delta = s->strstart - WSIZE;
            memmove(s->win, s->win + delta, WIN_BUFSZ - delta);
            s->base += delta;
            s->strstart -= delta;
            end -= delta;
        }
        size_t got;
        mgz_status st = read_input(s, s->win + end, WIN_BUFSZ - end, &got);
        if (st != MGZ_OK)
            return st;
        s->lookahead += got;
    }
    return MGZ_OK;
}

static uint32_t hash3(const uint8_t *p)
{
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return (v * 2654435761u) >> (32u - HASH_BITS);
}

/* Insere a posição na cadeia do seu hash e devolve a cabeça anterior. */
static uint64_t insert_string(deflate_state *s, size_t pos)
{
    uint32_t h = hash3(s->win + pos);
    uint64_t abs = s->base + pos;
    uint64_t old = s->head[h];
    s->prev[abs & (PREV_SIZE - 1u)] = old;
    s->head[h] = abs + 1;
    return old;
}

/*
 * Percorre a cadeia a partir de cand1 (posição + 1) e devolve o maior
 * comprimento encontrado; a posição do match vai para match_start. Só
 * aceita matches maiores que prev_length.
 */
static size_t longest_match(deflate_state *s, uint64_t cand1)
{
    unsigned chain = s->cfg.chain;
    size_t best = s->prev_length;
    size_t maxlen = s->lookahead < MAX_MATCH ? s->lookahead : MAX_MATCH;
    size_t nice = s->cfg.nice < maxlen ? s->cfg.nice : maxlen;
    uint64_t cur = s->base + s->strstart;
    uint64_t limit = cur > MAX_DIST ? cur - MAX_DIST : 0;
    const uint8_t *scan = s->win + s->strstart;

    if (limit < s->base)
        limit = s->base;
    if (best >= maxlen)
        return best;
    if (s->prev_length >= s->cfg.good)
        chain >>= 2;
    if (chain == 0)
        chain = 1;

    while (cand1 != 0) {
        uint64_t cand = cand1 - 1;
        if (cand < limit || cand >= cur)
            break;
        const uint8_t *m = s->win + (cand - s->base);
        /* Teste rápido: o byte que tornaria o match melhor e os dois primeiros. */
        if (m[best] == scan[best] && m[0] == scan[0] && m[1] == scan[1]) {
            size_t len = 2;
            while (len < maxlen && m[len] == scan[len])
                len++;
            if (len > best) {
                best = len;
                s->match_start = cand;
                if (len >= nice)
                    break;
            }
        }
        if (--chain == 0)
            break;
        cand1 = s->prev[cand & (PREV_SIZE - 1u)];
    }
    return best;
}

/* Níveis 1 a 3: emite o primeiro match encontrado (estratégia gulosa). */
static mgz_status deflate_greedy(deflate_state *s)
{
    mgz_status st;
    for (;;) {
        if (s->lookahead < MIN_LOOKAHEAD) {
            if ((st = fill_window(s)) != MGZ_OK)
                return st;
            if (s->lookahead == 0)
                break;
        }
        uint64_t head = 0;
        size_t mlen = 0;
        if (s->lookahead >= MIN_MATCH)
            head = insert_string(s, s->strstart);
        if (head != 0) {
            s->prev_length = MIN_MATCH - 1;
            mlen = longest_match(s, head);
        }
        if (mlen >= MIN_MATCH) {
            tally_match(s, (size_t)(s->base + s->strstart - s->match_start), mlen);
            s->lookahead -= mlen;
            if (mlen <= s->cfg.lazy && s->lookahead >= MIN_MATCH) {
                for (size_t i = 1; i < mlen; i++)
                    insert_string(s, s->strstart + i);
            }
            s->strstart += mlen;
        } else {
            tally_literal(s, s->win[s->strstart]);
            s->strstart++;
            s->lookahead--;
        }
        if (s->nsyms == SYM_BUFSZ && (st = flush_block(s, 0)) != MGZ_OK)
            return st;
    }
    return flush_block(s, 1);
}

/*
 * Níveis 4 a 9: avaliação preguiçosa. Ao achar um match na posição p,
 * ainda olhamos a posição p + 1: se lá houver um match mais longo, p vira
 * literal e ficamos com o novo. Exemplo: em "abcbcdef...", olhar um byte à
 * frente troca um match curto por um longo.
 */
static mgz_status deflate_lazy(deflate_state *s)
{
    mgz_status st;
    for (;;) {
        if (s->lookahead < MIN_LOOKAHEAD) {
            if ((st = fill_window(s)) != MGZ_OK)
                return st;
            if (s->lookahead == 0)
                break;
        }
        uint64_t head = 0;
        if (s->lookahead >= MIN_MATCH)
            head = insert_string(s, s->strstart);

        s->prev_length = s->match_length;
        s->prev_match = s->match_start;
        s->match_length = MIN_MATCH - 1;

        if (head != 0 && s->prev_length < s->cfg.lazy) {
            s->match_length = longest_match(s, head);
            if (s->match_length == MIN_MATCH &&
                s->base + s->strstart - s->match_start > TOO_FAR)
                s->match_length = MIN_MATCH - 1;
        }

        if (s->prev_length >= MIN_MATCH && s->match_length <= s->prev_length) {
            /* O match que começou no byte anterior é o melhor: emite. */
            size_t max_insert = s->strstart + s->lookahead - MIN_MATCH;
            tally_match(s, (size_t)(s->base + s->strstart - 1 - s->prev_match), s->prev_length);
            s->lookahead -= s->prev_length - 1;
            for (size_t n = s->prev_length - 2; n > 0; n--) {
                if (++s->strstart <= max_insert)
                    insert_string(s, s->strstart);
            }
            s->match_available = 0;
            s->match_length = MIN_MATCH - 1;
            s->strstart++;
            if (s->nsyms == SYM_BUFSZ && (st = flush_block(s, 0)) != MGZ_OK)
                return st;
        } else if (s->match_available) {
            /* Não houve match melhor no byte anterior: ele vira literal. */
            tally_literal(s, s->win[s->strstart - 1]);
            if (s->nsyms == SYM_BUFSZ && (st = flush_block(s, 0)) != MGZ_OK)
                return st;
            s->strstart++;
            s->lookahead--;
        } else {
            /* Primeira posição analisada: decide no próximo passo. */
            s->match_available = 1;
            s->strstart++;
            s->lookahead--;
        }
    }
    if (s->match_available) {
        tally_literal(s, s->win[s->strstart - 1]);
        s->match_available = 0;
    }
    return flush_block(s, 1);
}

/* Nível 0: só blocos armazenados de até 65535 bytes. */
static mgz_status deflate_stored_only(deflate_state *s)
{
    for (;;) {
        size_t n = 0;
        while (n < MAX_STORED && !s->eof) {
            size_t got;
            mgz_status st = read_input(s, s->win + n, MAX_STORED - n, &got);
            if (st != MGZ_OK)
                return st;
            n += got;
        }
        emit_stored(s, s->win, n, s->eof);
        if (s->bw->err)
            return MGZ_ERR_WRITE;
        if (s->eof)
            return MGZ_OK;
    }
}

static void init_fixed_codes(deflate_state *s)
{
    unsigned i;
    for (i = 0; i < 144; i++)
        s->fixed_ll_len[i] = 8;
    for (; i < 256; i++)
        s->fixed_ll_len[i] = 9;
    for (; i < 280; i++)
        s->fixed_ll_len[i] = 7;
    for (; i < 288; i++)
        s->fixed_ll_len[i] = 8;
    make_codes(s->fixed_ll_len, 288, s->fixed_ll_code);
    for (i = 0; i < 30; i++)
        s->fixed_d_len[i] = 5;
    make_codes(s->fixed_d_len, 30, s->fixed_d_code);
}

mgz_status deflate_stream(bit_writer *bw, mgz_read_fn read, void *rctx, int level,
                          uint32_t *crc, uint64_t *total_in, deflate_stats *stats)
{
    if (level < 0 || level > 9 || read == NULL)
        return MGZ_ERR_ARGUMENT;
    deflate_state *s = calloc(1, sizeof *s);
    if (s == NULL)
        return MGZ_ERR_MEMORY;
    s->bw = bw;
    s->read = read;
    s->rctx = rctx;
    s->cfg = CONFIG[level];
    s->match_length = s->prev_length = MIN_MATCH - 1;
    s->win = malloc(WIN_BUFSZ);
    s->head = calloc(HASH_SIZE, sizeof *s->head);
    s->prev = calloc(PREV_SIZE, sizeof *s->prev);
    s->sym_dist = malloc(SYM_BUFSZ * sizeof *s->sym_dist);
    s->sym_lc = malloc(SYM_BUFSZ);
    mgz_status st = MGZ_ERR_MEMORY;
    if (s->win && s->head && s->prev && s->sym_dist && s->sym_lc) {
        init_fixed_codes(s);
        if (level == 0)
            st = deflate_stored_only(s);
        else if (level <= 3)
            st = deflate_greedy(s);
        else
            st = deflate_lazy(s);
    }
    if (st == MGZ_OK && bw->err)
        st = MGZ_ERR_WRITE;
    *crc = s->crc;
    *total_in = s->total_in;
    if (stats != NULL)
        *stats = s->stats;
    free(s->win);
    free(s->head);
    free(s->prev);
    free(s->sym_dist);
    free(s->sym_lc);
    free(s);
    return st;
}
