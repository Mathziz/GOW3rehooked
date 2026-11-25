// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include "orbis/common/types.h"

namespace Libraries::AudioOut {

struct PortOut;

class PortBackend {
public:
    virtual ~PortBackend() = default;

    /// Guaranteed to be called in intervals of at least port buffer time,
    /// with size equal to port buffer size.
    virtual void Output(void* ptr) = 0;

    virtual void SetVolume(const std::array<int, 8>& ch_volumes) = 0;

    /// The output device's speakers, which the game mixes for: 2, 6 (5.1) or 8 (7.1).
    virtual u8 OutputChannels() const = 0;
};

class AudioOutBackend {
public:
    AudioOutBackend() = default;
    virtual ~AudioOutBackend() = default;

    virtual std::unique_ptr<PortBackend> Open(PortOut& port) = 0;
};

class SDLAudioOut final : public AudioOutBackend {
public:
    std::unique_ptr<PortBackend> Open(PortOut& port) override;
};

} // namespace Libraries::AudioOut
