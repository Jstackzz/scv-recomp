// Checks inserted into the game's code ([[midasm_hook]] entries named Sc5Guard* in config/sc5_codegen.toml): where
// its own code reads through a pointer that can be empty, and the console's memory map let that pass while here the
// read lands on unmapped guest memory. Each one makes the read harmless the way the game's own code would treat the
// value, and the same on every PC (netplay compares the games' state).

#include <rex/ppc/context.h>

// sub_82480C08, a battle object's state save: r4 = its owner (+0x264) + 0x5BE90, the 4 bytes it writes next. With no
// owner the restore skips that field (sub_82480C90) but still reads its 4 bytes, so the save must write 4: read them
// from the zero page (all zeros, sc5_app.cpp maps it readable), at 0x100 and not 0, which the stream's write takes for
// "nothing to write" and skips, leaving every later read of the snapshot 4 bytes off (the KO replay's restore then
// crashed in ever different places).
void Sc5GuardOwnerField(PPCRegister& r4) {
  if (r4.u32 < 0x00100000) r4.u64 = 0x100;
}
