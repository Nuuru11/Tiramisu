// tiramisu/src/guest/guest_data.c
// Separate translation unit so the pointer table survives optimization:
// clang cannot fold the reads (no LTO), so absolute fixups must exist in
// .rdata and the base-relocation pass has real DIR64 work.
typedef unsigned int u32;

u32 some_data[4] = {1, 2, 3, 4};
u32* const g_ptr_table[2] = {some_data, &some_data[2]};
