/* huffman.c: construção e decodificação de códigos de Huffman canônicos. */
#include "huffman.h"

#include <string.h>

unsigned huff_reverse(unsigned code, unsigned len)
{
    unsigned r = 0;
    for (unsigned i = 0; i < len; i++) {
        r = (r << 1) | (code & 1u);
        code >>= 1;
    }
    return r;
}

static unsigned reverse16(unsigned v)
{
    v = ((v & 0xAAAAu) >> 1) | ((v & 0x5555u) << 1);
    v = ((v & 0xCCCCu) >> 2) | ((v & 0x3333u) << 2);
    v = ((v & 0xF0F0u) >> 4) | ((v & 0x0F0Fu) << 4);
    v = ((v & 0xFF00u) >> 8) | ((v & 0x00FFu) << 8);
    return v & 0xFFFFu;
}

void huff_canonical_codes(const uint8_t *lens, unsigned n, uint16_t *codes)
{
    unsigned count[HUFF_MAX_BITS + 1] = {0};
    unsigned next[HUFF_MAX_BITS + 1] = {0};
    for (unsigned i = 0; i < n; i++)
        count[lens[i]]++;
    count[0] = 0;
    /* Passo 2 da RFC 1951: o primeiro código de cada comprimento. */
    unsigned code = 0;
    for (unsigned bits = 1; bits <= HUFF_MAX_BITS; bits++) {
        code = (code + count[bits - 1]) << 1;
        next[bits] = code;
    }
    /* Passo 3: símbolos do mesmo comprimento recebem códigos consecutivos. */
    for (unsigned i = 0; i < n; i++)
        codes[i] = lens[i] ? (uint16_t)next[lens[i]]++ : 0;
}

mgz_status huff_build(huff_decoder *d, const uint8_t *lens, unsigned n, huff_kind kind)
{
    unsigned count[HUFF_MAX_BITS + 1] = {0};
    unsigned next[HUFF_MAX_BITS + 1];

    if (n > HUFF_MAX_SYMS)
        return MGZ_ERR_CODE_LENGTHS;
    for (unsigned i = 0; i < n; i++) {
        if (lens[i] > HUFF_MAX_BITS)
            return MGZ_ERR_CODE_LENGTHS;
        count[lens[i]]++;
    }
    count[0] = 0;

    /*
     * Desigualdade de Kraft: começamos com 1 "vaga" na raiz da árvore; cada
     * nível dobra as vagas e cada código de comprimento len ocupa uma delas.
     * Vagas negativas: códigos demais (sobreposição). Vagas sobrando: código
     * incompleto, aceito só em casos especiais, como faz o zlib.
     */
    long left = 1;
    unsigned total = 0, max_len = 0;
    for (unsigned len = 1; len <= HUFF_MAX_BITS; len++) {
        left = left * 2 - (long)count[len];
        if (left < 0)
            return MGZ_ERR_CODE_LENGTHS;
        if (count[len] != 0)
            max_len = len;
        total += count[len];
    }
    if (total == 0) {
        if (kind != HUFF_DISTANCE)
            return MGZ_ERR_CODE_LENGTHS;
    } else if (left > 0) {
        if (kind == HUFF_COMPLETE || max_len != 1)
            return MGZ_ERR_CODE_LENGTHS;
    }

    memset(d->fast, 0, sizeof d->fast);
    unsigned code = 0, index = 0;
    for (unsigned len = 1; len <= HUFF_MAX_BITS; len++) {
        d->first_code[len] = (uint16_t)code;
        d->first_index[len] = (uint16_t)index;
        next[len] = code;
        code += count[len];
        d->limit[len] = (uint32_t)code << (16 - len);
        code <<= 1;
        index += count[len];
    }
    d->first_code[0] = 0;
    d->first_index[0] = 0;
    d->limit[0] = 0;
    d->limit[HUFF_MAX_BITS + 1] = 0x10000u;
    d->count = total;

    for (unsigned sym = 0; sym < n; sym++) {
        unsigned len = lens[sym];
        if (len == 0)
            continue;
        unsigned c = next[len]++;
        unsigned pos = d->first_index[len] + (c - d->first_code[len]);
        d->sorted_len[pos] = (uint8_t)len;
        d->sorted_sym[pos] = (uint16_t)sym;
        if (len <= HUFF_FAST_BITS) {
            /* Os bits chegam invertidos (LSB primeiro): preenchemos todas
               as entradas cujo prefixo, lido assim, é este código. */
            uint16_t entry = (uint16_t)((len << 9) | sym);
            for (unsigned j = huff_reverse(c, len); j < HUFF_FAST_SIZE; j += 1u << len)
                d->fast[j] = entry;
        }
    }
    return MGZ_OK;
}

