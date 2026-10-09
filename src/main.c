/* main.c: mini-gzip, a interface de linha de comando. */
#include "mgz.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { MODE_COMPRESS, MODE_DECOMPRESS, MODE_TEST, MODE_LIST, MODE_BLOCKS } run_mode;

typedef struct {
    run_mode mode;
    int to_stdout;
    int keep;
    int force;
    int no_name;
    int verbose;
    int level;
} options;

enum { EXIT_OK = 0, EXIT_ERROR = 1, EXIT_WARNING = 2 };

static const char *PROGRAM = "mini-gzip";

/* --------------------------------------------------------- E/S de arquivos */

typedef struct {
    FILE *f;
    uint64_t count;
} file_io;

static int file_read(void *ctx, uint8_t *buf, size_t cap, size_t *got)
{
    file_io *io = ctx;
    size_t n = fread(buf, 1, cap, io->f);
    if (n == 0 && ferror(io->f))
        return 1;
    io->count += n;
    *got = n;
    return 0;
}

static int file_write(void *ctx, const uint8_t *buf, size_t len)
{
    file_io *io = ctx;
    if (fwrite(buf, 1, len, io->f) != len)
        return 1;
    io->count += len;
    return 0;
}

static void warn(const char *name, const char *msg)
{
    fprintf(stderr, "%s: %s: %s\n", PROGRAM, name, msg);
}

static const char *open_error(void)
{
#ifdef ENOENT
    if (errno == ENOENT)
        return "arquivo não encontrado";
#endif
#ifdef EACCES
    if (errno == EACCES)
        return "permissão negada";
#endif
#ifdef EISDIR
    if (errno == EISDIR)
        return "é um diretório";
#endif
    return "não foi possível abrir o arquivo";
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a > b && strcmp(s + a - b, suffix) == 0;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');
    if (back != NULL && (slash == NULL || back > slash))
        slash = back;
    return slash ? slash + 1 : path;
}

static char *concat(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *s = malloc(la + lb + 1);
    if (s != NULL) {
        memcpy(s, a, la);
        memcpy(s + la, b, lb + 1);
    }
    return s;
}

/* Nome de saída da descompactação: tira .gz ou troca .tgz por .tar. */
static char *decompressed_name(const char *path)
{
    size_t n = strlen(path);
    if (ends_with(path, ".gz")) {
        char *s = malloc(n - 2);
        if (s != NULL) {
            memcpy(s, path, n - 3);
            s[n - 3] = '\0';
        }
        return s;
    }
    if (ends_with(path, ".tgz")) {
        char *s = malloc(n + 1);
        if (s != NULL) {
            memcpy(s, path, n - 4);
            memcpy(s + n - 4, ".tar", 5);
        }
        return s;
    }
    return NULL;
}

/* Taxa de redução com uma casa decimal e vírgula: "63,2%" ou "-12,5%". */
static void format_ratio(char *buf, size_t cap, uint64_t compressed, uint64_t original)
{
    if (original == 0) {
        snprintf(buf, cap, "0,0%%");
        return;
    }
    /* Em décimos de ponto percentual, com arredondamento. */
    int negative = compressed > original;
    uint64_t diff = negative ? compressed - original : original - compressed;
    double tenths = (double)diff * 1000.0 / (double)original + 0.5;
    unsigned long long t = (unsigned long long)tenths;
    snprintf(buf, cap, "%s%llu,%llu%%", negative && t > 0 ? "-" : "", t / 10, t % 10);
}

/* ------------------------------------------------------------- operações */

