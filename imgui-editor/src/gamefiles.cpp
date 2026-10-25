#include "gamefiles.h"

#include <algorithm>
#include <cstring>

#include "blowfish_tables.h"

namespace ra2 {

// ---------------------------------------------------------------- crc32 / id

static uint32_t CRC_TABLE[256];
static bool crcTableInit = [] {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : (c >> 1);
        CRC_TABLE[n] = c;
    }
    return true;
}();

uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) crc = CRC_TABLE[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

uint32_t mixFileId(const char* name) {
    std::string s;
    for (const char* p = name; *p; p++) {
        char c = *p;
        if (c == '/') c = '\\';
        s += (char)toupper((unsigned char)c);
    }
    size_t l = s.size();
    size_t a = l & ~(size_t)3;
    if (l & 3) {
        s += (char)(l - a);
        char first = s[a];
        for (size_t i = 0; i < 3 - (l & 3); i++) s += first;
    }
    return crc32((const uint8_t*)s.data(), s.size());
}

// ---------------------------------------------------------------- blowfish

namespace {

struct Blowfish {
    uint32_t p[18];
    uint32_t s[1024];

    static uint32_t rd32(const uint8_t* b) {
        return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    }
    static void wr32(uint8_t* b, uint32_t v) {
        b[0] = (v >> 24) & 0xff; b[1] = (v >> 16) & 0xff;
        b[2] = (v >> 8) & 0xff; b[3] = v & 0xff;
    }

    Blowfish(const uint8_t* key, size_t keyLen) {
        memcpy(p, BF_P, sizeof(p));
        memcpy(s, BF_S, sizeof(s));
        size_t j = 0;
        for (int i = 0; i < 18; i++) {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) {
                v = (v << 8) | (j < keyLen ? key[j] : 0);
                j++;
            }
            p[i] ^= v;
        }
        uint8_t block[8] = {};
        for (int i = 0; i < 18; i += 2) {
            cipherBlock(block);
            p[i] = rd32(block);
            p[i + 1] = rd32(block + 4);
        }
        for (int box = 0; box < 4; box++) {
            for (int i = 0; i < 256; i += 2) {
                cipherBlock(block);
                s[box * 256 + i] = rd32(block);
                s[box * 256 + i + 1] = rd32(block + 4);
            }
        }
    }

    uint32_t f(uint32_t x) const {
        uint32_t a = (x >> 24) & 0xff, b = (x >> 16) & 0xff, c = (x >> 8) & 0xff, d = x & 0xff;
        return ((s[a] + s[256 + b]) ^ s[512 + c]) + s[768 + d];
    }

    void cipherBlock(uint8_t* block) {
        uint32_t xl = rd32(block), xr = rd32(block + 4);
        for (int i = 0; i < 16; i++) {
            xl ^= p[i];
            xr ^= f(xl);
            uint32_t t = xl; xl = xr; xr = t;
        }
        uint32_t t = xl;
        xl = xr ^ p[17];
        xr = t ^ p[16];
        wr32(block, xl);
        wr32(block + 4, xr);
    }

    void decipherBlock(uint8_t* block) {
        uint32_t xl = rd32(block), xr = rd32(block + 4);
        for (int i = 17; i > 1; i--) {
            xl ^= p[i];
            xr ^= f(xl);
            uint32_t t = xl; xl = xr; xr = t;
        }
        uint32_t t = xl;
        xl = xr ^ p[0];
        xr = t ^ p[1];
        wr32(block, xl);
        wr32(block + 4, xr);
    }
};

// ------------------------------------------------------------ tiny bignum (320-bit) for the RSA step

const int NW = 12; // 384 bits, enough for 320-bit modulus products/carries
struct Big {
    uint32_t w[NW] = {}; // little-endian words
    bool isZero() const {
        for (int i = 0; i < NW; i++) if (w[i]) return false;
        return true;
    }
    static int cmp(const Big& a, const Big& b) {
        for (int i = NW - 1; i >= 0; i--) {
            if (a.w[i] < b.w[i]) return -1;
            if (a.w[i] > b.w[i]) return 1;
        }
        return 0;
    }
    static void subInPlace(Big& a, const Big& b) { // a -= b (a >= b)
        uint64_t borrow = 0;
        for (int i = 0; i < NW; i++) {
            uint64_t v = (uint64_t)a.w[i] - b.w[i] - borrow;
            a.w[i] = (uint32_t)v;
            borrow = (v >> 63) & 1;
        }
    }
    static void addInPlace(Big& a, const Big& b) { // a += b
        uint64_t carry = 0;
        for (int i = 0; i < NW; i++) {
            uint64_t v = (uint64_t)a.w[i] + b.w[i] + carry;
            a.w[i] = (uint32_t)v;
            carry = v >> 32;
        }
    }
    void shl1() {
        uint32_t carry = 0;
        for (int i = 0; i < NW; i++) {
            uint32_t nc = w[i] >> 31;
            w[i] = (w[i] << 1) | carry;
            carry = nc;
        }
    }
};

static Big mulmod(const Big& a, const Big& b, const Big& n) {
    Big prod; // zero
    Big acc = b;
    for (int bit = 0; bit < 384; bit++) {
        if ((a.w[bit >> 5] >> (bit & 31)) & 1) {
            Big::addInPlace(prod, acc);
            if (Big::cmp(prod, n) >= 0) Big::subInPlace(prod, n);
        }
        if (bit < 383) {
            acc.shl1();
            if (Big::cmp(acc, n) >= 0) Big::subInPlace(acc, n);
        }
    }
    return prod;
}

// result = base^65537 mod n (e is always 0x10001 for Westwood keys)
static Big modPow65537(Big base, const Big& n) {
    Big result;
    result.w[0] = 1;
    // e = 0x10001: set bits are 0 and 16
    for (int bit = 16; bit >= 0; bit--) {
        result = mulmod(result, result, n);
        if (bit == 0 || bit == 16) result = mulmod(result, base, n);
    }
    return result;
}

// Recover the 56-byte Blowfish key for a protected MIX from the 80-byte key source.
static bool getBlowfishKey(const uint8_t* s, size_t sLen, uint8_t out[56]) {
    // public key (DER integer, base64) — Westwood 320-bit RSA modulus
    static const char* PUBKEY_B64 = "AihRvNoIbTn85FZRYNZRcT+i6KpU+maCsEqr3Q5q+LDB5tH7Tz2qQ38V";
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t der[64];
    size_t derLen = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (const char* p = PUBKEY_B64; *p; p++) {
        const char* hit = strchr(B64, *p);
        if (!hit) continue;
        acc = (acc << 6) | (uint32_t)(hit - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            der[derLen++] = (uint8_t)(acc >> bits);
        }
    }
    if (der[0] != 0x02) return false;
    size_t pos = 1;
    size_t len = der[pos++];
    if (len & 0x80) {
        int n = len & 0x7f;
        len = 0;
        for (int i = 0; i < n; i++) len = (len << 8) | der[pos++];
    }
    // modulus, big-endian, len bytes (39)
    Big modulus;
    if (len > 39) return false; // must fit 320 bits + slack
    for (size_t i = 0; i < len; i++) {
        uint32_t b = der[pos + i];
        size_t beIdx = len - 1 - i; // byte position from LSB
        modulus.w[beIdx >> 2] |= b << ((beIdx & 3) * 8);
    }

    size_t a = (len * 8 - 2) >> 3; // 39
    uint8_t outBuf[256];
    size_t outPos = 0;
    size_t pre = 0;
    size_t preLen = (size_t)((55 / a + 1) * (a + 1)); // 80
    while (a + 1 <= preLen && pre + a + 1 <= sLen) {
        // block (a+1 bytes) reversed = big-endian number
        Big block;
        for (size_t i = 0; i < a + 1; i++) {
            uint32_t b = s[pre + i];
            size_t beIdx = a - i; // reverse
            block.w[beIdx >> 2] |= b << ((beIdx & 3) * 8);
        }
        Big m = modPow65537(block, modulus);
        for (size_t i = 0; i < a; i++) {
            outBuf[outPos++] = (uint8_t)(m.w[i >> 2] >> ((i & 3) * 8));
        }
        pre += a + 1;
        preLen -= a + 1;
    }
    if (outPos < 56) return false;
    memcpy(out, outBuf, 56);
    return true;
}

} // namespace

