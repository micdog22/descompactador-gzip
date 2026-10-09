# Descompactador Gzip

DEFLATE implementado do zero em C: um `mini-gzip` que compacta e descompacta arquivos `.gz` compatíveis com o gzip de verdade, escrito para quem quer entender como LZ77, códigos de Huffman canônicos e o formato gzip funcionam por dentro. Sem zlib, sem dependências, só C11 e a biblioteca padrão.

## Recursos

- **Descompactador completo (RFC 1951):** blocos armazenados, Huffman fixo e Huffman dinâmico, referências de até 32 KiB para trás, validação rigorosa das tabelas (as mesmas regras do zlib).
- **Compressor de verdade:** LZ77 com cadeias de hash, janela de 32 KiB, avaliação preguiçosa (níveis 4 a 9) e estratégia gulosa (níveis 1 a 3), com os mesmos parâmetros do zlib.
- **Huffman ótimo com limite de 15 bits** pelo algoritmo package-merge, e escolha do tipo de bloco pelo custo exato em bits.
- **Contêiner gzip (RFC 1952):** FTEXT, FHCRC, FEXTRA, FNAME, FCOMMENT, MTIME, XFL, OS, trailer com CRC-32 e ISIZE conferidos, membros concatenados.
- **Funciona em fluxo:** memória constante (cerca de 1 MiB para compactar e 200 KiB para descompactar) para arquivos de qualquer tamanho, inclusive pela entrada e saída padrão.
- **Erros claros em português:** cabeçalho inválido, CRC que não confere, dados truncados, distância muito para trás, comprimentos de código inválidos.
- **Modo didático `--blocos`:** mostra cada membro e cada bloco DEFLATE de um arquivo, com tipo, tamanho em bits e tabelas.
- **Testado contra o gzip do sistema** nos dois sentidos, com dados de 0 byte a 1 MiB, arquivos corrompidos e truncados.

## Como usar

```
uso: mini-gzip [opções] [arquivo ...]

  -c, --stdout       escreve na saída padrão e mantém os originais
  -d, --decompress   descompacta
  -k, --keep         mantém o arquivo de entrada
  -f, --force        sobrescreve a saída se ela já existir
  -t, --test         testa a integridade (CRC-32 e tamanho)
  -l, --list         lista tamanhos, taxa de redução e nome original
  -n, --no-name      não grava o nome original no cabeçalho
  -v, --verbose      mostra a taxa de redução de cada arquivo
  -1 a -9            nível: -1 é o mais rápido, -9 comprime mais (padrão -6)
      --blocos       mostra os membros e os blocos DEFLATE de um .gz
```

Como no gzip, compactar troca `arquivo` por `arquivo.gz` (use `-k` para manter o original) e `-d` faz o caminho inverso. Sem arquivos, ou com `-`, usa a entrada e a saída padrão.

```
$ mini-gzip -v -k texto.txt
texto.txt: 71,9% de redução, gravado em texto.txt.gz

$ mini-gzip -l texto.txt.gz
  comprimido     original    taxa  nome original
       56209       200000   71,9%  texto.txt

$ mini-gzip -tv texto.txt.gz
texto.txt.gz: OK (1 membro, CRC-32 e ISIZE conferidos)

$ gzip -dc texto.txt.gz | cmp - texto.txt && echo "o gzip do sistema concorda"
o gzip do sistema concorda
```

O modo `--blocos` abre o arquivo e mostra a estrutura interna:

```
$ mini-gzip --blocos texto.txt.gz
texto.txt.gz
membro 1: MTIME 0, XFL 0, SO 255 (desconhecido), nome "texto.txt"
  bloco  tipo         final  bits comprimidos  bytes gerados  tabelas
      1  dinâmico     não              262726         115357  HLIT 272, HDIST 30, HCLEN 17
      2  dinâmico     sim              186722          84643  HLIT 271, HDIST 30, HCLEN 15
  fim do membro: CRC-32 f4f5de69 e ISIZE 200000 conferidos
total: 1 membro, 56209 bytes comprimidos, 200000 originais (71,9% de redução)
```

