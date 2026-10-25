#include "theater.h"

#include <cstring>

namespace ra2 {

// ---------------------------------------------------------------- tmp

static int32_t ti32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

// Expand the packed diamond (row widths 4..cx..4) into a full cx*cy canvas.
static void unpackDiamond(const uint8_t* packed, size_t& r, int cx, int cy, uint8_t* out) {
    memset(out, 0, (size_t)cx * cy);
    size_t wLine = 0;
    int lineLen = 0;
    int lineStart = cx / 2;
    for (int y = 0; y < cy / 2; y++) {
        lineLen += 4;
        lineStart -= 2;
        for (int i = 0; i < lineLen; i++) out[wLine + lineStart + i] = packed[r++];
        wLine += cx;
    }
    for (int y = cy / 2; y < cy; y++) {
        lineLen -= 4;
        lineStart += 2;
        for (int i = 0; i < lineLen; i++) out[wLine + lineStart + i] = packed[r++];
        wLine += cx;
    }
}

bool parseTmp(const uint8_t* data, size_t len, TmpFile& out, std::string* err) {
    if (len < 16) { if (err) *err = "tmp too small"; return false; }
    out.cblocksX = ti32(data);
    out.cblocksY = ti32(data + 4);
    out.cx = ti32(data + 8);
    out.cy = ti32(data + 12);
    if (out.cblocksX <= 0 || out.cblocksY <= 0 || out.cblocksX > 64 || out.cblocksY > 64 ||
        (out.cx != 48 && out.cx != 60) || out.cy * 2 != out.cx) {
        if (err) *err = "invalid TMP header";
        return false;
    }
    size_t cTiles = (size_t)out.cblocksX * out.cblocksY;
    if (len < 16 + cTiles * 4) { if (err) *err = "tmp index truncated"; return false; }
    std::vector<int32_t> index(cTiles);
    for (size_t i = 0; i < cTiles; i++) index[i] = ti32(data + 16 + i * 4);

    out.tiles.clear();
    out.tiles.resize(cTiles);
    std::vector<uint8_t> packed((size_t)out.cx * out.cy / 2);
    for (size_t i = 0; i < cTiles; i++) {
        TmpTile& t = out.tiles[i];
        if (index[i] == 0 || (size_t)index[i] + 52 > len) continue;
        const uint8_t* b = data + index[i];
        t.x = ti32(b);
        t.y = ti32(b + 4);
        int32_t extraOfs = ti32(b + 8);
        // b+12: z offset; b+16: extra z offset
        t.xExtra = ti32(b + 20);
        t.yExtra = ti32(b + 24);
        t.cxExtra = ti32(b + 28);
        t.cyExtra = ti32(b + 32);
        uint32_t flags;
        memcpy(&flags, b + 36, 4);
        t.hasExtra = (flags & 1) != 0;
        t.height = b[40];
        t.terrainType = b[41];
        t.rampType = b[42];
        // b+43..48: radar colors
        t.present = true;
        size_t packedLen = (size_t)out.cx * out.cy / 2;
        if ((size_t)index[i] + 52 + packedLen > len) { t.present = false; continue; }
        size_t r = (size_t)index[i] + 52;
        t.pixels.resize((size_t)out.cx * out.cy);
        unpackDiamond(data, r, out.cx, out.cy, t.pixels.data());
        if (t.hasExtra && t.cxExtra > 0 && t.cyExtra > 0) {
            size_t start = (size_t)index[i] + extraOfs;
            size_t sz = (size_t)t.cxExtra * t.cyExtra;
            if (start + sz <= len) t.extra.assign(data + start, data + start + sz);
        }
    }
    return true;
}

// ---------------------------------------------------------------- theater ini

static const char* isoPalName(const std::string& t) {
    if (t == "TEMPERATE") return "isotem.pal";
    if (t == "SNOW") return "isosno.pal";
    if (t == "URBAN") return "isourb.pal";
    if (t == "NEWURBAN") return "isoubn.pal";
    if (t == "LUNAR") return "isolun.pal";
    if (t == "DESERT") return "isodes.pal";
    return "";
}

static const char* theaterIni(const std::string& t) {
    if (t == "TEMPERATE") return "Temperat.ini";
    if (t == "SNOW") return "Snow.ini";
    if (t == "URBAN") return "Urban.ini";
    if (t == "NEWURBAN") return "UrbanNMD.ini";
    if (t == "LUNAR") return "LunarMD.ini";
    if (t == "DESERT") return "DesertMD.ini";
    return "";
}

static const char* theaterIniMd(const std::string& t) {
    if (t == "TEMPERATE") return "TemperatMD.ini";
    if (t == "SNOW") return "SnowMD.ini";
    if (t == "URBAN") return "UrbanMD.ini";
    return nullptr;
}

TheaterData* TheaterData::load(GameAssets* assets, const std::string& theaterName, bool yuriMode,
                               std::string* err) {
    if (theaterName.empty()) { if (err) *err = "map has no theater"; return nullptr; }

    Ini ini;
    bool loadedAny = false;
    const char* base = theaterIni(theaterName);
    if (base) {
        std::vector<uint8_t> data;
        if (assets->find(base, data)) {
            ini.parse(std::string((const char*)data.data(), data.size()));
            loadedAny = true;
        }
    }
    if (const char* md = theaterIniMd(theaterName)) {
        if (yuriMode) {
            std::vector<uint8_t> data;
            if (assets->find(md, data)) {
                Ini mdIni;
                mdIni.parse(std::string((const char*)data.data(), data.size()));
                for (auto& [name, sec] : mdIni.sections) {
                    IniSection& dst = ini.sections[name];
                    for (auto& kv : sec.entries) dst.set(kv.first, kv.second);
                }
                loadedAny = true;
            }
        }
    }
    if (!loadedAny) {
        if (err) *err = "theater INI not found: " + std::string(base ? base : "?");
        return nullptr;
    }

    TheaterData* td = new TheaterData();
    td->assets_ = assets;
    td->theater_ = theaterName;
    if (theaterName == "TEMPERATE") td->suffix_ = ".tem";
    else if (theaterName == "SNOW") td->suffix_ = ".sno";
    else if (theaterName == "URBAN") td->suffix_ = ".urb";
    else if (theaterName == "NEWURBAN") td->suffix_ = ".ubn";
    else if (theaterName == "LUNAR") td->suffix_ = ".lun";
    else if (theaterName == "DESERT") td->suffix_ = ".des";

    // palette: 768 bytes, 6-bit channels expanded
    std::vector<uint8_t> pal;
    if (assets->find(isoPalName(theaterName), pal) && pal.size() >= 768) {
        for (int i = 0; i < 768; i++) td->isoPal_[i] = (uint8_t)((pal[i] << 2) | (pal[i] >> 4));
    }

    // expand tilesets
    char secName[32];
    for (int i = 0; i < 10000; i++) {
        snprintf(secName, sizeof(secName), "TileSet%04d", i);
        auto it = ini.sections.find(secName);
        if (it == ini.sections.end()) break;
        const IniSection& sec = it->second;
        int tileCount = atoi(sec.get("TilesInSet", "0").c_str());
        if (tileCount <= 0) continue;
        TileSetInfo set;
        set.id = i;
        set.setName = sec.get("SetName", "");
        set.fileName = sec.get("FileName", "");
        set.tileCount = tileCount;
        set.startIndex = (int)td->tiles_.size();
        td->tileSets_.push_back(set);
        for (int e = 0; e < tileCount; e++) td->tiles_.push_back({(int)td->tileSets_.size() - 1, e + 1});
    }
    if (td->tiles_.empty()) {
        delete td;
        if (err) *err = "theater INI has no tilesets";
        return nullptr;
    }
    return td;
}

std::vector<std::string> TheaterData::tmpNames(int globalId) const {
    std::vector<std::string> names;
    if (globalId < 0 || globalId >= (int)tiles_.size()) return names;
    const TileEntry& e = tiles_[globalId];
    const TileSetInfo& set = tileSets_[e.setIdx];
    if (set.fileName.empty()) return names;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%02d", set.fileName.c_str(), e.indexInSet);
    std::string base = buf;
    names.push_back(base + suffix_);
    if (theater_ != "TEMPERATE") names.push_back(base + ".tem");
    if (theater_ == "NEWURBAN") names.push_back(base + ".urb");
    return names;
}

TmpFile* TheaterData::loadTmp(int globalId) {
    if (tmpMissing_.count(globalId)) return nullptr;
    auto it = tmpCache_.find(globalId);
    if (it != tmpCache_.end()) return &it->second;

    TmpFile* result = nullptr;
    for (const std::string& name : tmpNames(globalId)) {
        std::vector<uint8_t> data;
        if (!assets_->find(name.c_str(), data)) continue;
        TmpFile tmp;
        std::string err;
        if (parseTmp(data.data(), data.size(), tmp, &err)) {
            auto [ins, ok] = tmpCache_.emplace(globalId, std::move(tmp));
            result = &ins->second;
            break;
        }
    }
    if (!result) tmpMissing_[globalId] = true;
    return result;
}

const TileSetInfo* TheaterData::tileSetOf(int globalId) const {
    if (globalId < 0 || globalId >= (int)tiles_.size()) return nullptr;
    return &tileSets_[tiles_[globalId].setIdx];
}

std::string TheaterData::tileSetName(int globalId) const {
    const TileSetInfo* s = tileSetOf(globalId);
    return s ? s->setName : "";
}

} // namespace ra2