// ---------------------------------------------------------------- MixFile

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rdu32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static int32_t rd32(const uint8_t* p) { return (int32_t)rdu32(p); }

bool MixFile::parse(const uint8_t* data, size_t dataLen, uint64_t fullSize, std::string* err) {
    if (dataLen < 16) { if (err) *err = "mix too small"; return false; }
    uint32_t flags = rdu32(data);
    entries.clear();
    if ((flags & 0xffff) != 0) {
        if (rd16(data) != 0 && parsePlainAt(data, dataLen, 0)) return true;
    }
    if (tryEncrypted(data, dataLen, fullSize)) return true;
    if (parsePlainAt(data, dataLen, 4)) return true;
    if (err) *err = "not a valid MIX archive";
    return false;
}

bool MixFile::parsePlainAt(const uint8_t* data, size_t dataLen, size_t prefix) {
    if (prefix + 6 > dataLen) return false;
    uint16_t cFiles = rd16(data + prefix);
    size_t indexEnd = prefix + 6 + (size_t)cFiles * 12;
    if (cFiles < 1 || cFiles > 300000 || indexEnd > dataLen) return false;
    dataOffset = (int64_t)indexEnd;
    const uint8_t* p = data + prefix + 6;
    entries.reserve(cFiles);
    for (int i = 0; i < cFiles; i++, p += 12) {
        entries.push_back({rdu32(p), dataOffset + rd32(p + 4), rd32(p + 8)});
    }
    return true;
}