Arquivos estragados são recusados com uma mensagem que diz o que aconteceu (código de saída 1):

```
$ mini-gzip -t estragado.gz
mini-gzip: estragado.gz: CRC-32 não confere: os dados estão corrompidos

$ mini-gzip -d cortado.gz
mini-gzip: cortado.gz: dados truncados: o arquivo terminou antes do esperado
```

### Como biblioteca

A API pública está em [`include/mgz.h`](include/mgz.h). Para dados em memória:

```c
#include "mgz.h"

uint8_t *gz;
size_t gz_len;
mgz_status st = mgz_gzip_buffer(dados, tamanho, 9, &gz, &gz_len);
if (st != MGZ_OK)
    fprintf(stderr, "erro: %s\n", mgz_strerror(st));
free(gz);
```

Para arquivos grandes, `mgz_gzip` e `mgz_gunzip` recebem uma função de leitura e uma de escrita (`mgz_stream`), então nada precisa caber na memória. Também há `mgz_deflate` e `mgz_inflate` para DEFLATE puro e `mgz_crc32`.

## Como compilar

Precisa de um compilador C11 (clang ou gcc) e `make` ou `cmake`. Testado no macOS com clang; o código usa só a biblioteca padrão do C, sem chamadas de sistema.

```sh
make                 # gera build/mini-gzip e build/libmgz.a
```

Ou com CMake:

```sh
cmake -S . -B build
cmake --build build
```

O executável fica em `build/mini-gzip`.

## Testes

```sh
make test            # testes de unidade + testes diferenciais contra o gzip do sistema
make sanitize        # os mesmos testes com AddressSanitizer e UndefinedBehaviorSanitizer
ctest --test-dir build --output-on-failure   # depois do build com CMake
```

O que é verificado:

- **Unidade** (`tests/test_unidade.c`): leitor e escritor de bits; códigos canônicos com o exemplo da RFC 1951; regras de validação das tabelas; package-merge comparado com o Huffman clássico e com força bruta; CRC-32 (`"123456789"` dá `cbf43926`); os três tipos de bloco com vetores gerados pelo zlib; distância máxima de 32768; todos os erros (BTYPE 11, LEN/NLEN, distância, códigos reservados, tabelas sobrepostas, falta do fim de bloco); todos os prefixos de um arquivo truncado; 3000 bits trocados ao acaso sem nenhuma falha silenciosa; ida e volta em todos os níveis com tamanhos nas bordas da janela (32767, 32768, 65536, 131072 bytes...); 40 mil entradas aleatórias e mutadas.
- **Diferencial** (`tests/diferencial.sh`): o nosso compressor e depois o `gzip -d` do sistema; o `gzip -1` e o `gzip -9` do sistema e depois o nosso descompactador; arquivos vazios, de 1 byte, texto, aleatórios, muito repetitivos e de 1 MiB, comparados com `cmp`; um byte trocado precisa dar erro de CRC; arquivos truncados precisam dar erro; membros concatenados; as opções da linha de comando; o nome gravado no cabeçalho lido pelo `gzip -N`.

Os testes diferenciais também imprimem uma comparação de tamanhos:

```
  arquivo        original    mini -6    gzip -6    mini -9    gzip -9
  vazio                 0         26         26         26         26
  um_byte               1         29         29         29         29
  texto            200000      56205      56215      55331      55357
  aleatorio        300000     300123     300123     300123     300123
  repetitivo       500000       1473       1475       1333       1334
  misto           1048576     261263     261242     260278     260316
  periodo          200000      34092     200091      34092     200091
```

O caso `periodo` é curioso: são 32 KiB aleatórios repetidos. O zlib (que está por trás do gzip) só usa distâncias até 32506, então não enxerga a repetição; este compressor usa a janela inteira de 32768 bytes e acha tudo.

