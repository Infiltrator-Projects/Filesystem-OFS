CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
OFS := native/filesystems/ofs
CORE := $(OFS)/core/ofs_core.c $(OFS)/core/ofs_primitives.c
ENGINE := $(addprefix $(OFS)/core/,ofs_volume.c ofs_nodes.c ofs_files.c ofs_namespace.c ofs_check.c ofs_strings.c ofs_cache.c) $(OFS)/defrag/ofs_relayout.c $(OFS)/defrag/ofs_defrag_module.c
READER := $(OFS)/efi/reader.c
MODULE := $(OFS)/module/ofs_module.c $(OFS)/module/ofs_v3.c $(OFS)/module/ofs_boot.c $(OFS)/module/ofs_memory.c
INC := -I$(OFS)/core -I$(OFS)/efi -I$(OFS)/module -Inative/include
OUT ?= build
PYTHON ?= python3
HEADERS := $(wildcard $(OFS)/core/*.h $(OFS)/module/*.h $(OFS)/efi/*.h $(OFS)/defrag/*.h native/include/infiltratr/fs/*.h)

.PHONY: all test check interop release native clean efi kernel
all: $(OUT)/OFS.so
$(OUT):
	mkdir -p "$(OUT)"

$(OUT)/OFS.so: $(CORE) $(ENGINE) $(MODULE) $(HEADERS) Makefile standalone/build.py | $(OUT)
	CC="$(CC)" $(PYTHON) standalone/build.py --profile portable --output "$@"

$(OUT)/ofs-module-test: tests/ofs_dropin_module_test.c $(OUT)/OFS.so $(HEADERS) Makefile | $(OUT)
	$(CC) $(CFLAGS) $(INC) $< $(OUT)/OFS.so -Wl,-rpath,'$$ORIGIN' -o "$@"

$(OUT)/ofs-reader-test: tests/ofs_efi_reader_test.c $(READER) $(CORE) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(filter %.c,$^) -o "$@"

$(OUT)/ofs-core-test: tests/ofs_core_test.c $(CORE) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(filter %.c,$^) -o "$@"

$(OUT)/ofs-primitives-test: tests/ofs_primitives_test.c $(OFS)/core/ofs_primitives.c | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(filter %.c,$^) -o "$@"

$(OUT)/ofs-orphan-test: tests/ofs_orphan_test.c $(filter-out $(OFS)/defrag/%,$(ENGINE)) $(OFS)/core/ofs_primitives.c $(HEADERS) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(filter %.c,$^) -o "$@"

test: $(OUT)/OFS.so $(OUT)/ofs-module-test $(OUT)/ofs-reader-test $(OUT)/ofs-core-test $(OUT)/ofs-primitives-test $(OUT)/ofs-orphan-test
	$(OUT)/ofs-module-test
	$(OUT)/ofs-reader-test
	$(OUT)/ofs-core-test
	$(OUT)/ofs-primitives-test
	$(OUT)/ofs-orphan-test
	readelf -Ws $(OUT)/OFS.so | grep -F 'infiltrator_filesystem_module_v1'
	readelf -Ws $(OUT)/OFS.so | grep -F 'infiltrator_filesystem_module_v2'
	readelf -p .ifsmeta $(OUT)/OFS.so | grep -F 'IFS-MODULE:ofs:1'
	readelf -p .ifsmeta $(OUT)/OFS.so | grep -F 'IFS-MODULE:ofs:2'
	! readelf -d $(OUT)/OFS.so | grep -q '[(]NEEDED[)]'

check: test
	$(PYTHON) standalone/build.py --output $(OUT)/OFS.so --inspect-only
	$(PYTHON) tests/ofs_complete_test.py --module $(OUT)/OFS.so

interop: $(OUT)/OFS.so
	$(PYTHON) tests/ofs_v3_test.py --module $(OUT)/OFS.so --oracle "$(AMITOOLS_SOURCE)"

native:
	$(PYTHON) standalone/build.py --profile aggressive --output build-native/OFS.so --check

release:
	$(PYTHON) standalone/release.py release

efi:
	bash scripts/build-ofs-efi.sh "$(abspath $(OUT))/uefi"

kernel:
	mkdir -p "$(OUT)/kernel"
	bash scripts/build-ofs-kernel.sh "$(abspath $(OUT))/kernel" "$(KERNEL_BUILD)"

clean:
	rm -rf -- "$(OUT)"