static int compress_file(const char *path, const options *opt)
{
    int from_stdin = strcmp(path, "-") == 0;
    const char *shown = from_stdin ? "(entrada padrão)" : path;
    int use_stdout = opt->to_stdout || from_stdin;
    file_io in = {NULL, 0}, out = {NULL, 0};
    char *out_name = NULL;
    int rc = EXIT_OK;

    if (!from_stdin && ends_with(path, ".gz") && !opt->force) {
        warn(path, "já tem o sufixo .gz, ignorado (use -f para forçar)");
        return EXIT_WARNING;
    }
    in.f = from_stdin ? stdin : fopen(path, "rb");
    if (in.f == NULL) {
        warn(path, open_error());
        return EXIT_ERROR;
    }
    if (use_stdout) {
        out.f = stdout;
    } else {
        out_name = concat(path, ".gz");
        if (out_name == NULL) {
            warn(path, mgz_strerror(MGZ_ERR_MEMORY));
            fclose(in.f);
            return EXIT_ERROR;
        }
        if (!opt->force && file_exists(out_name)) {
            warn(out_name, "já existe; use -f para sobrescrever");
            fclose(in.f);
            free(out_name);
            return EXIT_ERROR;
        }
        /* "x" (C11): falha se o arquivo surgir entre a checagem e a criação. */
        out.f = fopen(out_name, opt->force ? "wb" : "wbx");
        if (out.f == NULL) {
            warn(out_name, open_error());
            fclose(in.f);
            free(out_name);
            return EXIT_ERROR;
        }
    }

    mgz_stream io;
    memset(&io, 0, sizeof io);
    io.read = file_read;
    io.read_ctx = &in;
    io.write = file_write;
    io.write_ctx = &out;
    mgz_gzip_options gopt = {opt->level, NULL, NULL, 0, 0};
    if (!from_stdin && !opt->no_name)
        gopt.name = base_name(path);

    mgz_status st = mgz_gzip(&io, &gopt);
    if (st == MGZ_OK && fflush(out.f) != 0)
        st = MGZ_ERR_WRITE;
    if (!use_stdout && fclose(out.f) != 0 && st == MGZ_OK)
        st = MGZ_ERR_WRITE;
    if (!from_stdin)
        fclose(in.f);

    if (st != MGZ_OK) {
        warn(shown, mgz_strerror(st));
        if (out_name != NULL)
            remove(out_name);
        rc = EXIT_ERROR;
    } else {
        if (opt->verbose) {
            char ratio[32];
            format_ratio(ratio, sizeof ratio, out.count, in.count);
            if (out_name != NULL)
                fprintf(stderr, "%s: %s de redução, gravado em %s\n", shown, ratio, out_name);
            else
                fprintf(stderr, "%s: %s de redução\n", shown, ratio);
        }
        if (!use_stdout && !opt->keep && remove(path) != 0)
            warn(path, "não foi possível apagar o arquivo original");
    }
    free(out_name);
    return rc;
}

static int decompress_file(const char *path, const options *opt)
{
    int from_stdin = strcmp(path, "-") == 0;
    const char *shown = from_stdin ? "(entrada padrão)" : path;
    int use_stdout = opt->to_stdout || from_stdin;
    file_io in = {NULL, 0}, out = {NULL, 0};
    char *out_name = NULL;
    int rc = EXIT_OK;

    if (!use_stdout) {
        out_name = decompressed_name(path);
        if (out_name == NULL) {
            warn(path, "sufixo desconhecido (esperado .gz ou .tgz), ignorado");
            return EXIT_WARNING;
        }
    }
    in.f = from_stdin ? stdin : fopen(path, "rb");
    if (in.f == NULL) {
        warn(path, open_error());
        free(out_name);
        return EXIT_ERROR;
    }
    if (use_stdout) {
        out.f = stdout;
    } else {
        if (!opt->force && file_exists(out_name)) {
            warn(out_name, "já existe; use -f para sobrescrever");
            fclose(in.f);
            free(out_name);
            return EXIT_ERROR;
        }
        /* "x" (C11): falha se o arquivo surgir entre a checagem e a criação. */
        out.f = fopen(out_name, opt->force ? "wb" : "wbx");
        if (out.f == NULL) {
            warn(out_name, open_error());
            fclose(in.f);
            free(out_name);
            return EXIT_ERROR;
        }
    }

    mgz_stream io;
    mgz_info info;
    memset(&io, 0, sizeof io);
    io.read = file_read;
    io.read_ctx = &in;
    io.write = file_write;
    io.write_ctx = &out;
    mgz_status st = mgz_gunzip(&io, &info);
    if (st == MGZ_OK && fflush(out.f) != 0)
        st = MGZ_ERR_WRITE;
    if (!use_stdout && fclose(out.f) != 0 && st == MGZ_OK)
        st = MGZ_ERR_WRITE;
    if (!from_stdin)
        fclose(in.f);

    if (st != MGZ_OK) {
        warn(shown, mgz_strerror(st));
        if (out_name != NULL)
            remove(out_name);
        rc = EXIT_ERROR;
    } else {
        if (info.trailing_garbage) {
            warn(shown, "dados extras depois do fim do gzip foram ignorados");
            rc = EXIT_WARNING;
        }
        if (opt->verbose) {
            char ratio[32];
            format_ratio(ratio, sizeof ratio, in.count, info.uncompressed_size);
            if (out_name != NULL)
                fprintf(stderr, "%s: %s de redução, restaurado em %s\n", shown, ratio, out_name);
            else
                fprintf(stderr, "%s: %s de redução\n", shown, ratio);
        }
        if (!use_stdout && !opt->keep && remove(path) != 0)
            warn(path, "não foi possível apagar o arquivo compactado");
    }
    free(out_name);
    return rc;
}

