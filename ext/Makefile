# Builds Oyl, the C library, from the sources vendored in oyl/ (shards
# doesn't fetch git submodules) into oyl/build/liboyl.a, with the flags
# Oyl's own Makefile uses. The shard's postinstall is build.cr, which does
# the same and also builds with MSVC on Windows.
#
#   make -C ext                          build on Unix
#   make -C ext sync OYL=path/to/oyl     vendor another version: the
#                                        checkout's committed HEAD,
#        ... sync OYL=path/to/oyl REF=v1.3.0   or a release tag

CC ?= cc
AR ?= ar
CFLAGS ?= -O2
REF ?= HEAD

SRCS := $(wildcard oyl/src/*.c)
OBJS := $(SRCS:oyl/src/%.c=oyl/build/%.o)

all: oyl/build/liboyl.a

oyl/build/liboyl.a: $(OBJS)
	$(AR) rcs $@ $^

oyl/build/%.o: oyl/src/%.c $(wildcard oyl/src/*.h) oyl/include/oyl/oyl.h
	@mkdir -p oyl/build
	$(CC) -std=c11 $(CFLAGS) -Ioyl/include -fvisibility=hidden -c $< -o $@

sync:
	@test -n "$(OYL)" || { echo "usage: make sync OYL=path/to/oyl" >&2; exit 1; }
	rm -rf oyl
	mkdir -p oyl
	git -C "$(OYL)" archive "$(REF)" src include LICENSE | tar -x -C oyl
	git -C "$(OYL)" log -1 --format='%H %cs %s' "$(REF)" > oyl/COMMIT
	@echo "vendored: $$(cat oyl/COMMIT)"

clean:
	rm -rf oyl/build

.PHONY: all sync clean
