// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "orbis/core/loader/symbols_resolver.h"
namespace Core {
struct SysModules {
    std::string_view module_name;
    void (*callback)(Loader::SymbolsResolver*);
};
}
