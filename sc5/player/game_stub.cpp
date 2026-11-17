// Part of sc5game.dll, compiled with the translated code on the player's PC (README.md here).

#include <rex/image_info.h>

// A DLL with no C runtime of its own: the compiler names this symbol wherever a program uses floating point, and the
// runtime that would define it is sc5.exe's.
extern "C" const int _fltused = 0;

extern const rex::PPCImageInfo PPCImageConfig;

// what sc5.exe starts the game with (src/sc5/gameimage.h): the image's layout and its function table
extern "C" const rex::PPCImageInfo* Sc5GameImage() { return &PPCImageConfig; }
