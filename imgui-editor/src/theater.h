// Theater (tile set) loading for RA2/YR — port of MissionEditorWeb src/game/{theater,pal,tmp}.ts
// Maps global tile ids (as stored in IsoMapPack5 cell records) to TMP graphics.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gamefiles.h"
#include "ra2map.h"

namespace ra2 {

struct TmpTile {
    bool present = false;
    int32_t x = 0, y = 0;         // position within the tile block (pixels)
    uint8_t height = 0;           // relative ground height (half-cell units)
    uint8_t terrainType = 0, rampType = 0;
    bool hasExtra = false;
    int32_t xExtra = 0, yExtra = 0, cxExtra = 0, cyExtra = 0;
    std::vector<uint8_t> pixels;  // cx*cy palette indices, 0 = transparent
    std::vector<uint8_t> extra;   // cxExtra*cyExtra, 0 = transparent
};

struct TmpFile {
    int cblocksX = 0, cblocksY = 0, cx = 60, cy = 30;
    std::vector<TmpTile> tiles;
};

bool parseTmp(const uint8_t* data, size_t len, TmpFile& out, std::string* err);

struct TileSetInfo {
    int id = 0;
    std::string setName, fileName;
    int startIndex = 0, tileCount = 0;
};

class TheaterData {
public:
    // theaterName: TEMPERATE/SNOW/URBAN/NEWURBAN/LUNAR/DESERT (as stored in the map)
    static TheaterData* load(GameAssets* assets, const std::string& theaterName, bool yuriMode,
                             std::string* err);

    const std::string& theater() const { return theater_; }
    const std::string& suffix() const { return suffix_; } // ".tem" etc

    // TMP names candidates for a global tile id (FA2 fallback order)
    std::vector<std::string> tmpNames(int globalId) const;

    // Load + cache; null when unavailable
    TmpFile* loadTmp(int globalId);

    const uint8_t* isoPalette() const { return isoPal_; }
    int tileCount() const { return (int)tiles_.size(); }
    const TileSetInfo* tileSetOf(int globalId) const;
    // set display name for a global tile id (for the status bar)
    std::string tileSetName(int globalId) const;

private:
    std::string theater_, suffix_;
    uint8_t isoPal_[768] = {};
    std::vector<TileSetInfo> tileSets_;
    struct TileEntry {
        int setIdx;        // index into tileSets_
        int indexInSet;    // 1-based
    };
    std::vector<TileEntry> tiles_;
    std::map<int, TmpFile> tmpCache_;     // globalId -> parsed (or empty marker via missing)
    std::map<int, bool> tmpMissing_;      // globalId -> true when load failed
    GameAssets* assets_ = nullptr;
};

} // namespace ra2
