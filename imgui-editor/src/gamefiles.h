// Westwood MIX archive access for RA2/YR: plain + protected (Blowfish/RSA) mixes,
// top-level mixes kept on disk (index-only) with lazy reads, nested sub-mixes in memory.
// Port of MissionEditorWeb src/game/{mix,mixkeys,blowfish,crc32,gamefiles}.ts
// (those were ported from xcc and verified against real game data).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ra2 {

class MixFile {
public:
    struct Entry { uint32_t id; int64_t offset; int32_t size; };
    std::vector<Entry> entries;
    int64_t dataOffset = 0; // absolute offset of data area

    // data: first headerLen bytes of the mix; fullSize: total file size on disk.
    bool parse(const uint8_t* data, size_t dataLen, uint64_t fullSize, std::string* err = nullptr);

    bool has(uint32_t id) const;
    const Entry* find(uint32_t id) const;

private:
    bool parsePlainAt(const uint8_t* data, size_t dataLen, size_t prefix);
    bool tryEncrypted(const uint8_t* data, size_t dataLen, uint64_t fullSize);
};

// PKZIP crc32
uint32_t crc32(const uint8_t* data, size_t len);
// MIX file id for a name (xcc get_id: uppercase, / -> \, padded to 4-byte multiple)
uint32_t mixFileId(const char* name);

// A top-level mix that stays on disk; only its index is in memory.
class DiskMix {
public:
    ~DiskMix();
    bool open(const std::wstring& path, std::string* err = nullptr);
    const std::string& name() const { return name_; }
    bool has(const char* fname) const { return idx_.has(mixFileId(fname)); }
    // reads an entry fully into out (const: file position is internal state)
    bool read(const char* fname, std::vector<uint8_t>& out) const;

private:
    FILE* f_ = nullptr;
    uint64_t fullSize_ = 0;
    MixFile idx_;
    std::string name_;
};

// In-memory sub-mix (e.g. isosnow.mix nested in ra2md.mix)
class MemMix {
public:
    std::vector<uint8_t> data;
    MixFile idx;
    bool parse(std::string* err = nullptr) { return idx.parse(data.data(), data.size(), data.size(), err); }
    bool has(const char* fname) const { return idx.has(mixFileId(fname)); }
    bool read(const char* fname, const uint8_t** out, size_t* outSize) const;
};

class GameAssets {
public:
    // Scan dir for *.mix (ra2/ra2md first, then the rest), open known sub-mixes.
    // looseDirFiles: also index loose files in the dir.
    bool open(const std::wstring& dir, std::string* err = nullptr);

    // Returns a copy of the file contents; tries loose files, disk mixes, sub-mixes.
    bool find(const char* name, std::vector<uint8_t>& out) const;
    bool hasAny(const char* name) const;

    int mixCount() const { return (int)(diskMixes_.size() + memMixes_.size()); }
    const std::wstring& dir() const { return dir_; }

private:
    void openSubMixes();
    std::wstring dir_;
    std::vector<DiskMix*> diskMixes_;
    std::vector<MemMix> memMixes_;
};

} // namespace ra2
