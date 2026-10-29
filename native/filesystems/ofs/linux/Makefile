# One canonical engine, compiled natively for Linux until the generic SO bridge.
obj-m += ofs.o
ofs-y := vfs.o engine_volume.o engine_nodes.o engine_files.o engine_namespace.o \
         engine_check.o engine_strings.o engine_cache.o engine_primitives.o
ccflags-y += -I$(src)