## Como funciona

O gzip é uma casca fina (o contêiner) em volta do DEFLATE, que faz o trabalho pesado em duas etapas:

```
bytes originais
      |
      v
  LZ77  (src/deflate.c)        repetições viram pares (comprimento, distância)
      |
      v
  Huffman  (src/huffman.c)     símbolos frequentes ganham códigos curtos
      |
      v
  blocos DEFLATE               armazenado, Huffman fixo ou Huffman dinâmico
      |
      v
  contêiner gzip  (src/gzip.c) cabeçalho + blocos + CRC-32 + tamanho
```

### 1. LZ77: repetições viram referências

O compressor anda pelos dados olhando para os últimos 32 KiB (a "janela"). Quando os próximos bytes já apareceram antes, em vez de repeti-los ele emite um par **(comprimento, distância)**: "copie `comprimento` bytes começando `distância` bytes atrás". Comprimentos vão de 3 a 258 e distâncias de 1 a 32768.

```
entrada:  a b r a c a d a b r a _ a b r a c a d a b r a
saída:    a b r a c a d <4, 7> _ <11, 12>
```

Na posição 7, `abra` já apareceu 7 bytes antes (comprimento 4, distância 7). Na posição 12, a palavra inteira de 11 bytes está 12 bytes atrás.

A cópia pode se sobrepor ao que está sendo escrito. Depois de `abc`, o par `<9, 3>` gera `abcabcabc`, porque o descompactador copia byte a byte e vai lendo o que acabou de escrever. É assim que `aaaaaaaa` vira só `a <7, 1>`.

**Cadeias de hash.** Comparar com todas as 32768 posições anteriores seria lento demais. Então cada posição é indexada pelo hash dos 3 bytes que começam nela: `head[h]` guarda a posição mais recente com aquele hash e `prev[pos]` aponta para a anterior com o mesmo hash. Para achar candidatos, basta seguir a corrente, do mais recente para o mais antigo. No exemplo acima, ao chegar na posição 12:

```
head[hash("abr")] --> 7 --> 0 --> fim
```

O nível de compressão controla quantos candidatos visitar: 4 no `-1`, 128 no `-6`, 4096 no `-9`. A busca para cedo quando acha um match "bom o bastante" (o parâmetro `nice`).

**Avaliação preguiçosa** (níveis 4 a 9). Ao achar um match na posição p, o compressor ainda olha a posição p + 1 antes de decidir. Se lá houver um match mais longo, p vira literal:

```
já visto:     ... abc ... bcdefgh ...
agora:        abcdefgh
guloso:       <3: "abc">, depois procura de novo em "defgh"
preguiçoso:   'a' literal, depois <7: "bcdefgh">
```

### 2. Huffman e códigos canônicos

Depois do LZ77 sobra uma sequência de símbolos. O DEFLATE usa dois alfabetos:

- **literal/comprimento** (0 a 285): 0 a 255 são bytes literais, 256 é o fim do bloco e 257 a 285 são comprimentos, cada um com alguns bits extras;
- **distância** (0 a 29), também com bits extras (o código 29 cobre 24577 a 32768 com 13 bits extras).

```
símbolo  extras  comprimentos        código  extras  distâncias
257      0       3                   0       0       1
264      0       10                  4       1       5-6
265      1       11-12               10      4       33-48
284      5       227-257             28      13      16385-24576
285      0       258                 29      13      24577-32768
```

Um código de Huffman dá menos bits aos símbolos frequentes, e nenhum código é prefixo de outro, então dá para decodificar sem separadores. O truque do DEFLATE é que **só os comprimentos dos códigos viajam no arquivo**. Os códigos são reconstruídos pela regra canônica: ordene por (comprimento, símbolo) e numere em sequência, deslocando um bit para a esquerda sempre que o comprimento aumenta. Com o exemplo da RFC 1951:

