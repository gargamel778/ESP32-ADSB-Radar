// The firmware embeds its data files with the linker (board_build.embed_files). The host tests do the same with .incbin, under the same symbol names.
#ifndef DATA_DIR
#error "compile with -DDATA_DIR=\"/path/to/firmware/data\""
#endif
#ifdef __APPLE__
#define SECT ".section __DATA,__const\n"
#define SYM(n) "_binary_data_" n
#else
#define SECT ".section .rodata\n"
#define SYM(n) "_binary_data_" n
#endif
#define EMBED(file, name) \
  __asm__(SECT ".balign 4\n.globl " SYM(name "_start") "\n" SYM(name "_start") ":\n.incbin \"" DATA_DIR "/" file "\"\n.globl " SYM(name "_end") "\n" SYM(name "_end") ":\n.byte 0\n.text\n")
EMBED("geo.bin", "geo_bin");
EMBED("world.bin", "world_bin");
EMBED("airlines.bin", "airlines_bin");
EMBED("mapdata.bin", "mapdata_bin");
EMBED("flags.bin", "flags_bin");
