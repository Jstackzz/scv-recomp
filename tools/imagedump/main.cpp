// imagedump: writes the game's executable image exactly as codegen and the runtime see it (the disc's
// default.xex with a sibling default.xexp applied, decrypted and decompressed), for disassembly and for
// finding the addresses the codegen config and the netplay hooks need.
//
//   imagedump <game dir with default.xex [+ default.xexp]> <out.bin>
//
// The file starts at the image's base address (printed), so file offset = address - base.

#include <cstdio>
#include <fstream>

#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: imagedump <game dir> <out.bin>\n");
    return 2;
  }
  rex::Runtime runtime(argv[1]);
  auto status = runtime.Setup(rex::RuntimeConfig{
      .kernel_init = rex::kernel::InitializeKernel,
      .tool_mode = true,
  });
  if (status) {
    std::fprintf(stderr, "runtime setup failed: %#x\n", static_cast<unsigned>(status));
    return 1;
  }
  status = runtime.LoadXexImage("game:\\default.xex");
  if (status) {
    std::fprintf(stderr, "loading game:\\default.xex failed: %#x\n", static_cast<unsigned>(status));
    return 1;
  }
  auto module = runtime.kernel_state()->GetExecutableModule();
  auto* xex = module->xex_module();
  const uint32_t base = xex->base_address();
  const uint32_t size = xex->image_size();
  const uint8_t* image = runtime.memory()->TranslateVirtual(base);
  std::ofstream out(argv[2], std::ios::binary);
  out.write(reinterpret_cast<const char*>(image), size);
  if (!out) {
    std::fprintf(stderr, "writing %s failed\n", argv[2]);
    return 1;
  }
  std::printf("base %08X size %08X entry %08X -> %s\n", base, size, xex->entry_point(), argv[2]);
  return 0;
}
