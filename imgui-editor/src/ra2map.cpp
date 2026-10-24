#include "ra2map.h"

#include <algorithm>
#include <stdexcept>

#include "../3rdparty/minilzo/minilzo.h"

extern "C" {
// minilzo.c compiled separately; declarations it needs
}

namespace ra2 {

// ---------------------------------------------------------------- Ini

const std::string* IniSection::find(const std::string& key) const {
    for (auto& kv : entries)
        if (kv.first == key) return &kv.second;
    return nullptr;
}
std::string IniSection::get(const std::string& key, const std::string& fallback) const {
    const std::string* v = find(key);
    return v ? *v : fallback;
}
void IniSection::set(const std::string& key, const std::string& value) {
    for (auto& kv : entries)
        if (kv.first == key) { kv.second = value; return; }
    entries.emplace_back(key, value);
}

Ini Ini::parse(const std::string& text) {
    Ini ini;
    IniSection* cur = nullptr;
    size_t pos = 0;
    bool first = true;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = (eol == std::string::npos) ? text.size() + 1 : eol + 1;

        if (first) {
            first = false;
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
        }
        if (line.empty() || line[0] == ';') continue;

        if (line[0] == '[') {
            size_t close = line.find(']');
            if (close != std::string::npos)
                cur = &ini.sections[line.substr(1, close - 1)];
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || !cur) continue;
        cur->entries.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return ini;
}

std::string Ini::serialize() const {
    std::string out;
    for (auto& [name, sec] : sections) {
        out += "[" + name + "]\r\n";
        for (auto& [k, v] : sec.entries) out += k + "=" + v + "\r\n";
        out += "\r\n";
    }
    return out;
}

// ---------------------------------------------------------------- codecs

static int b64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<uint8_t> base64Decode(const std::string& text) {
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        int v = b64Val(c);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((acc >> bits) & 0xFF);
        }
    }
    return out;
}

// IsoMapPack5 payload: sequence of [u16 srcSize][u16 dstSize][LZO1X block]
static std::vector<uint8_t> lzoChunkedDecompress(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    size_t pos = 0;
    while (pos + 4 <= size) {
        uint16_t srcSize, dstSize;
        memcpy(&srcSize, data + pos, 2);
        memcpy(&dstSize, data + pos + 2, 2);
        pos += 4;
        if (pos + srcSize > size) throw std::runtime_error("isomappack: truncated chunk");
        size_t old = out.size();
        out.resize(old + dstSize);
        lzo_uint outLen = dstSize;
        int r = lzo1x_decompress_safe(data + pos, srcSize, out.data() + old, &outLen, nullptr);
        if (r != LZO_E_OK || outLen != dstSize)
            throw std::runtime_error("isomappack: lzo error");
        pos += srcSize;
    }
    return out;
}

// Westwood Format80 (LCW), following xcc's decode80c semantics:
//   0cccpppp p  short backward copy   10cccccc  literal run
//   11cccccc pp absolute copy         11111110 ccv  fill
//   11111111 cccc copy
static size_t lcwDecompressBlock(const uint8_t* r, uint8_t* w, uint8_t* wBase) {
    for (;;) {
        uint8_t code = *r++;
        if (!(code & 0x80)) {
            int count = (code >> 4) + 3;
            const uint8_t* m = w - (((code & 0x0F) << 8) | *r++);
            while (count--) *w++ = *m++;
        } else {
            int count = code & 0x3F;
            if (!(code & 0x40)) {
                if (!count) return w - wBase; // end marker
                while (count--) *w++ = *r++;
            } else if (count < 0x3E) {
                count += 3;
                uint16_t off;
                memcpy(&off, r, 2);
                r += 2;
                const uint8_t* m = wBase + off;
                while (count--) *w++ = *m++;
            } else if (count == 0x3E) {
                uint16_t cnt;
                memcpy(&cnt, r, 2);
                r += 2;
                uint8_t v = *r++;
                while (cnt--) *w++ = v;
            } else {
                uint16_t cnt, off;
                memcpy(&cnt, r, 2);
                memcpy(&off, r + 2, 2);
                r += 4;
                const uint8_t* m = wBase + off;
                while (cnt--) *w++ = *m++;
            }
        }
    }
}

