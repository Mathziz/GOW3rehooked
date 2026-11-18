// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <ranges>

#include "orbis/common/assert.h"
#include "orbis/common/io_file.h"
#include "orbis/logging/log.h"
#include "orbis/core/file_format/psf.h"

static const std::unordered_map<std::string_view, u32> psf_known_max_sizes = {
    {"ACCOUNT_ID", 8},  {"CATEGORY", 4},  {"DETAIL", 1024},       {"FORMAT", 4},
    {"MAINTITLE", 128}, {"PARAMS", 1024}, {"SAVEDATA_BLOCKS", 8}, {"SAVEDATA_DIRECTORY", 32},
    {"SUBTITLE", 128},  {"TITLE_ID", 12},
};
static inline u32 get_max_size(std::string_view key, u32 default_value) {
    if (const auto& v = psf_known_max_sizes.find(key); v != psf_known_max_sizes.end()) {
        return v->second;
    }
    return default_value;
}

bool PSF::Open(const std::filesystem::path& filepath) {
    using namespace std::chrono;
    if (std::filesystem::exists(filepath)) {
        const auto t = std::filesystem::last_write_time(filepath);
        const auto rel =
            duration_cast<seconds>(t - std::filesystem::file_time_type::clock::now()).count();
        const auto tp = system_clock::to_time_t(system_clock::now() + seconds{rel});
        last_write = system_clock::from_time_t(tp);
    }

    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        return false;
    }

    const u64 psfSize = file.GetSize();
    std::vector<u8> psf(psfSize);
    file.Seek(0);
    if (file.Read(psf) != psf.size()) return false;
    return Open(psf);
}

bool PSF::Open(const std::unique_ptr<Core::FileSys::IFile>& file) {
    std::vector<u8> psf_buf(file->Size());
    if (file->Read(psf_buf.data(), psf_buf.size()) != static_cast<s64>(psf_buf.size())) {
        return false;
    }
    return Open(psf_buf);
}

bool PSF::Open(const std::vector<u8>& psf_buffer) {
    entry_list.clear();
    map_binaries.clear();
    map_strings.clear();
    map_integers.clear();

    if (psf_buffer.size() < sizeof(PSFHeader)) return false;
    const u8* psf_data = psf_buffer.data();
    PSFHeader header{};
    std::memcpy(&header, psf_data, sizeof(header));

    if (header.magic != PSF_MAGIC) {
        LOG_ERROR(Core, "Invalid PSF magic number");
        return false;
    }
    if (header.version != PSF_VERSION_1_1 && header.version != PSF_VERSION_1_0) {
        LOG_ERROR(Core, "Unsupported PSF version: 0x{:08x}", header.version);
        return false;
    }

    const size_t key_table = header.key_table_offset;
    const size_t data_table = header.data_table_offset;
    const size_t count = header.index_table_entries;
    if (key_table < sizeof(header) || data_table < key_table || data_table > psf_buffer.size() ||
        count > (key_table - sizeof(header)) / sizeof(PSFRawEntry)) return false;

    for (size_t i = 0; i < count; ++i) {
        PSFRawEntry raw_entry{};
        std::memcpy(&raw_entry, psf_data + sizeof(PSFHeader) + i * sizeof(PSFRawEntry),
                    sizeof(raw_entry));

        const size_t key_offset = raw_entry.key_offset;
        const size_t data_offset = raw_entry.data_offset;
        const size_t length = raw_entry.param_len;
        const size_t data_size = psf_buffer.size() - data_table;
        if (key_offset >= data_table - key_table || data_offset > data_size || length > data_size - data_offset)
            return false;
        const char* key = reinterpret_cast<const char*>(psf_data + key_table + key_offset);
        const auto* key_end = static_cast<const char*>(std::memchr(key, 0, data_table - key_table - key_offset));
        if (!key_end) return false;

        Entry& entry = entry_list.emplace_back();
        entry.key.assign(key, key_end);
        entry.param_fmt = static_cast<PSFEntryFmt>(raw_entry.param_fmt.Raw());
        entry.max_len = raw_entry.param_max_len;

        const u8* data = psf_data + data_table + data_offset;

        switch (entry.param_fmt) {
        case PSFEntryFmt::Binary: {
            map_binaries.emplace(i, std::vector<u8>(data, data + length));
        } break;
        case PSFEntryFmt::Text: {
            const char* value = reinterpret_cast<const char*>(data);
            const auto* end = static_cast<const char*>(std::memchr(value, 0, length));
            if (!end) return false;
            map_strings.emplace(i, std::string(value, end));
        } break;
        case PSFEntryFmt::Integer: {
            if (length != sizeof(s32)) return false;
            s32 integer;
            std::memcpy(&integer, data, sizeof(integer));
            map_integers.emplace(i, integer);
        } break;
        default:
            return false;
        }
    }
    return true;
}