/* Abre a entrada e roda a descompactação só para verificar (sem saída). */
static mgz_status inspect(const char *path, mgz_stream *io, mgz_info *info, file_io *in)
{
    int from_stdin = strcmp(path, "-") == 0;
    in->f = from_stdin ? stdin : fopen(path, "rb");
    in->count = 0;
    if (in->f == NULL)
        return MGZ_ERR_READ;
    io->read = file_read;
    io->read_ctx = in;
    io->write = NULL;
    mgz_status st = mgz_gunzip(io, info);
    if (!from_stdin)
        fclose(in->f);
    return st;
}

static int test_file(const char *path, const options *opt)
{
    const char *shown = strcmp(path, "-") == 0 ? "(entrada padrão)" : path;
    mgz_stream io;
    mgz_info info;
    file_io in;
    memset(&io, 0, sizeof io);
    mgz_status st = inspect(path, &io, &info, &in);
    if (in.f == NULL) {
        warn(shown, open_error());
        return EXIT_ERROR;
    }
    if (st != MGZ_OK) {
        warn(shown, mgz_strerror(st));
        return EXIT_ERROR;
    }
    if (info.trailing_garbage) {
        warn(shown, "dados extras depois do fim do gzip");
        return EXIT_WARNING;
    }
    if (opt->verbose)
        printf("%s: OK (%u membro%s, CRC-32 e ISIZE conferidos)\n", shown, info.members,
               info.members == 1 ? "" : "s");
    return EXIT_OK;
}

typedef struct {
    uint64_t compressed, original;
    unsigned files;
} list_totals;

static int list_file(const char *path, list_totals *tot)
{
    const char *shown = strcmp(path, "-") == 0 ? "(entrada padrão)" : path;
    mgz_stream io;
    mgz_info info;
    file_io in;
    memset(&io, 0, sizeof io);
    mgz_status st = inspect(path, &io, &info, &in);
    if (in.f == NULL) {
        warn(shown, open_error());
        return EXIT_ERROR;
    }
    if (st != MGZ_OK) {
        warn(shown, mgz_strerror(st));
        return EXIT_ERROR;
    }
    char ratio[32];
    format_ratio(ratio, sizeof ratio, in.count, info.uncompressed_size);
    const char *name = shown;
    char *derived = NULL;
    if (info.first.has_name) {
        name = info.first.name;
    } else if ((derived = decompressed_name(base_name(path))) != NULL) {
        name = derived;
    }
    printf("%12llu %12llu %7s  %s\n", (unsigned long long)in.count,
           (unsigned long long)info.uncompressed_size, ratio, name);
    free(derived);
    tot->compressed += in.count;
    tot->original += info.uncompressed_size;
    tot->files++;
    return info.trailing_garbage ? EXIT_WARNING : EXIT_OK;
}

/* ------------------------------------------------- --blocos (didático) */

static const char *os_name(unsigned os)
{
    static const char *names[] = {"FAT", "Amiga", "VMS", "Unix", "VM/CMS", "Atari TOS",
                                  "HPFS", "Macintosh", "Z-System", "CP/M", "TOPS-20",
                                  "NTFS", "QDOS", "Acorn RISCOS"};
    if (os < sizeof names / sizeof names[0])
        return names[os];
    return os == 255 ? "desconhecido" : "?";
}

/* Imprime s alinhado à esquerda em width colunas, contando caracteres
   UTF-8 (e não bytes), para que "dinâmico" e "não" fiquem alinhados. */
static void print_padded(const char *s, int width)
{
    int chars = 0;
    for (const char *p = s; *p; p++)
        if (((unsigned char)*p & 0xC0u) != 0x80u)
            chars++;
    printf("%s", s);
    for (; chars < width; chars++)
        putchar(' ');
}

