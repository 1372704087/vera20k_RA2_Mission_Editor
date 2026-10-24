// RA2 map file model: INI text + packed binary sections (IsoMapPack5,
// OverlayPack, OverlayDataPack), matching the formats verified against the
// MFC editor and the vera20k engine reader.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ra2 {

struct IniSection {
    // insertion-ordered key/value pairs; duplicate keys preserved
    std::vector<std::pair<std::string, std::string>> entries;
    const std::string* find(const std::string& key) const;
    std::string get(const std::string& key, const std::string& fallback = "") const;
    void set(const std::string& key, const std::string& value);
};

class Ini {
public:
    std::map<std::string, IniSection> sections;
    static Ini parse(const std::string& text);
    std::string serialize() const; // CRLF, matches the editor's output
};

struct IsoCell {
    int16_t x;      // diamond axis 1
    uint16_t y;     // diamond axis 2
    int32_t tile;
    uint8_t subtile;
    uint8_t height;
    uint8_t ice;
};

struct Waypoint {
    int id;
    int rx, ry;     // value = ry * 1000 + rx
};

struct Map {
    Ini ini;
    std::string path;
    std::string name;
    std::string theater;
    int size[4] = {0, 0, 0, 0};          // [Map] Size
    std::vector<IsoCell> cells;          // unpacked IsoMapPack5
    std::vector<Waypoint> waypoints;     // sorted by id
    int houseCount = 0;                  // [Houses] entries
    int overlayBytes = 0;                // unpacked OverlayPack size

    std::string error;

    // Parses a raw .map/.mpr/.yrm (bare INI). MIX containers are rejected
    // for now with a clear message.
    bool loadFromData(const std::string& data, std::string* error);
};

// --- codecs (standard public algorithms) ---
std::vector<uint8_t> base64Decode(const std::string& text);
// LZO1X decompress, throws std::runtime_error on malformed input
void lzo1xDecompress(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen);
// Westwood LCW (Format80) decompress; maxOut caps the output buffer
std::vector<uint8_t> lcwDecompress(const uint8_t* src, size_t srcLen, size_t maxOut);

} // namespace ra2