mgz_status huff_decode_slow(bit_reader *br, const huff_decoder *d, unsigned *sym)
{
    /* Coloca os próximos 16 bits na ordem do código (MSB primeiro). */
    unsigned k = reverse16(br_peek(br, 16));
    unsigned len;
    for (len = HUFF_FAST_BITS + 1; len <= HUFF_MAX_BITS; len++)
        if (k < d->limit[len])
            break;
    if (len <= HUFF_MAX_BITS) {
        unsigned code = k >> (16 - len);
        if (code >= d->first_code[len]) {
            unsigned pos = code - d->first_code[len] + d->first_index[len];
            if (pos < d->count && d->sorted_len[pos] == len) {
                if (len > br->nbits)
                    return br_short_status(br);
                br_drop(br, len);
                *sym = d->sorted_sym[pos];
                return MGZ_OK;
            }
        }
    }
    /* Sem código válido: se os dados acabaram, o problema é truncamento. */
    if (br->eof && br->nbits < HUFF_MAX_BITS)
        return br_short_status(br);
    return MGZ_ERR_CODE;
}

/* --------------------------------------------------------- package-merge */

/*
 * O package-merge resolve o "problema do colecionador de moedas": cada
 * símbolo vira uma moeda em cada nível de profundidade 1..L, com valor
 * de face 2^-nível e custo igual à sua frequência. Precisamos escolher
 * moedas somando n - 1 com custo mínimo; o comprimento do código de um
 * símbolo é quantas moedas dele foram escolhidas.
 *
 * Na prática: no nível mais fundo a lista são as folhas ordenadas. Para
 * subir um nível, juntamos os itens da lista de baixo dois a dois
 * ("pacotes") e intercalamos com as folhas. No nível de cima escolhemos os
 * 2n - 2 itens mais baratos e descemos contando quantas folhas cada nível
 * usou.
 */
void huff_limited_lengths(const uint32_t *freq, unsigned n, unsigned max_bits, uint8_t *lens)
{
    unsigned sym[HUFF_MAX_SYMS];
    unsigned m = 0;

    if (n > HUFF_MAX_SYMS)
        n = HUFF_MAX_SYMS;
    if (max_bits > HUFF_MAX_BITS)
        max_bits = HUFF_MAX_BITS;
    for (unsigned i = 0; i < n; i++) {
        lens[i] = 0;
        if (freq[i] != 0)
            sym[m++] = i;
    }
    if (m == 0)
        return;
    if (m == 1) {
        lens[sym[0]] = 1;
        return;
    }

    /* Ordena por frequência (e pelo símbolo, para ser determinístico). */
    for (unsigned i = 1; i < m; i++) {
        unsigned s = sym[i], j = i;
        while (j > 0 && freq[sym[j - 1]] > freq[s]) {
            sym[j] = sym[j - 1];
            j--;
        }
        sym[j] = s;
    }

    static const unsigned MAX_ITEMS = 2 * HUFF_MAX_SYMS;
    uint64_t weight[2][2 * HUFF_MAX_SYMS];
    uint8_t is_leaf[HUFF_MAX_BITS][2 * HUFF_MAX_SYMS];
    unsigned size[HUFF_MAX_BITS];

    /* Nível mais fundo: só folhas. */
    unsigned deep = max_bits - 1;
    for (unsigned i = 0; i < m; i++) {
        weight[deep & 1u][i] = freq[sym[i]];
        is_leaf[deep][i] = 1;
    }
    size[deep] = m;

    /* Sobe de nível: pacotes do nível de baixo intercalados com as folhas. */
    for (unsigned lev = deep; lev-- > 0;) {
        const uint64_t *below = weight[(lev + 1) & 1u];
        uint64_t *cur = weight[lev & 1u];
        unsigned npk = size[lev + 1] / 2, li = 0, pi = 0, k = 0;
        while ((li < m || pi < npk) && k < MAX_ITEMS) {
            uint64_t pw = pi < npk ? below[2 * pi] + below[2 * pi + 1] : 0;
            if (li < m && (pi >= npk || (uint64_t)freq[sym[li]] <= pw)) {
                cur[k] = freq[sym[li++]];
                is_leaf[lev][k++] = 1;
            } else {
                cur[k] = pw;
                is_leaf[lev][k++] = 0;
                pi++;
            }
        }
        size[lev] = k;
    }

    /* Escolhe 2m - 2 itens no topo e desce contando as folhas usadas. */
    unsigned take = 2 * m - 2;
    for (unsigned lev = 0; lev <= deep && take > 0; lev++) {
        unsigned leaves = 0;
        if (take > size[lev])
            take = size[lev];
        for (unsigned j = 0; j < take; j++)
            leaves += is_leaf[lev][j];
        for (unsigned i = 0; i < leaves; i++)
            lens[sym[i]]++;
        take = 2 * (take - leaves);
    }
}
