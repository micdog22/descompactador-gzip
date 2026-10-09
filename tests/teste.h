/* teste.h: um mini framework de testes, sem dependências. */
#ifndef TESTE_H
#define TESTE_H

#include <stdio.h>
#include <string.h>

static int tests_run;
static int tests_failed;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        tests_run++;                                                                \
        if (!(cond)) {                                                              \
            tests_failed++;                                                         \
            fprintf(stderr, "  FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        long long va_ = (long long)(a), vb_ = (long long)(b);                       \
        tests_run++;                                                                \
        if (va_ != vb_) {                                                           \
            tests_failed++;                                                         \
            fprintf(stderr, "  FALHOU %s:%d: %s == %s (obtido %lld, esperado %lld)\n", \
                    __FILE__, __LINE__, #a, #b, va_, vb_);                          \
        }                                                                           \
    } while (0)

#define CHECK_MEM(a, b, n)                                                          \
    do {                                                                            \
        tests_run++;                                                                \
        if (memcmp((a), (b), (n)) != 0) {                                           \
            tests_failed++;                                                         \
            fprintf(stderr, "  FALHOU %s:%d: conteúdo de %s difere de %s\n",        \
                    __FILE__, __LINE__, #a, #b);                                    \
        }                                                                           \
    } while (0)

#define SECTION(name) printf("- %s\n", name)

static int test_summary(void)
{
    printf("%d verificações, %d falhas\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}

#endif /* TESTE_H */
