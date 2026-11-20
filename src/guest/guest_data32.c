// tiramisu/src/guest/guest_data32.c
// M7 Phase 1 probe guest data (32-bit): a pointer table whose entries are
// absolute 32-bit addresses, so the image has HIGHLOW base relocations
// (the gate test verifies them byte-wise against the file's .reloc).
typedef unsigned int u32;
u32 some_data32[4] = {1, 2, 3, 4};
u32* const g_ptr32_table[2] = {some_data32, &some_data32[2]};
