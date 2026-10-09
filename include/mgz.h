/*
 * mgz.h: API pública da biblioteca do Descompactador Gzip.
 *
 * A biblioteca implementa do zero o formato DEFLATE (RFC 1951) e o
 * contêiner gzip (RFC 1952). Tudo funciona em fluxo: a entrada chega por
 * uma função de leitura e a saída sai por uma função de escrita, então
 * arquivos de qualquer tamanho são processados com memória constante
 * (cerca de 1 MiB para compactar e 200 KiB para descompactar).
 */
#ifndef MGZ_H
#define MGZ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MGZ_VERSION "1.0.0"

typedef enum {
    MGZ_OK = 0,
    MGZ_ERR_MEMORY,        /* falta de memória */
    MGZ_ERR_READ,          /* a função de leitura informou erro */
    MGZ_ERR_WRITE,         /* a função de escrita informou erro */
    MGZ_ERR_TRUNCATED,     /* os dados terminaram antes do esperado */
    MGZ_ERR_HEADER,        /* assinatura gzip (31, 139) ausente */
    MGZ_ERR_METHOD,        /* CM diferente de 8 */
    MGZ_ERR_FLAGS,         /* bits reservados de FLG ligados */
    MGZ_ERR_HEADER_CRC,    /* FHCRC não confere */
    MGZ_ERR_CRC,           /* CRC-32 do trailer não confere */
    MGZ_ERR_SIZE,          /* ISIZE do trailer não confere */
    MGZ_ERR_BLOCK_TYPE,    /* BTYPE 11 (reservado) */
    MGZ_ERR_STORED_LEN,    /* LEN e NLEN de bloco armazenado não batem */
    MGZ_ERR_CODE_LENGTHS,  /* comprimentos de código inválidos */
    MGZ_ERR_CODE,          /* código ou símbolo inválido nos dados */
    MGZ_ERR_DISTANCE,      /* distância antes do início dos dados */
    MGZ_ERR_ARGUMENT       /* parâmetro inválido */
} mgz_status;

/*
 * Leitura: preenche buf com até cap bytes e guarda em *got quantos leu.
 * *got == 0 significa fim dos dados. Retorne 0 em sucesso e qualquer outro
 * valor em erro de E/S.
 */
typedef int (*mgz_read_fn)(void *ctx, uint8_t *buf, size_t cap, size_t *got);

/* Escrita: recebe len bytes. Retorne 0 em sucesso e outro valor em erro. */
typedef int (*mgz_write_fn)(void *ctx, const uint8_t *buf, size_t len);

/* Tipos de bloco DEFLATE (o valor é o próprio BTYPE). */
enum { MGZ_BLOCK_STORED = 0, MGZ_BLOCK_FIXED = 1, MGZ_BLOCK_DYNAMIC = 2 };

/* Informações de um bloco DEFLATE decodificado (para fins didáticos). */
typedef struct {
    unsigned member;        /* membro gzip (1, 2, ...); 0 em DEFLATE puro */
    unsigned index;         /* número do bloco dentro do fluxo (1, 2, ...) */
    int type;               /* MGZ_BLOCK_STORED, _FIXED ou _DYNAMIC */
    int final;              /* BFINAL */
    uint64_t bit_start;     /* posição do cabeçalho do bloco, em bits */
    uint64_t bit_end;       /* posição logo após o fim do bloco, em bits */
    uint64_t out_bytes;     /* bytes produzidos pelo bloco */
    unsigned hlit, hdist, hclen; /* tamanhos das tabelas (só blocos dinâmicos) */
} mgz_block_info;

typedef void (*mgz_block_fn)(void *ctx, const mgz_block_info *info);

/* Cabeçalho e trailer de um membro gzip. */
typedef struct {
    unsigned number;        /* 1, 2, ... */
    uint8_t flags;          /* FLG */
    uint32_t mtime;         /* MTIME (0 = não informado) */
    uint8_t xfl;            /* XFL */
    uint8_t os;             /* OS */
    unsigned extra_len;     /* tamanho do campo FEXTRA (0 se ausente) */
    char name[256];         /* FNAME (truncado em 255 bytes) */
    int has_name;
    char comment[256];      /* FCOMMENT (truncado em 255 bytes) */
    int has_comment;
    int has_header_crc;     /* FHCRC presente (e conferido) */
    /* Preenchidos só no fim do membro: */
    uint32_t crc32;         /* CRC-32 conferido */
    uint32_t isize;         /* ISIZE conferido */
    uint64_t out_bytes;     /* bytes descompactados do membro */
} mgz_member_info;

enum { MGZ_MEMBER_START = 0, MGZ_MEMBER_END = 1 };

typedef void (*mgz_member_fn)(void *ctx, int stage, const mgz_member_info *info);

/* Entrada, saída e observadores de um processamento. */
typedef struct {
    mgz_read_fn read;
    void *read_ctx;
    mgz_write_fn write;        /* pode ser NULL: os dados são só verificados */
    void *write_ctx;
    mgz_block_fn on_block;     /* opcional */
    void *block_ctx;
    mgz_member_fn on_member;   /* opcional (só no gzip) */
    void *member_ctx;
} mgz_stream;

typedef struct {
    int level;                 /* 0 (só armazena) a 9; use 6 se não souber */
    const char *name;          /* FNAME opcional (NULL: não grava) */
    const char *comment;       /* FCOMMENT opcional */
    uint32_t mtime;            /* MTIME; 0 significa "não informado" */
    int header_crc;            /* diferente de zero: grava FHCRC */
} mgz_gzip_options;

/* Resumo de uma descompactação gzip completa. */
typedef struct {
    unsigned members;          /* membros encontrados */
    uint64_t uncompressed_size;/* total de bytes produzidos */
    uint64_t compressed_size;  /* bytes da entrada até o fim do último membro */
    int trailing_garbage;      /* havia bytes que não são gzip depois do fim */
    mgz_member_info first;     /* cabeçalho do primeiro membro */
} mgz_info;

/* CRC-32 (o mesmo do gzip, zip e PNG). Comece com crc = 0. */
uint32_t mgz_crc32(uint32_t crc, const void *data, size_t len);

/* Mensagem de erro em português. */
const char *mgz_strerror(mgz_status st);

/* Compacta tudo o que io->read entregar e escreve um arquivo gzip. */
mgz_status mgz_gzip(const mgz_stream *io, const mgz_gzip_options *opt);

/* Descompacta um ou mais membros gzip concatenados (info pode ser NULL). */
mgz_status mgz_gunzip(const mgz_stream *io, mgz_info *info);

/* DEFLATE puro, sem contêiner (útil para testes e estudo). */
mgz_status mgz_deflate(const mgz_stream *io, int level);
mgz_status mgz_inflate(const mgz_stream *io);

/* Atalhos em memória. *out é alocado com malloc; libere com free. */
mgz_status mgz_gzip_buffer(const uint8_t *in, size_t len, int level,
                           uint8_t **out, size_t *out_len);
mgz_status mgz_gunzip_buffer(const uint8_t *in, size_t len,
                             uint8_t **out, size_t *out_len, mgz_info *info);
mgz_status mgz_deflate_buffer(const uint8_t *in, size_t len, int level,
                              uint8_t **out, size_t *out_len);
mgz_status mgz_inflate_buffer(const uint8_t *in, size_t len,
                              uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MGZ_H */