static void show_member(void *ctx, int stage, const mgz_member_info *m)
{
    (void)ctx;
    if (stage == MGZ_MEMBER_START) {
        printf("membro %u: MTIME %lu, XFL %u, SO %u (%s)", m->number, (unsigned long)m->mtime,
               m->xfl, m->os, os_name(m->os));
        if (m->has_name)
            printf(", nome \"%s\"", m->name);
        if (m->has_comment)
            printf(", comentário \"%s\"", m->comment);
        if (m->extra_len)
            printf(", FEXTRA de %u bytes", m->extra_len);
        if (m->has_header_crc)
            printf(", FHCRC conferido");
        printf("\n  %5s  %-12s %-6s %16s %14s  %s\n", "bloco", "tipo", "final", "bits comprimidos",
               "bytes gerados", "tabelas");
    } else {
        printf("  fim do membro: CRC-32 %08lx e ISIZE %lu conferidos\n", (unsigned long)m->crc32,
               (unsigned long)m->isize);
    }
}

static void show_block(void *ctx, const mgz_block_info *b)
{
    static const char *types[] = {"armazenado", "fixo", "dinâmico"};
    (void)ctx;
    printf("  %5u  ", b->index);
    print_padded(types[b->type], 13);
    print_padded(b->final ? "sim" : "não", 7);
    printf("%16llu %14llu", (unsigned long long)(b->bit_end - b->bit_start),
           (unsigned long long)b->out_bytes);
    if (b->type == MGZ_BLOCK_DYNAMIC)
        printf("  HLIT %u, HDIST %u, HCLEN %u", b->hlit, b->hdist, b->hclen);
    printf("\n");
}

static int blocks_file(const char *path)
{
    const char *shown = strcmp(path, "-") == 0 ? "(entrada padrão)" : path;
    mgz_stream io;
    mgz_info info;
    file_io in;
    memset(&io, 0, sizeof io);
    io.on_block = show_block;
    io.on_member = show_member;
    printf("%s\n", shown);
    mgz_status st = inspect(path, &io, &info, &in);
    if (in.f == NULL) {
        warn(shown, open_error());
        return EXIT_ERROR;
    }
    if (st != MGZ_OK) {
        fflush(stdout);
        warn(shown, mgz_strerror(st));
        return EXIT_ERROR;
    }
    char ratio[32];
    format_ratio(ratio, sizeof ratio, info.compressed_size, info.uncompressed_size);
    printf("total: %u membro%s, %llu bytes comprimidos, %llu originais (%s de redução)\n",
           info.members, info.members == 1 ? "" : "s",
           (unsigned long long)info.compressed_size,
           (unsigned long long)info.uncompressed_size, ratio);
    if (info.trailing_garbage)
        printf("aviso: há dados extras depois do último membro\n");
    return info.trailing_garbage ? EXIT_WARNING : EXIT_OK;
}

/* ------------------------------------------------------------ argumentos */

static void usage(FILE *f)
{
    fprintf(f,
            "uso: mini-gzip [opções] [arquivo ...]\n"
            "\n"
            "Compacta arquivos no formato gzip (RFC 1952) ou, com -d, descompacta.\n"
            "Sem arquivos, ou com \"-\", usa a entrada e a saída padrão.\n"
            "\n"
            "  -c, --stdout       escreve na saída padrão e mantém os originais\n"
            "  -d, --decompress   descompacta\n"
            "  -k, --keep         mantém o arquivo de entrada\n"
            "  -f, --force        sobrescreve a saída se ela já existir\n"
            "  -t, --test         testa a integridade (CRC-32 e tamanho)\n"
            "  -l, --list         lista tamanhos, taxa de redução e nome original\n"
            "  -n, --no-name      não grava o nome original no cabeçalho\n"
            "  -v, --verbose      mostra a taxa de redução de cada arquivo\n"
            "  -1 a -9            nível: -1 é o mais rápido, -9 comprime mais (padrão -6)\n"
            "      --fast         o mesmo que -1\n"
            "      --best         o mesmo que -9\n"
            "      --blocos       mostra os membros e os blocos DEFLATE de um .gz\n"
            "  -h, --help         mostra esta ajuda\n"
            "  -V, --version      mostra a versão\n"
            "\n"
            "Exemplos:\n"
            "  mini-gzip relatorio.txt         cria relatorio.txt.gz e apaga o original\n"
            "  mini-gzip -9k dados.csv         nível 9, mantendo dados.csv\n"
            "  mini-gzip -d relatorio.txt.gz   restaura relatorio.txt\n"
            "  mini-gzip -l *.gz               mostra tamanhos e taxas\n");
}