bool PSF::Encode(const std::filesystem::path& filepath) const {
    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) {
        return false;
    }

    last_write = std::chrono::system_clock::now();

    const auto psf_buffer = Encode();
    const size_t written = file.Write(psf_buffer);
    if (written != psf_buffer.size()) {
        LOG_ERROR(Core, "Failed to write PSF file. Written {} Expected {}", written,
                  psf_buffer.size());
    }
    file.Close();
    return written == psf_buffer.size();
}

std::vector<u8> PSF::Encode() const {
    std::vector<u8> psf_buffer;
    Encode(psf_buffer);
    return psf_buffer;
}

void PSF::Encode(std::vector<u8>& psf_buffer) const {
    psf_buffer.resize(sizeof(PSFHeader) + sizeof(PSFRawEntry) * entry_list.size());

    {
        auto& header = *(PSFHeader*)psf_buffer.data();
        header.magic = PSF_MAGIC;
        header.version = PSF_VERSION_1_1;
        header.index_table_entries = entry_list.size();
    }

    const size_t key_table_offset = psf_buffer.size();
    ((PSFHeader*)psf_buffer.data())->key_table_offset = key_table_offset;
    for (size_t i = 0; i < entry_list.size(); i++) {
        auto& raw_entry = ((PSFRawEntry*)(psf_buffer.data() + sizeof(PSFHeader)))[i];
        const Entry& entry = entry_list[i];
        raw_entry.key_offset = psf_buffer.size() - key_table_offset;
        raw_entry.param_fmt.FromRaw(static_cast<u16>(entry.param_fmt));
        raw_entry.param_max_len = entry.max_len;
        std::ranges::copy(entry.key, std::back_inserter(psf_buffer));
        psf_buffer.push_back(0); // NULL terminator
    }

    const size_t data_table_offset = psf_buffer.size();
    ((PSFHeader*)psf_buffer.data())->data_table_offset = data_table_offset;
    for (size_t i = 0; i < entry_list.size(); i++) {
        if (psf_buffer.size() % 4 != 0) {
            std::ranges::fill_n(std::back_inserter(psf_buffer), 4 - psf_buffer.size() % 4, 0);
        }
        auto& raw_entry = ((PSFRawEntry*)(psf_buffer.data() + sizeof(PSFHeader)))[i];
        const Entry& entry = entry_list[i];
        raw_entry.data_offset = psf_buffer.size() - data_table_offset;

        s32 additional_padding = s32(raw_entry.param_max_len);

        switch (entry.param_fmt) {
        case PSFEntryFmt::Binary: {
            const auto& value = map_binaries.at(i);
            raw_entry.param_len = value.size();
            additional_padding -= s32(raw_entry.param_len);
            std::ranges::copy(value, std::back_inserter(psf_buffer));
        } break;
        case PSFEntryFmt::Text: {
            const auto& value = map_strings.at(i);
            raw_entry.param_len = value.size() + 1;
            additional_padding -= s32(raw_entry.param_len);
            std::ranges::copy(value, std::back_inserter(psf_buffer));
            psf_buffer.push_back(0); // NULL terminator
        } break;
        case PSFEntryFmt::Integer: {
            const auto& value = map_integers.at(i);
            raw_entry.param_len = sizeof(s32);
            additional_padding -= s32(raw_entry.param_len);
            const auto value_bytes = reinterpret_cast<const u8*>(&value);
            std::ranges::copy(value_bytes, value_bytes + sizeof(s32),
                              std::back_inserter(psf_buffer));
        } break;
        default:
            UNREACHABLE_MSG("Unknown PSF entry format");
        }
        ASSERT_MSG(additional_padding >= 0, "PSF entry max size mismatch");
        std::ranges::fill_n(std::back_inserter(psf_buffer), additional_padding, 0);
    }
}

