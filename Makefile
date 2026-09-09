# Developer conveniences on top of CMake (the build system), scripts/ (the gates)
# and packaging/ (the release artifacts).
#
#   make / make debug   configure + build the Debug tree (build/)
#   make release        configure + build the Release tree (build-rel/)
#   make test           unit tests + regress binaries of the Debug tree (ctest)
#   make package        release artifacts into packaging/dist/: binary tarball and
#                       .deb (and .rpm when rpmbuild is installed); staged from a
#                       Release build in build-pkg/ via the CMake install rules
#   make tarball | deb | rpm
#                       one artifact
#   make format         move trailing comments above their code, then clang-format
#                       every tracked C++ source in place (.clang-format)
#   make format-check   report formatting drift and trailing comments without
#                       touching files (the CI gate: scripts/format_check.sh)
#   make tidy           clang-tidy over the sources (scripts/tidy.sh)
#   make clean          remove the build trees (build/, build-rel/, build-pkg/)
#   make distclean      clean plus the packaging/dist/ artifacts
#
# Variables: DEBUG_DIR (build), RELEASE_DIR (build-rel), PKG_DIR (build-pkg), JOBS
# (half the cores, the machine's build limit), CLANG_FORMAT (clang-format).
# `make BUILD_DIR=x BUILD_TYPE=RelWithDebInfo build` drives an arbitrary tree.

DEBUG_DIR    ?= build
RELEASE_DIR  ?= build-rel
PKG_DIR      ?= build-pkg
BUILD_DIR    ?= $(DEBUG_DIR)
BUILD_TYPE   ?= Debug
JOBS         ?= $(shell n=$$(nproc 2>/dev/null || echo 2); echo $$(( n / 2 > 0 ? n / 2 : 1 )))
CLANG_FORMAT ?= clang-format

# Tracked C++ sources outside third_party/; generated .inc files are exempt.
FORMAT_FILES = $(shell git ls-files '*.cpp' '*.hpp' ':!:third_party/**' ':!:tests/*.inc')

.PHONY: all debug release configure build test package tarball deb rpm \
        format format-check tidy clean distclean help

all: debug

# ---- build trees ----
debug:
	cmake -S . -B $(DEBUG_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(DEBUG_DIR) -j $(JOBS)

release:
	cmake -S . -B $(RELEASE_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(RELEASE_DIR) -j $(JOBS)

# An arbitrary tree: BUILD_DIR + BUILD_TYPE.
configure:
	cmake -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) -j $(JOBS)

test: debug
	ctest --test-dir $(DEBUG_DIR) -j $(JOBS) --output-on-failure

# ---- packaging (plan doc 10 §4.5): the scripts build Release in PKG_DIR and stage
# through `cmake --install`, so every format ships the same file set ----
package: tarball deb
	@if command -v rpmbuild >/dev/null; then $(MAKE) rpm; else echo "package: rpmbuild not installed, .rpm skipped"; fi
	@echo "package: artifacts in packaging/dist/"; ls -1 packaging/dist

tarball:
	LNFS_PKG_BUILD_DIR=$(abspath $(PKG_DIR)) LNFS_JOBS=$(JOBS) packaging/make_tarball.sh

deb:
	LNFS_PKG_BUILD_DIR=$(abspath $(PKG_DIR)) LNFS_JOBS=$(JOBS) packaging/make_deb.sh

rpm:
	LNFS_PKG_BUILD_DIR=$(abspath $(PKG_DIR)) LNFS_JOBS=$(JOBS) packaging/make_rpm.sh

# ---- source gates ----
format:
	@command -v $(CLANG_FORMAT) >/dev/null || { echo "make format: $(CLANG_FORMAT) not found (pip install clang-format, or set CLANG_FORMAT=)"; exit 1; }
	python3 scripts/trailing_comments.py --fix $(FORMAT_FILES)
	$(CLANG_FORMAT) -i $(FORMAT_FILES)
	@echo "format: reformatted $(words $(FORMAT_FILES)) files"

format-check:
	CLANG_FORMAT=$(CLANG_FORMAT) scripts/format_check.sh

tidy:
	scripts/tidy.sh $(DEBUG_DIR)

# ---- cleaning ----
clean:
	rm -rf $(DEBUG_DIR) $(RELEASE_DIR) $(PKG_DIR)

distclean: clean
	rm -rf packaging/dist

help:
	@sed -n '1,22p' Makefile | sed 's/^# \{0,1\}//'