```
símbolo  comprimento  código
F        2            00
A        3            010
B        3            011
C        3            100
D        3            101
E        3            110
G        4            1110
H        4            1111
```

Detalhe que confunde muita gente: os campos comuns são gravados a partir do bit menos significativo, mas os códigos de Huffman são gravados a partir do bit mais significativo. Por isso o compressor inverte os bits de cada código antes de escrever (`huff_reverse`) e a tabela do descompactador é indexada pelos bits já invertidos.

### 3. O limite de 15 bits e o package-merge

O DEFLATE não aceita códigos com mais de 15 bits (7 no código de comprimentos). O Huffman clássico passa desse limite quando as frequências variam muito: com frequências de Fibonacci (1, 1, 2, 3, 5, 8...) a árvore vira uma escada com profundidade n - 1. O zlib resolve com um ajuste heurístico; aqui foi usado o **package-merge** (Larmore e Hirschberg, 1990), que acha os comprimentos **ótimos** dentro do limite.

A ideia: cada símbolo vira uma "moeda" em cada nível de profundidade de 1 a L, e escolher comprimentos vira escolher moedas de custo mínimo. Na prática:

```
nível L (mais fundo):   as folhas, ordenadas por frequência
nível L-1:              folhas + pacotes (pares de itens do nível L, somados)
...
nível 1:                folhas + pacotes do nível 2
escolha:                os 2n - 2 itens mais baratos do nível 1
comprimento de um símbolo = quantas vezes ele aparece nos itens escolhidos
```

Os testes comparam o resultado com o Huffman clássico (quando o limite não pesa) e com uma busca por força bruta (quando pesa).

### 4. Os três tipos de bloco

Todo bloco começa com 3 bits: `BFINAL` (este é o último?) e `BTYPE`:

```
BTYPE 00  armazenado: alinha no byte, LEN (16 bits), NLEN = complemento de LEN, LEN bytes crus
BTYPE 01  Huffman fixo: códigos definidos pela RFC, nada de tabela no arquivo
          literais 0-143: 8 bits, 144-255: 9 bits, 256-279: 7 bits, 280-287: 8 bits
          distâncias: 5 bits
BTYPE 10  Huffman dinâmico: o bloco traz as próprias tabelas
BTYPE 11  reservado (erro)
```

A cada 16384 símbolos o compressor fecha um bloco, para que as tabelas se adaptem quando o conteúdo muda. Para cada bloco ele calcula o **custo exato em bits** das três opções e escolhe o menor. Dados aleatórios viram blocos armazenados (não há o que comprimir), textos curtos usam o código fixo (não vale pagar por uma tabela) e textos longos usam tabelas dinâmicas.

### 5. Blocos dinâmicos: tabelas que se descrevem

As tabelas também são comprimidas, com um terceiro código de Huffman (o "código de comprimentos"):

```
HLIT   5 bits   quantos códigos de literal/comprimento (257 a 286)
HDIST  5 bits   quantos códigos de distância (1 a 30)
HCLEN  4 bits   quantos comprimentos do código de comprimentos (4 a 19)
HCLEN x 3 bits  comprimentos, na ordem 16 17 18 0 8 7 9 6 10 5 11 4 12 3 13 2 14 1 15
HLIT + HDIST comprimentos, escritos com o código de comprimentos:
    0 a 15  comprimento literal
    16      repete o anterior de 3 a 6 vezes (2 bits extras)
    17      repete zero de 3 a 10 vezes (3 bits extras)
    18      repete zero de 11 a 138 vezes (7 bits extras)
os dados do bloco, até o símbolo 256
```

A ordem esquisita põe primeiro os comprimentos mais usados e por último os mais raros (1 e 15), para que os zeros do final possam ser cortados pelo HCLEN.

### 6. Decodificando rápido