std::optional<std::span<const u8>> PSF::GetBinary(std::string_view key) const {
    const auto& [it, index] = FindEntry(key);
    if (it == entry_list.end() || it->param_fmt != PSFEntryFmt::Binary) {
        return {};
    }
    return std::span{map_binaries.at(index)};
}

std::optional<std::string_view> PSF::GetString(std::string_view key) const {
    const auto& [it, index] = FindEntry(key);
    if (it == entry_list.end() || it->param_fmt != PSFEntryFmt::Text) {
        return {};
    }
    return std::string_view{map_strings.at(index)};
}

std::optional<s32> PSF::GetInteger(std::string_view key) const {
    const auto& [it, index] = FindEntry(key);
    if (it == entry_list.end() || it->param_fmt != PSFEntryFmt::Integer) {
        return {};
    }
    return map_integers.at(index);
}

void PSF::AddBinary(std::string key, std::vector<u8> value, bool update) {
    auto [it, index] = FindEntry(key);
    bool exist = it != entry_list.end();
    if (exist && !update) {
        LOG_ERROR(Core, "PSF: Tried to add binary key that already exists: {}", key);
        return;
    }
    if (exist) {
        ASSERT_MSG(it->param_fmt == PSFEntryFmt::Binary, "PSF: Change format is not supported");
        it->max_len = get_max_size(key, value.size());
        map_binaries.at(index) = std::move(value);
        return;
    }
    Entry& entry = entry_list.emplace_back();
    entry.max_len = get_max_size(key, value.size());
    entry.key = std::move(key);
    entry.param_fmt = PSFEntryFmt::Binary;
    map_binaries.emplace(entry_list.size() - 1, std::move(value));
}

void PSF::AddBinary(std::string key, uint64_t value, bool update) {
    std::vector<u8> data(8);
    std::memcpy(data.data(), &value, 8);
    return AddBinary(std::move(key), std::move(data), update);
}

void PSF::AddString(std::string key, std::string value, bool update) {
    auto [it, index] = FindEntry(key);
    bool exist = it != entry_list.end();
    if (exist && !update) {
        LOG_ERROR(Core, "PSF: Tried to add string key that already exists: {}", key);
        return;
    }
    if (exist) {
        ASSERT_MSG(it->param_fmt == PSFEntryFmt::Text, "PSF: Change format is not supported");
        it->max_len = get_max_size(key, value.size() + 1);
        map_strings.at(index) = std::move(value);
        return;
    }
    Entry& entry = entry_list.emplace_back();
    entry.max_len = get_max_size(key, value.size() + 1);
    entry.key = std::move(key);
    entry.param_fmt = PSFEntryFmt::Text;
    map_strings.emplace(entry_list.size() - 1, std::move(value));
}

void PSF::AddInteger(std::string key, s32 value, bool update) {
    auto [it, index] = FindEntry(key);
    bool exist = it != entry_list.end();
    if (exist && !update) {
        LOG_ERROR(Core, "PSF: Tried to add integer key that already exists: {}", key);
        return;
    }
    if (exist) {
        ASSERT_MSG(it->param_fmt == PSFEntryFmt::Integer, "PSF: Change format is not supported");
        it->max_len = sizeof(s32);
        map_integers.at(index) = value;
        return;
    }
    Entry& entry = entry_list.emplace_back();
    entry.key = std::move(key);
    entry.param_fmt = PSFEntryFmt::Integer;
    entry.max_len = sizeof(s32);
    map_integers.emplace(entry_list.size() - 1, value);
}

std::pair<std::vector<PSF::Entry>::iterator, size_t> PSF::FindEntry(std::string_view key) {
    auto entry =
        std::ranges::find_if(entry_list, [&](const auto& entry) { return entry.key == key; });
    return {entry, std::distance(entry_list.begin(), entry)};
}

std::pair<std::vector<PSF::Entry>::const_iterator, size_t> PSF::FindEntry(
    std::string_view key) const {
    auto entry =
        std::ranges::find_if(entry_list, [&](const auto& entry) { return entry.key == key; });
    return {entry, std::distance(entry_list.begin(), entry)};
}
