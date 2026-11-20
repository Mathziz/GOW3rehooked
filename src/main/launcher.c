// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

__declspec(dllimport) int Gow3Main(void);

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    return Gow3Main();
}
