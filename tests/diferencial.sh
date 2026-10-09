#!/bin/sh
# Testes diferenciais: o mini-gzip contra o gzip do sistema.
# Uso: sh tests/diferencial.sh <caminho do mini-gzip> <caminho do gerador>
set -u

MGZ=$1
GER=$2

if ! command -v gzip >/dev/null 2>&1; then
    echo "gzip do sistema não encontrado: testes diferenciais ignorados"
    exit 0
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/mini-gzip-teste.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

pass=0
fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "  FALHOU: $*"; }

# Arquivos de teste (conteúdo determinístico).
"$GER" zeros 0 "$TMP/vazio"
"$GER" texto 1 "$TMP/um_byte"
"$GER" texto 200000 "$TMP/texto"
"$GER" aleatorio 300000 "$TMP/aleatorio"
"$GER" repetitivo 500000 "$TMP/repetitivo"
"$GER" misto 1048576 "$TMP/misto"
"$GER" periodo 200000 "$TMP/periodo"
FILES="vazio um_byte texto aleatorio repetitivo misto periodo"

echo "- nosso compressor, gzip -d do sistema"
for f in $FILES; do
    for n in 1 6 9; do
        if ! "$MGZ" -c -$n "$TMP/$f" > "$TMP/$f.m$n.gz"; then
            bad "mini-gzip -$n $f"
            continue
        fi
        if gzip -d -c "$TMP/$f.m$n.gz" | cmp -s - "$TMP/$f"; then ok; else bad "gzip -d de $f.m$n.gz"; fi
        if gzip -t "$TMP/$f.m$n.gz" 2>/dev/null; then ok; else bad "gzip -t de $f.m$n.gz"; fi
    done
done

echo "- gzip -1 e -9 do sistema, nosso descompactador"
for f in $FILES; do
    for n in 1 9; do
        gzip -c -$n "$TMP/$f" > "$TMP/$f.g$n.gz"
        if "$MGZ" -d -c "$TMP/$f.g$n.gz" | cmp -s - "$TMP/$f"; then ok; else bad "mini-gzip -d de $f.g$n.gz"; fi
        if "$MGZ" -t "$TMP/$f.g$n.gz"; then ok; else bad "mini-gzip -t de $f.g$n.gz"; fi
    done
done

echo "- tamanhos em bytes (mini-gzip e gzip do sistema)"
printf "  %-12s %10s %10s %10s %10s %10s\n" arquivo original "mini -6" "gzip -6" "mini -9" "gzip -9"
for f in $FILES; do
    gzip -c -6 "$TMP/$f" > "$TMP/$f.g6.gz"
    printf "  %-12s %10s %10s %10s %10s %10s\n" "$f" \
        "$(wc -c < "$TMP/$f" | tr -d ' ')" \
        "$(wc -c < "$TMP/$f.m6.gz" | tr -d ' ')" "$(wc -c < "$TMP/$f.g6.gz" | tr -d ' ')" \
        "$(wc -c < "$TMP/$f.m9.gz" | tr -d ' ')" "$(wc -c < "$TMP/$f.g9.gz" | tr -d ' ')"
done

echo "- arquivos corrompidos"
# Byte trocado no CRC-32 do trailer.
cp "$TMP/texto.m6.gz" "$TMP/crc.gz"
"$GER" inverter "$TMP/crc.gz" -8
msg=$("$MGZ" -t "$TMP/crc.gz" 2>&1)
if [ $? -ne 0 ] && echo "$msg" | grep -q "CRC-32 não confere"; then ok; else bad "CRC trocado não detectado: $msg"; fi
# Byte trocado no meio dos dados de um bloco armazenado (dados aleatórios).
cp "$TMP/aleatorio.m6.gz" "$TMP/meio.gz"
if "$MGZ" --blocos "$TMP/meio.gz" | grep -q "^ *1  armazenado"; then ok; else bad "o primeiro bloco de dados aleatórios devia ser armazenado"; fi
"$GER" inverter "$TMP/meio.gz" 5000
msg=$("$MGZ" -t "$TMP/meio.gz" 2>&1)
if [ $? -ne 0 ] && echo "$msg" | grep -q "CRC-32 não confere"; then ok; else bad "byte trocado nos dados não detectado: $msg"; fi
# Byte trocado em dados comprimidos: qualquer erro serve, mas tem que falhar.
cp "$TMP/texto.m9.gz" "$TMP/comp.gz"
"$GER" inverter "$TMP/comp.gz" 3000
if "$MGZ" -t "$TMP/comp.gz" 2>/dev/null; then bad "corrupção em dados comprimidos passou"; else ok; fi
# A descompactação com erro não deixa arquivo pela metade.
cp "$TMP/comp.gz" "$TMP/parcial.gz"
"$MGZ" -d "$TMP/parcial.gz" 2>/dev/null
if [ $? -eq 1 ] && [ ! -e "$TMP/parcial" ] && [ -e "$TMP/parcial.gz" ]; then ok; else bad "saída parcial não foi removida"; fi
# Arquivos truncados.
size=$(wc -c < "$TMP/texto.m6.gz" | tr -d ' ')
for keep in 5 10 $((size / 2)) $((size - 4)) $((size - 1)); do
    head -c "$keep" "$TMP/texto.m6.gz" > "$TMP/trunc.gz"
    msg=$("$MGZ" -t "$TMP/trunc.gz" 2>&1)
    if [ $? -ne 0 ] && echo "$msg" | grep -q "truncados"; then ok; else bad "truncado em $keep bytes: $msg"; fi
