# Developer conveniences on top of CMake (the build system) and scripts/ (the gates).
#
#   make            configure + build the default tree (build/, Debug)
#   make test       run the unit tests and regress binaries in build/
#   make format     move trailing comments above their code, then clang-format every
#                   tracked C++ source in place (.clang-format)
#   make format-check
#                   report formatting drift and trailing comments without touching
#                   files (the CI gate: scripts/format_check.sh)
#   make tidy       clang-tidy over the sources (scripts/tidy.sh)
#
# Variables: BUILD_DIR (build), BUILD_TYPE (Debug), JOBS (half the cores, the
# machine's build limit), CLANG_FORMAT (clang-format).

BUILD_DIR    ?= build
BUILD_TYPE   ?= Debug
JOBS         ?= $(shell n=$$(nproc 2>/dev/null || echo 2); echo $$(( n / 2 > 0 ? n / 2 : 1 )))
CLANG_FORMAT ?= clang-format

# Tracked C++ sources outside third_party/; generated .inc files are exempt.
FORMAT_FILES = $(shell git ls-files '*.cpp' '*.hpp' ':!:third_party/**' ':!:tests/*.inc')

.PHONY: all configure build test format format-check tidy clean help

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) -j $(JOBS)

test: build
	ctest --test-dir $(BUILD_DIR) -j $(JOBS) --output-on-failure

format:
	@command -v $(CLANG_FORMAT) >/dev/null || { echo "make format: $(CLANG_FORMAT) not found (pip install clang-format, or set CLANG_FORMAT=)"; exit 1; }
	python3 scripts/trailing_comments.py --fix $(FORMAT_FILES)
	$(CLANG_FORMAT) -i $(FORMAT_FILES)
	@echo "format: reformatted $(words $(FORMAT_FILES)) files"

format-check:
	CLANG_FORMAT=$(CLANG_FORMAT) scripts/format_check.sh

tidy:
	scripts/tidy.sh $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

help:
	@sed -n '1,14p' Makefile | sed 's/^# \{0,1\}//'
