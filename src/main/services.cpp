// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "orbis/internals/libs.h"
#include "orbis/internals/kernel/kernel.h"
#include "orbis/internals/sysmodule/sysmodule.h"
#include "orbis/internals/system/userservice.h"
#include "orbis/internals/system/systemservice.h"
#include "orbis/internals/system/commondialog.h"
#include "orbis/internals/system/msgdialog.h"
#include "orbis/internals/pad/pad.h"
#include "orbis/internals/audio/audioout.h"
#include "orbis/internals/ajm/ajm.h"
#include "orbis/internals/playgo/playgo.h"
#include "orbis/internals/save_data/savedata.h"
#include "orbis/internals/save_data/dialog/savedatadialog.h"
#include "orbis/internals/app_content/app_content.h"
#include "orbis/internals/gnmdriver/gnmdriver.h"
#include "orbis/internals/videoout/video_out.h"

void LinkSymbolImpl(Core::Loader::SymbolsResolver* sym, const char* nid, const char* lib,
                    u16 version, const char* mod, u64 address, Core::Loader::SymbolType type) {
    sym->AddSymbol({nid, {}, lib, version, mod, type}, address);
}
namespace Libraries {
namespace Videodec { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
void InitHLELibs(Core::Loader::SymbolsResolver* sym) {
    Kernel::RegisterLib(sym);
    SysModule::RegisterLib(sym);
    UserService::RegisterLib(sym);
    SystemService::RegisterLib(sym);
    CommonDialog::RegisterLib(sym);
    MsgDialog::RegisterLib(sym);
    Pad::RegisterLib(sym);
    AudioOut::RegisterLib(sym);
    Ajm::RegisterLib(sym);
    Videodec::RegisterLib(sym);
    PlayGo::RegisterLib(sym);
    SaveData::RegisterLib(sym);
    SaveData::Dialog::RegisterLib(sym);
    AppContent::RegisterLib(sym);
    GnmDriver::RegisterLib(sym);
    VideoOut::RegisterLib(sym);
}
}