static int parse_long(const char *arg)
{
    static const struct {
        const char *name;
        char letter;
    } table[] = {{"stdout", 'c'}, {"to-stdout", 'c'}, {"decompress", 'd'}, {"uncompress", 'd'},
                 {"keep", 'k'},   {"force", 'f'},     {"test", 't'},       {"list", 'l'},
                 {"no-name", 'n'}, {"verbose", 'v'},  {"fast", '1'},       {"best", '9'},
                 {"help", 'h'},   {"version", 'V'},   {"blocos", 'B'}};
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(arg, table[i].name) == 0)
            return table[i].letter;
    return 0;
}

/* Aplica uma opção curta. Retorna 0 se ok, -1 para sair com sucesso, 1 em erro. */
static int apply_option(int c, options *opt)
{
    switch (c) {
    case 'c': opt->to_stdout = 1; break;
    case 'd': opt->mode = MODE_DECOMPRESS; break;
    case 'k': opt->keep = 1; break;
    case 'f': opt->force = 1; break;
    case 't': opt->mode = MODE_TEST; break;
    case 'l': opt->mode = MODE_LIST; break;
    case 'n': opt->no_name = 1; break;
    case 'v': opt->verbose = 1; break;
    case 'B': opt->mode = MODE_BLOCKS; break;
    case 'h': usage(stdout); return -1;
    case 'V': printf("mini-gzip %s\n", MGZ_VERSION); return -1;
    default:
        if (c >= '1' && c <= '9') {
            opt->level = c - '0';
            break;
        }
        fprintf(stderr, "%s: opção inválida: -%c\n", PROGRAM, c);
        fprintf(stderr, "use \"mini-gzip --help\" para ver as opções\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    options opt = {MODE_COMPRESS, 0, 0, 0, 0, 0, 6};
    const char **files = malloc(sizeof(char *) * (size_t)(argc + 1));
    int nfiles = 0, only_files = 0;

    if (files == NULL) {
        fprintf(stderr, "%s: %s\n", PROGRAM, mgz_strerror(MGZ_ERR_MEMORY));
        return EXIT_ERROR;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (only_files || a[0] != '-' || a[1] == '\0') {
            files[nfiles++] = a;
            continue;
        }
        if (strcmp(a, "--") == 0) {
            only_files = 1;
            continue;
        }
        int r = 0;
        if (a[1] == '-') {
            int c = parse_long(a + 2);
            if (c == 0) {
                fprintf(stderr, "%s: opção inválida: %s\n", PROGRAM, a);
                fprintf(stderr, "use \"mini-gzip --help\" para ver as opções\n");
                r = 1;
            } else {
                r = apply_option(c, &opt);
            }
        } else {
            for (const char *p = a + 1; *p && r == 0; p++)
                r = apply_option(*p == 'B' ? '?' : *p, &opt);
        }
        if (r != 0) {
            free(files);
            return r < 0 ? EXIT_OK : EXIT_ERROR;
        }
    }
    if (nfiles == 0)
        files[nfiles++] = "-";

    int rc = EXIT_OK;
    list_totals tot = {0, 0, 0};
    if (opt.mode == MODE_LIST)
        printf("%12s %12s %7s  %s\n", "comprimido", "original", "taxa", "nome original");
    for (int i = 0; i < nfiles; i++) {
        int r;
        switch (opt.mode) {
        case MODE_COMPRESS: r = compress_file(files[i], &opt); break;
        case MODE_DECOMPRESS: r = decompress_file(files[i], &opt); break;
        case MODE_TEST: r = test_file(files[i], &opt); break;
        case MODE_LIST: r = list_file(files[i], &tot); break;
        default: r = blocks_file(files[i]); break;
        }
        /* Erro tem precedência sobre aviso, como no gzip. */
        if (r == EXIT_ERROR || (r == EXIT_WARNING && rc == EXIT_OK))
            rc = r;
    }
    if (opt.mode == MODE_LIST && tot.files > 1) {
        char ratio[32];
        format_ratio(ratio, sizeof ratio, tot.compressed, tot.original);
        printf("%12llu %12llu %7s  (total)\n", (unsigned long long)tot.compressed,
               (unsigned long long)tot.original, ratio);
    }
    free(files);
    return rc;
}
