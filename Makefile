CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
OFS := native/filesystems/ofs
CORE := $(OFS)/core/ofs_core.c $(OFS)/core/ofs_primitives.c
READER := $(OFS)/efi/reader.c
MODULE := $(OFS)/module/ofs_module.c
INC := -I$(OFS)/core -I$(OFS)/efi -I$(OFS)/module
OUT ?= build

.PHONY: all test clean efi
all: $(OUT)/OFS.so
$(OUT):
	mkdir -p "$(OUT)"

$(OUT)/OFS.so: $(CORE) $(READER) $(MODULE) | $(OUT)
	$(CC) $(CFLAGS) -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fvisibility=hidden -nostdlib -shared -Wl,-z,defs -Wl,-z,relro -Wl,-z,now $(INC) $(MODULE) $(READER) $(CORE) -o "$@"

$(OUT)/ofs-module-test: tests/ofs_dropin_module_test.c $(MODULE) $(READER) $(CORE) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -o "$@"

$(OUT)/ofs-reader-test: tests/ofs_efi_reader_test.c $(READER) $(CORE) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -o "$@"

$(OUT)/ofs-core-test: tests/ofs_core_test.c $(CORE) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -o "$@"

$(OUT)/ofs-primitives-test: tests/ofs_primitives_test.c $(OFS)/core/ofs_primitives.c | $(OUT)
	$(CC) $(CFLAGS) $(INC) $^ -o "$@"

test: $(OUT)/OFS.so $(OUT)/ofs-module-test $(OUT)/ofs-reader-test $(OUT)/ofs-core-test $(OUT)/ofs-primitives-test
	$(OUT)/ofs-module-test
	$(OUT)/ofs-reader-test
	$(OUT)/ofs-core-test
	$(OUT)/ofs-primitives-test
	readelf -Ws $(OUT)/OFS.so | grep -F 'infiltrator_filesystem_module_v1'
	readelf -Ws $(OUT)/OFS.so | grep -F 'infiltrator_filesystem_module_v2'
	readelf -p .ifsmeta $(OUT)/OFS.so | grep -F 'IFS-MODULE:ofs:1'
	readelf -p .ifsmeta $(OUT)/OFS.so | grep -F 'IFS-MODULE:ofs:2'
	! readelf -d $(OUT)/OFS.so | grep -q '[(]NEEDED[)]'

efi:
	bash scripts/build-ofs-efi.sh "$(abspath $(OUT))/uefi"

clean:
	rm -rf -- "$(OUT)"
