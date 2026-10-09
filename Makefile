# Makefile do Descompactador Gzip.
# Variáveis que podem ser trocadas: CC, CFLAGS, LDFLAGS, BUILD.
#   make                       compila build/mini-gzip e build/libmgz.a
#   make test                  testes de unidade + diferenciais contra o gzip
#   make sanitize              os mesmos testes com AddressSanitizer e UBSan
CFLAGS ?= -O2
LDFLAGS ?=
BUILD ?= build
ALL_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Iinclude -Isrc $(CFLAGS)

LIB_SRC = src/bitio.c src/crc32.c src/deflate.c src/gzip.c src/huffman.c src/inflate.c src/tables.c
LIB_OBJ = $(LIB_SRC:src/%.c=$(BUILD)/%.o)
HEADERS = $(wildcard include/*.h src/*.h)

all: $(BUILD)/mini-gzip $(BUILD)/libmgz.a

$(BUILD)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(BUILD)/libmgz.a: $(LIB_OBJ)
	$(AR) rcs $@ $(LIB_OBJ)

$(BUILD)/mini-gzip: $(BUILD)/main.o $(BUILD)/libmgz.a
	$(CC) $(CFLAGS) $(BUILD)/main.o $(BUILD)/libmgz.a $(LDFLAGS) -o $@

$(BUILD)/test_unidade: tests/test_unidade.c tests/teste.h $(BUILD)/libmgz.a $(HEADERS)
	$(CC) $(ALL_CFLAGS) tests/test_unidade.c $(BUILD)/libmgz.a $(LDFLAGS) -o $@

$(BUILD)/gerador: tests/gerador.c
	@mkdir -p $(BUILD)
	$(CC) $(ALL_CFLAGS) tests/gerador.c $(LDFLAGS) -o $@

test: $(BUILD)/test_unidade $(BUILD)/mini-gzip $(BUILD)/gerador
	./$(BUILD)/test_unidade
	sh tests/diferencial.sh ./$(BUILD)/mini-gzip ./$(BUILD)/gerador

sanitize:
	$(MAKE) BUILD=$(BUILD)/sanitize CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" test

clean:
	rm -rf $(BUILD)

.PHONY: all test sanitize clean
