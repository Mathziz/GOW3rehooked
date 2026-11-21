// SPDX-License-Identifier: GPL-2.0-or-later
// gow3_update.exe in release ZIPs. v0.1.5 and v0.1.6 update through a separate gow3_update.exe and refuse a release without
// one; the runtime now updates itself (main/update.cpp) and deletes this stand-in when it starts. Run by an older runtime,
// it reports no update, and the game starts.
#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    return 0;
}