bool MixFile::tryEncrypted(const uint8_t* data, size_t dataLen, uint64_t fullSize) {
    uint32_t flags = rdu32(data);
    if (dataLen < 92 || !(flags & 0x00020000)) return false;

    uint8_t key[56];
    if (!getBlowfishKey(data + 4, 80, key)) return false;
    Blowfish bf(key, 56);

    uint8_t header[8];
    memcpy(header, data + 84, 8);
    bf.decipherBlock(header);
    uint16_t cFiles = rd16(header);
    uint32_t size = rdu32(header + 2);
    uint64_t cbF = ((uint64_t)cFiles * 12 + 5) & ~(uint64_t)7;
    uint64_t dataStart = 92 + cbF;
    uint64_t withChecksum = (flags & 0x00010000) ? 20 : 0;
    if (cFiles == 0 || cFiles >= 0x10000 || dataStart + size + withChecksum != fullSize) return false;
    if (dataStart > dataLen) return false; // caller must supply at least the index area

    std::vector<uint8_t> area(data + 84, data + dataStart);
    for (size_t off = 0; off + 8 <= area.size(); off += 8) bf.decipherBlock(area.data() + off);
    dataOffset = (int64_t)dataStart;
    if ((size_t)dataStart < 90 + (size_t)cFiles * 12) return false;
    const uint8_t* p = area.data() + 6; // entries begin at area offset 6
    entries.reserve(cFiles);
    for (int i = 0; i < cFiles; i++, p += 12) {
        entries.push_back({rdu32(p), dataOffset + rd32(p + 4), rd32(p + 8)});
    }
    return true;
}

bool MixFile::has(uint32_t id) const {
    for (const auto& e : entries) if (e.id == id) return true;
    return false;
}

const MixFile::Entry* MixFile::find(uint32_t id) const {
    for (const auto& e : entries) if (e.id == id) return &e;
    return nullptr;
}

// ---------------------------------------------------------------- DiskMix

DiskMix::~DiskMix() {
    if (f_) fclose(f_);
}

bool DiskMix::open(const std::wstring& path, std::string* err) {
    f_ = _wfopen(path.c_str(), L"rb");
    if (!f_) { if (err) *err = "cannot open"; return false; }
    _fseeki64(f_, 0, SEEK_END);
    fullSize_ = (uint64_t)_ftelli64(f_);
    _fseeki64(f_, 0, SEEK_SET);

    // read enough for any plausible header/index: grow if needed
    size_t want = 8 << 20;
    if (want > fullSize_) want = (size_t)fullSize_;
    std::vector<uint8_t> buf(want);
    size_t got = fread(buf.data(), 1, want, f_);
    buf.resize(got);

    if (!idx_.parse(buf.data(), buf.size(), fullSize_, err)) return false;

    // if the encrypted index area is bigger than what we read, re-read
    uint64_t need = (uint64_t)idx_.dataOffset;
    if (need > got && need <= fullSize_) {
        buf.resize((size_t)need);
        _fseeki64(f_, 0, SEEK_SET);
        size_t got2 = fread(buf.data(), 1, (size_t)need, f_);
        buf.resize(got2);
        if (!idx_.parse(buf.data(), buf.size(), fullSize_, err)) return false;
    }

    // name
    std::wstring fn = path;
    size_t slash = fn.find_last_of(L"/\\");
    if (slash != std::wstring::npos) fn = fn.substr(slash + 1);
    name_.assign(fn.begin(), fn.end());
    for (auto& c : name_) c = (char)tolower((unsigned char)c);
    return true;
}

bool DiskMix::read(const char* fname, std::vector<uint8_t>& out) const {
    const MixFile::Entry* e = idx_.find(mixFileId(fname));
    if (!e || e->size <= 0) return false;
    out.resize((size_t)e->size);
    if (_fseeki64(f_, e->offset, SEEK_SET) != 0) return false;
    size_t got = fread(out.data(), 1, (size_t)e->size, f_);
    if (got != (size_t)e->size) return false;
    return true;
}

bool MemMix::read(const char* fname, const uint8_t** out, size_t* outSize) const {
    const MixFile::Entry* e = idx.find(mixFileId(fname));
    if (!e || e->size <= 0) return false;
    *out = data.data() + e->offset;
    *outSize = (size_t)e->size;
    return true;
}