Ler um código bit a bit funciona, mas é lento. O descompactador monta uma tabela de 512 entradas: olhando os próximos 9 bits, ela devolve direto o símbolo e o tamanho do código. Um código de L bits ocupa todas as entradas cujos L bits de baixo são o código invertido. Códigos maiores que 9 bits caem num caminho lento que usa a propriedade canônica: compara o valor lido com o maior código de cada comprimento.

Antes de montar a tabela, os comprimentos passam pela desigualdade de Kraft: começando com 1 vaga, cada nível dobra as vagas e cada código ocupa uma. Vagas negativas significam códigos sobrepostos (erro). Vagas sobrando significam código incompleto, aceito só no caso especial de um único código de 1 bit, como faz o zlib.

### 7. O contêiner gzip

```
+---+---+---+---+---+---+---+---+---+---+
|ID1|ID2|CM |FLG|     MTIME     |XFL|OS |   ID1 = 31, ID2 = 139, CM = 8 (deflate)
+---+---+---+---+---+---+---+---+---+---+
[FEXTRA: XLEN (2 bytes) e XLEN bytes]        se FLG tem o bit 2
[FNAME: nome original terminado em zero]     se FLG tem o bit 3
[FCOMMENT: comentário terminado em zero]     se FLG tem o bit 4
[FHCRC: 2 bytes do CRC-32 do cabeçalho]      se FLG tem o bit 1
... blocos DEFLATE ...
+---+---+---+---+---+---+---+---+
|     CRC32     |     ISIZE     |            CRC dos dados e tamanho módulo 2^32
+---+---+---+---+---+---+---+---+
```

Os números são little-endian. O bit 0 de FLG (FTEXT) é só um palpite de que o conteúdo é texto, e os bits 5 a 7 são reservados (precisam ser zero). Um arquivo pode ter vários **membros** concatenados (`cat a.gz b.gz > ab.gz`); descompactar devolve a concatenação dos conteúdos, e cada membro tem a sua própria janela: uma referência nunca alcança o membro anterior.

O CRC-32 é o resto da divisão dos dados (vistos como um polinômio sobre GF(2)) pelo polinômio `0x04C11DB7`. A versão rápida processa um byte por vez com uma tabela de 256 restos pré-calculados. Conferir CRC e ISIZE no final é o que garante que um byte trocado não passe despercebido.

### Mapa do código

```
include/mgz.h      API pública
src/bitio.c/.h     leitor e escritor de bits
src/huffman.c/.h   códigos canônicos, tabela de decodificação e package-merge
src/inflate.c/.h   descompactador: blocos armazenado, fixo e dinâmico
src/deflate.c/.h   compressor: LZ77, cadeias de hash e escolha de blocos
src/tables.c/.h    tabelas de comprimento e distância da RFC 1951
src/crc32.c/.h     CRC-32 (com tabela e bit a bit)
src/gzip.c         contêiner gzip e atalhos em memória
src/main.c         o programa mini-gzip
tests/             testes de unidade, gerador de dados e testes diferenciais
```

### Para experimentar

- Compare `mini-gzip --blocos` de um arquivo de texto com o de um arquivo aleatório (`head -c 100000 /dev/urandom`): um vira blocos dinâmicos, o outro blocos armazenados.
- Compacte o mesmo arquivo com `-1` e `-9` e veja o tamanho e o tempo mudarem.
- Mude `SYM_BUFSZ` em `src/deflate.c` (símbolos por bloco) e observe o efeito no tamanho final.

## Limitações

- O MTIME é gravado como 0: o C11 puro não tem como ler a data de modificação de um arquivo. Isso também deixa a saída reprodutível (o mesmo arquivo gera sempre os mesmos bytes).
- Ao descompactar, datas e permissões do original não são restauradas, e não há modo recursivo (`-r`) nem sufixo personalizado (`-S`).
- O objetivo é ensinar. Em produção, use o zlib, que é mais rápido e foi auditado ao longo de décadas.

## Contribuindo

Issues e pull requests são bem-vindos.

## Licença

MIT. Veja [LICENSE](LICENSE).
