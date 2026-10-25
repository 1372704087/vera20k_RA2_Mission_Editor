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

static const char* B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | data[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += B64[(acc >> bits) & 0x3F];
        }
    }
    if (bits) out += B64[(acc << (6 - bits)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

std::vector<uint8_t> lzo1xCompress(const uint8_t* src, size_t len) {
    static bool inited = false;
    if (!inited) {
        if (lzo_init() != LZO_E_OK) throw std::runtime_error("lzo_init failed");
        inited = true;
    }
    std::vector<uint8_t> wrk(LZO1X_1_MEM_COMPRESS);
    std::vector<uint8_t> out(len + len / 16 + 64 + 3);
    lzo_uint outLen = out.size();
    int r = lzo1x_1_compress(src, len, out.data(), &outLen, wrk.data());
    if (r != LZO_E_OK) throw std::runtime_error("lzo compress error");
    out.resize(outLen);
    return out;
}

// Format80 encoder: literal runs + fills only (valid stream, decoder-friendly).
std::vector<uint8_t> lcwCompress(const uint8_t* src, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(len + len / 8 + 16);
    size_t i = 0;
    size_t litStart = 0; // pending literal run start
    auto flushLiterals = [&](size_t end) {
        size_t n = end - litStart;
        size_t p = litStart;
        while (n > 0) {
            size_t take = n > 63 ? 63 : n;
            out.push_back((uint8_t)(0x80 | take));
            out.insert(out.end(), src + p, src + p + take);
            p += take;
            n -= take;
        }
    };
    while (i < len) {
        // count run of identical bytes
        size_t run = 1;
        while (i + run < len && src[i + run] == src[i] && run < 0xFFFF) run++;
        if (run >= 3) {
            flushLiterals(i);
            out.push_back(0xFE);
            out.push_back((uint8_t)(run & 0xFF));
            out.push_back((uint8_t)(run >> 8));
            out.push_back(src[i]);
            i += run;
            litStart = i;
        } else {
            i += run;
        }
    }
    flushLiterals(len);
    out.push_back(0x80); // end marker
    return out;
}

// chunked pack: [u16 inSize][u16 outSize][compressed block] per 8192 input bytes
static std::vector<uint8_t> chunkedPack(const std::vector<uint8_t>& input,
                                        bool lzo) {
    std::vector<uint8_t> out;
    size_t pos = 0;
    while (pos < input.size()) {
        size_t take = std::min<size_t>(input.size() - pos, 8192);
        std::vector<uint8_t> block = lzo
            ? lzo1xCompress(input.data() + pos, take)
            : lcwCompress(input.data() + pos, take);
        uint16_t in = (uint16_t)block.size(), outn = (uint16_t)take;
        out.insert(out.end(), (uint8_t*)&in, (uint8_t*)&in + 2);
        out.insert(out.end(), (uint8_t*)&outn, (uint8_t*)&outn + 2);
        out.insert(out.end(), block.begin(), block.end());
        pos += take;
    }
    return out;
}

std::string packSectionNumbered(const std::vector<uint8_t>& packed) {
    std::string b64 = base64Encode(packed.data(), packed.size());
    std::string out;
    int key = 1;
    for (size_t i = 0; i < b64.size(); i += 71) {
        out += std::to_string(key++) + "=" + b64.substr(i, 71) + "\r\n";
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

    overlayType.clear();
    overlayFrame.clear();
    overlayBytes = 0;
    auto itOv = ini.sections.find("OverlayPack");
    if (itOv != ini.sections.end()) {
        try {
            std::vector<uint8_t> packed = base64Decode(joinSection(itOv->second));
            std::vector<uint8_t> raw = lcwDecompress(packed.data(), packed.size(), 512 * 512);
            overlayBytes = (int)raw.size();
            if (raw.size() == 512 * 512) overlayType = std::move(raw);
        } catch (const std::exception&) {
            overlayBytes = -1; // present but undecodable
        }
    }
    auto itOvd = ini.sections.find("OverlayDataPack");
    if (itOvd != ini.sections.end()) {
        try {
            std::vector<uint8_t> packed = base64Decode(joinSection(itOvd->second));
            std::vector<uint8_t> raw = lcwDecompress(packed.data(), packed.size(), 512 * 512);
            if (raw.size() == 512 * 512) overlayFrame = std::move(raw);
        } catch (const std::exception&) {
        }
    }

    return true;
}

std::string Map::serializeToData() {
    // --- waypoints ---
    {
        IniSection sec;
        for (const auto& w : waypoints)
            sec.entries.emplace_back(std::to_string(w.id),
                                     std::to_string(w.ry * 1000 + w.rx));
        ini.sections["Waypoints"] = std::move(sec);
    }

    // --- IsoMapPack5 ---
    {
        std::vector<uint8_t> raw;
        raw.reserve(cells.size() * 11 + 11);
        for (const auto& c : cells) {
            raw.insert(raw.end(), (uint8_t*)&c.x, (uint8_t*)&c.x + 11);
        }
        uint8_t term[11] = {};
        raw.insert(raw.end(), term, term + 11);
        ini.sections["IsoMapPack5"].entries = {
            {"__packed__", packSectionNumbered(chunkedPack(raw, true))}};
    }

    // --- OverlayPack / OverlayDataPack (engine arrays: always write full 512*512) ---
    {
        std::vector<uint8_t> types(512 * 512, 0), frames(512 * 512, 0);
        if (overlayType.size() == 512 * 512) types = overlayType;
        if (overlayFrame.size() == 512 * 512) frames = overlayFrame;
        ini.sections["OverlayPack"].entries = {
            {"__packed__", packSectionNumbered(chunkedPack(types, false))}};
        ini.sections["OverlayDataPack"].entries = {
            {"__packed__", packSectionNumbered(chunkedPack(frames, false))}};
    }

    // --- assemble: replace the packed placeholder entries with numbered keys ---
    std::string out;
    for (auto& [name, sec] : ini.sections) {
        out += "[" + name + "]\r\n";
        if (auto it = std::find_if(sec.entries.begin(), sec.entries.end(),
                                   [](const auto& kv) { return kv.first == "__packed__"; });
            it != sec.entries.end()) {
            out += it->second;
        } else {
            for (auto& [k, v] : sec.entries) out += k + "=" + v + "\r\n";
        }
        out += "\r\n";
    }
    return out;
}

} // namespace ra2
