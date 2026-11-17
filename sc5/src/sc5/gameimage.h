// The recompiled image (generated/default/sc5_init.cpp): its layout and function table. In a player's game it is in
// sc5game.dll, the part built on their PC from their disc, and this sc5.exe comes ready made with the package
// (SC5_PLAYER_HOST, sc5/player/README.md); a developer's build links it into sc5.exe. Host code takes it from here,
// never from PPCImageConfig or PPCFuncMappings, which the ready-made sc5.exe cannot link.

#pragma once

#include <rex/image_info.h>
#include <rex/ppc/func.h>

#if defined(SC5_PLAYER_HOST)
extern "C" __declspec(dllimport) const rex::PPCImageInfo* Sc5GameImage();
namespace sc5 {
inline const rex::PPCImageInfo* GameImage() { return Sc5GameImage(); }
}  // namespace sc5
#else
extern const rex::PPCImageInfo PPCImageConfig;
namespace sc5 {
inline const rex::PPCImageInfo* GameImage() { return &PPCImageConfig; }
}  // namespace sc5
#endif
