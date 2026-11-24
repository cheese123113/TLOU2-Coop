#pragma once

#include <windows.h>

namespace boot {
	bool InitCore(HMODULE gameModule, HMODULE selfModule);
}

extern bool s_debugMenuOpen;
extern int  g_forcePlayerSkel;