std::vector<uint8_t> lcwDecompress(const uint8_t* src, size_t srcLen, size_t maxOut) {
    // section is chunked: [u16 sizeIn][u16 sizeOut][lcw block] ...
    std::vector<uint8_t> out;
    out.reserve(maxOut);
    size_t pos = 0;
    while (pos + 4 <= srcLen) {
        uint16_t sizeIn, sizeOut;
        memcpy(&sizeIn, src + pos, 2);
        memcpy(&sizeOut, src + pos + 2, 2);
        pos += 4;
        if (pos + sizeIn > srcLen) throw std::runtime_error("lcw: truncated chunk");
        size_t old = out.size();
        out.resize(old + sizeOut + 8);
        size_t used = lcwDecompressBlock(src + pos, out.data() + old, out.data());
        out.resize(old + sizeOut);
        pos += sizeIn;
    }
    return out;
}

// ---------------------------------------------------------------- Map

static std::string joinSection(const IniSection& sec) {
    std::string s;
    for (auto& [k, v] : sec.entries) s += v;
    return s;
}

static std::vector<int> parseInts(const std::string& s) {
    std::vector<int> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string part = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? s.size() : comma + 1;
        try { out.push_back(std::stoi(part)); } catch (...) { out.push_back(0); }
    }
    return out;
}

bool Map::loadFromData(const std::string& data, std::string* error) {
    if (data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00) {
        if (error) *error = "MIX 容器（.mmx/.yro）暂不支持，请先用原版编辑器另存为 .map/.mpr/.yrm";
        return false;
    }

    ini = Ini::parse(data);

    auto itMap = ini.sections.find("Map");
    if (itMap == ini.sections.end()) {
        if (error) *error = "缺少 [Map] 节，不是有效的地图文件";
        return false;
    }
    theater = itMap->second.get("Theater");
    auto sizeVals = parseInts(itMap->second.get("Size", "0,0,0,0"));
    for (int i = 0; i < 4 && i < (int)sizeVals.size(); i++) size[i] = sizeVals[i];

    auto itBasic = ini.sections.find("Basic");
    name = itBasic != ini.sections.end() ? itBasic->second.get("Name") : "";

    // IsoMapPack5: numbered keys 1..N concatenated, base64, chunked LZO
    auto itIso = ini.sections.find("IsoMapPack5");
    cells.clear();
    if (itIso != ini.sections.end()) {
        try {
            std::vector<uint8_t> packed = base64Decode(joinSection(itIso->second));
            std::vector<uint8_t> raw = lzoChunkedDecompress(packed.data(), packed.size());
            size_t i = 0;
            while (i + 11 <= raw.size()) {
                IsoCell c;
                memcpy(&c.x, raw.data() + i, 2);
                memcpy(&c.y, raw.data() + i + 2, 2);
                memcpy(&c.tile, raw.data() + i + 4, 4);
                c.subtile = raw[i + 8];
                c.height = raw[i + 9];
                c.ice = raw[i + 10];
                if (c.x == 0 && c.y == 0) break; // terminator
                cells.push_back(c);
                i += 11;
            }
        } catch (const std::exception& e) {
            if (error) *error = std::string("IsoMapPack5 解包失败: ") + e.what();
            return false;
        }
    }

    // Waypoints: value = ry * 1000 + rx
    waypoints.clear();
    auto itWp = ini.sections.find("Waypoints");
    if (itWp != ini.sections.end()) {
        for (auto& [k, v] : itWp->second.entries) {
            if (v.empty()) continue;
            int val = 0;
            try { val = std::stoi(v); } catch (...) { continue; }
            Waypoint w;
            try { w.id = std::stoi(k); } catch (...) { w.id = -1; }
            w.rx = val % 1000;
            w.ry = val / 1000;
            waypoints.push_back(w);
        }
        std::sort(waypoints.begin(), waypoints.end(), [](const Waypoint& a, const Waypoint& b) { return a.id < b.id; });
    }

    auto itHouses = ini.sections.find("Houses");
    houseCount = itHouses == ini.sections.end() ? 0 : (int)itHouses->second.entries.size();

    auto itOv = ini.sections.find("OverlayPack");
    overlayBytes = 0;
    if (itOv != ini.sections.end()) {
        try {
            std::vector<uint8_t> packed = base64Decode(joinSection(itOv->second));
            std::vector<uint8_t> raw = lcwDecompress(packed.data(), packed.size(), 512 * 512);
            overlayBytes = (int)raw.size();
        } catch (const std::exception&) {
            overlayBytes = -1; // present but undecodable
        }
    }

    return true;
}

} // namespace ra2