done
# Não é gzip.
msg=$("$MGZ" -t "$TMP/texto" 2>&1)
if [ $? -ne 0 ] && echo "$msg" | grep -q "não é um arquivo gzip"; then ok; else bad "arquivo comum aceito como gzip: $msg"; fi

echo "- membros concatenados"
cat "$TMP/texto.m6.gz" "$TMP/aleatorio.g9.gz" "$TMP/vazio.m1.gz" "$TMP/periodo.g1.gz" > "$TMP/cat.gz"
cat "$TMP/texto" "$TMP/aleatorio" "$TMP/vazio" "$TMP/periodo" > "$TMP/cat.esperado"
if "$MGZ" -d -c "$TMP/cat.gz" | cmp -s - "$TMP/cat.esperado"; then ok; else bad "membros concatenados"; fi
if gzip -d -c "$TMP/cat.gz" | cmp -s - "$TMP/cat.esperado"; then ok; else bad "gzip do sistema e os membros concatenados"; fi
if "$MGZ" --blocos "$TMP/cat.gz" | grep -q "total: 4 membros"; then ok; else bad "--blocos não contou 4 membros"; fi
printf 'lixo' >> "$TMP/cat.gz"
"$MGZ" -d -c "$TMP/cat.gz" 2>/dev/null | cmp -s - "$TMP/cat.esperado"
if [ $? -eq 0 ]; then ok; else bad "dados com lixo no final"; fi
"$MGZ" -t "$TMP/cat.gz" 2>/dev/null
if [ $? -eq 2 ]; then ok; else bad "lixo no final devia dar código 2 (aviso)"; fi

echo "- opções da linha de comando"
cp "$TMP/texto" "$TMP/arq.txt"
"$MGZ" "$TMP/arq.txt"
if [ ! -e "$TMP/arq.txt" ] && [ -e "$TMP/arq.txt.gz" ]; then ok; else bad "compactar devia trocar arq.txt por arq.txt.gz"; fi
if gzip -t "$TMP/arq.txt.gz"; then ok; else bad "gzip -t do arquivo criado"; fi
"$MGZ" -d "$TMP/arq.txt.gz"
if [ -e "$TMP/arq.txt" ] && [ ! -e "$TMP/arq.txt.gz" ] && cmp -s "$TMP/arq.txt" "$TMP/texto"; then ok; else bad "-d devia restaurar arq.txt"; fi
"$MGZ" -k -9 "$TMP/arq.txt"
if [ -e "$TMP/arq.txt" ] && [ -e "$TMP/arq.txt.gz" ]; then ok; else bad "-k devia manter o original"; fi
"$MGZ" -k "$TMP/arq.txt" 2>/dev/null
if [ $? -eq 1 ]; then ok; else bad "não devia sobrescrever sem -f"; fi
if "$MGZ" -kf "$TMP/arq.txt"; then ok; else bad "-f devia sobrescrever"; fi
out=$("$MGZ" -l "$TMP/arq.txt.gz")
if echo "$out" | grep -q " 200000 .*arq.txt$"; then ok; else bad "-l: $out"; fi
# O nome gravado no cabeçalho é lido pelo gzip do sistema (-N).
mkdir "$TMP/nome"
cp "$TMP/arq.txt.gz" "$TMP/nome/outro.gz"
(cd "$TMP/nome" && gzip -d -N outro.gz 2>/dev/null)
if [ -e "$TMP/nome/arq.txt" ] && cmp -s "$TMP/nome/arq.txt" "$TMP/texto"; then ok; else bad "gzip -N não achou o nome original"; fi
# Entrada e saída padrão.
if "$MGZ" < "$TMP/texto" | gzip -d -c | cmp -s - "$TMP/texto"; then ok; else bad "compactar da entrada padrão"; fi
if "$MGZ" -d < "$TMP/texto.g9.gz" | cmp -s - "$TMP/texto"; then ok; else bad "descompactar da entrada padrão"; fi
cat "$TMP/texto" "$TMP/periodo" > "$TMP/texto.periodo"
if "$MGZ" -dc "$TMP/texto.m6.gz" "$TMP/periodo.m9.gz" | cmp -s - "$TMP/texto.periodo"; then ok; else bad "-dc com dois arquivos"; fi
"$MGZ" -d "$TMP/texto" 2>/dev/null
if [ $? -eq 2 ]; then ok; else bad "sufixo desconhecido devia dar aviso"; fi
"$MGZ" -x "$TMP/texto" 2>/dev/null
if [ $? -eq 1 ]; then ok; else bad "opção inválida devia dar erro"; fi
if "$MGZ" --help | grep -q "uso: mini-gzip"; then ok; else bad "--help"; fi

echo "$pass verificações diferenciais, $fail falhas"
[ "$fail" -eq 0 ]
