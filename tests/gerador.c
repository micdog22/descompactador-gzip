/*
 * gerador.c: cria arquivos de teste determinísticos e corrompe bytes.
 *
 *   gerador <tipo> <bytes> <saída>
 *       tipos: aleatorio, texto, zeros, repetitivo, misto, periodo
 *   gerador inverter <arquivo> <posição>
 *       inverte os bits do byte na posição (negativa conta a partir do fim)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state;

static uint64_t rng_next(void)
{
    uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static void fill_random(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)rng_next();
}

static void fill_text(uint8_t *buf, size_t n)
{
    static const char *words[] = {
        "a", "o", "de", "que", "não", "em", "para", "com", "uma", "os", "no", "se", "na",
        "por", "mais", "as", "dos", "como", "mas", "foi", "ao", "ele", "das", "tem", "à",
        "seu", "sua", "ou", "ser", "quando", "muito", "há", "nos", "já", "está", "eu",
        "também", "só", "pelo", "pela", "até", "isso", "ela", "entre", "era", "depois",
        "sem", "mesmo", "aos", "ter", "seus", "quem", "nas", "me", "esse", "eles",
        "estão", "você", "tinha", "foram", "essa", "num", "nem", "suas", "meu", "às",
        "compressão", "árvore", "código", "janela", "distância", "bloco", "Huffman"};
    const size_t nwords = sizeof words / sizeof words[0];
    size_t i = 0, since_dot = 0;
    while (i < n) {
        const char *w = words[rng_next() % nwords];
        size_t wl = strlen(w);
        for (size_t k = 0; k < wl && i < n; k++)
            buf[i++] = (uint8_t)w[k];
        since_dot++;
        if (i >= n)
            break;
        if (since_dot > 6 && rng_next() % 5 == 0) {
            buf[i++] = '.';
            since_dot = 0;
            if (i < n)
                buf[i++] = rng_next() % 4 == 0 ? '\n' : ' ';
        } else {
            buf[i++] = ' ';
        }
    }
}

static void fill_repetitive(uint8_t *buf, size_t n)
{
    static const char *pattern = "abcabcabd ";
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)pattern[i % 10];
    /* Algumas variações para não ficar trivial demais. */
    for (size_t i = 997; i < n; i += 4099)
        buf[i] = (uint8_t)('A' + rng_next() % 26);
}

static void fill_mixed(uint8_t *buf, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t len = 2000 + (size_t)(rng_next() % 60000);
        if (len > n - i)
            len = n - i;
        switch (rng_next() % 4) {
        case 0: fill_random(buf + i, len); break;
        case 1: memset(buf + i, 0, len); break;
        case 2: fill_repetitive(buf + i, len); break;
        default: fill_text(buf + i, len); break;
        }
        i += len;
    }
}

static void fill_period(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = i < 32768 ? (uint8_t)rng_next() : buf[i - 32768];
}

static int flip(const char *path, long pos)
{
    FILE *f = fopen(path, "r+b");
    if (f == NULL) {
        fprintf(stderr, "gerador: não foi possível abrir %s\n", path);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 1;
    }
    long size = ftell(f);
    long at = pos < 0 ? size + pos : pos;
    if (at < 0 || at >= size) {
        fprintf(stderr, "gerador: posição %ld fora do arquivo (%ld bytes)\n", pos, size);
        fclose(f);
        return 1;
    }
    int c;
    if (fseek(f, at, SEEK_SET) != 0 || (c = fgetc(f)) == EOF || fseek(f, at, SEEK_SET) != 0 ||
        fputc(c ^ 0xFF, f) == EOF) {
        fclose(f);
        return 1;
    }
    return fclose(f) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "inverter") == 0)
        return flip(argv[2], strtol(argv[3], NULL, 10));
    if (argc != 4) {
        fprintf(stderr, "uso: gerador <tipo> <bytes> <saída> | gerador inverter <arquivo> <posição>\n");
        return 2;
    }
    const char *type = argv[1];
    size_t n = (size_t)strtoull(argv[2], NULL, 10);
    uint8_t *buf = malloc(n ? n : 1);
    if (buf == NULL)
        return 1;
    /* Semente fixa, diferente por tipo e tamanho. */
    rng_state = UINT64_C(0x5EED) ^ ((uint64_t)n << 8) ^ (uint64_t)(unsigned char)type[0];

    if (strcmp(type, "aleatorio") == 0)
        fill_random(buf, n);
    else if (strcmp(type, "texto") == 0)
        fill_text(buf, n);
    else if (strcmp(type, "zeros") == 0)
        memset(buf, 0, n);
    else if (strcmp(type, "repetitivo") == 0)
        fill_repetitive(buf, n);
    else if (strcmp(type, "misto") == 0)
        fill_mixed(buf, n);
    else if (strcmp(type, "periodo") == 0)
        fill_period(buf, n);
    else {
        fprintf(stderr, "gerador: tipo desconhecido: %s\n", type);
        free(buf);
        return 2;
    }

    FILE *f = fopen(argv[3], "wb");
    int rc = 0;
    if (f == NULL || fwrite(buf, 1, n, f) != n)
        rc = 1;
    if (f != NULL && fclose(f) != 0)
        rc = 1;
    free(buf);
    return rc;
}