// ---------------------------------------------------------------- GameAssets

static const char* SUB_MIX_NAMES[] = {
    "conquer.mix", "temperat.mix", "urban.mix", "snow.mix", "urbann.mix",
    "lunar.mix", "desert.mix",
    "isotemp.mix", "isosnow.mix", "isourb.mix", "isoubn.mix", "isolun.mix",
    "isodes.mix", "isogen.mix", "generic.mix", "cache.mix", "local.mix",
    "tem.mix", "sno.mix", "urb.mix", "ubn.mix", "lun.mix", "des.mix",
    "marble.mix", "language.mix", "langmd.mix",
    "cachemd.mix", "localmd.mix", "conqmd.mix", "genermd.mix", "isogenmd.mix",
    "cameomd.mix", "thememd.mix", "multimd.mix", "audiomd.mix",
};

bool GameAssets::open(const std::wstring& dir, std::string* err) {
    dir_ = dir;

    std::vector<std::wstring> mixPaths;
    std::wstring pattern = dir + L"\\*.mix";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = "no .mix files in dir"; return false; }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            mixPaths.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // open order: ra2, ra2md first, then the rest alphabetically
    auto isPriority = [](const std::wstring& p) {
        std::wstring n = p.substr(p.find_last_of(L"/\\") + 1);
        for (auto& c : n) c = (wchar_t)towlower(c);
        return n == L"ra2.mix" || n == L"ra2md.mix";
    };
    std::stable_sort(mixPaths.begin(), mixPaths.end(), [&](const std::wstring& a, const std::wstring& b) {
        bool pa = isPriority(a), pb = isPriority(b);
        if (pa != pb) return pa;
        return false; // stable: keep alphabetical scan order otherwise
    });

    for (auto& p : mixPaths) {
        DiskMix* m = new DiskMix();
        std::string e;
        if (m->open(p, &e)) {
            diskMixes_.push_back(m);
        } else {
            delete m;
        }
    }
    if (diskMixes_.empty()) { if (err) *err = "no readable .mix archives"; return false; }
    openSubMixes();
    return true;
}

void GameAssets::openSubMixes() {
    for (int depth = 0; depth < 2; depth++) {
        size_t before = memMixes_.size();
        for (const char* name : SUB_MIX_NAMES) {
            for (DiskMix* dm : diskMixes_) {
                if (!dm->has(name)) continue;
                MemMix mm;
                if (dm->read(name, mm.data) && mm.parse()) {
                    // dedup by first entry id set signature: skip identical nested mixes
                    bool dup = false;
                    for (auto& existing : memMixes_) {
                        if (existing.data.size() == mm.data.size() &&
                            !existing.data.empty() &&
                            memcmp(existing.data.data(), mm.data.data(),
                                   std::min(existing.data.size(), (size_t)2048)) == 0) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) memMixes_.push_back(std::move(mm));
                }
                break; // first disk mix containing it wins
            }
        }
        // also look inside freshly opened mem mixes for another level
        if (memMixes_.size() == before) break;
    }
}

bool GameAssets::find(const char* name, std::vector<uint8_t>& out) const {
    // loose file in the game dir first (mod repacks keep rules/art INIs unpacked)
    if (!dir_.empty()) {
        std::wstring wname;
        for (const char* p = name; *p; p++) wname += (*p == '/') ? L'\\' : (wchar_t)(unsigned char)*p;
        std::wstring path = dir_ + L"\\" + wname;
        if (FILE* f = _wfopen(path.c_str(), L"rb")) {
            bool ok = false;
            _fseeki64(f, 0, SEEK_END);
            long long sz = _ftelli64(f);
            _fseeki64(f, 0, SEEK_SET);
            if (sz > 0 && sz < 400LL * 1024 * 1024) {
                out.resize((size_t)sz);
                ok = fread(out.data(), 1, (size_t)sz, f) == (size_t)sz;
            }
            fclose(f);
            if (ok) return true;
            out.clear();
        }
    }
    for (const DiskMix* dm : diskMixes_) {
        if (dm->read(name, out)) return true;
    }
    for (const MemMix& mm : memMixes_) {
        const uint8_t* p;
        size_t n;
        if (mm.read(name, &p, &n)) {
            out.assign(p, p + n);
            return true;
        }
    }
    return false;
}

bool GameAssets::hasAny(const char* name) const {
    uint32_t id = mixFileId(name);
    for (const DiskMix* dm : diskMixes_) if (dm->has(name)) return true;
    for (const MemMix& mm : memMixes_) if (mm.has(name)) return true;
    return false;
}

} // namespace ra2
