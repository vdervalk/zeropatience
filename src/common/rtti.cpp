#include "rtti.h"

#include <set>
#include <unordered_map>

namespace zp {

std::string demangleTypeName(const std::string& m) {
    // ".?AVFoo@@" (class) of ".?AUFoo@@" (struct). Genest -> "Foo@Bar@@".
    if (m.size() < 7 || m.compare(0, 4, ".?AV") != 0) {
        if (m.size() < 7 || m.compare(0, 4, ".?AU") != 0) return "";
    }
    size_t end = m.rfind("@@");
    if (end == std::string::npos || end <= 4) return "";
    return m.substr(4, end - 4);
}

// Zoekt alle voorkomens van een byte-reeks binnen de snapshots van het image.
static std::vector<uint64_t> findBytesInImage(const Target& t,
                                              const void* needle, size_t n) {
    std::vector<uint64_t> hits;
    const uint8_t* pat = (const uint8_t*)needle;
    for (const Snapshot& s : t.snapshots()) {
        if (!t.inImage(s.base)) continue;
        const uint8_t* d = s.bytes.data();
        size_t len = s.bytes.size();
        if (len < n) continue;
        for (size_t i = 0; i + n <= len; ++i) {
            if (d[i] == pat[0] && memcmp(d + i, pat, n) == 0)
                hits.push_back(s.base + i);
        }
    }
    return hits;
}

// Eén lineaire pass over het image die alle gezochte 4-byte waarden tegelijk
// opspoort. Veel goedkoper dan per waarde opnieuw scannen.
static std::unordered_map<uint32_t, std::vector<uint64_t>>
findDwordsInImage(const Target& t, const std::set<uint32_t>& wanted) {
    std::unordered_map<uint32_t, std::vector<uint64_t>> out;
    if (wanted.empty()) return out;
    for (const Snapshot& s : t.snapshots()) {
        if (!t.inImage(s.base)) continue;
        const uint8_t* d = s.bytes.data();
        size_t len = s.bytes.size();
        for (size_t i = 0; i + 4 <= len; i += 4) {
            uint32_t v;
            memcpy(&v, d + i, 4);
            if (v && wanted.count(v)) out[v].push_back(s.base + i);
        }
    }
    return out;
}

static std::unordered_map<uint32_t, std::vector<uint64_t>>
findDwordsInImageAligned8(const Target& t, const std::set<uint32_t>& wanted) {
    // 64-bit COL-velden zijn image-relatieve DWORDs; die staan niet per se op
    // een 8-byte grens, dus scannen we op 4-byte stappen.
    return findDwordsInImage(t, wanted);
}

std::vector<uint64_t> readVtable(const Target& t, uint64_t vtable, uint32_t maxSlots) {
    std::vector<uint64_t> slots;
    for (uint32_t i = 0; i < maxSlots; ++i) {
        uint64_t fn = 0;
        if (!t.rptr(vtable + (uint64_t)i * t.ptrSize(), fn)) break;
        if (!fn || !t.isExecutable(fn)) break;
        slots.push_back(fn);
    }
    return slots;
}

std::vector<VtableInfo> scanRtti(const Target& t,
                                 const std::vector<std::string>& classNames) {
    std::vector<VtableInfo> out;
    const bool is64 = t.is64();
    const uint64_t base = t.imageBase();

    // Stap 1: van naam naar TypeDescriptor.
    struct TdHit { std::string cls, mangled; uint64_t td; };
    std::vector<TdHit> tds;
    for (const std::string& cls : classNames) {
        for (const char* prefix : {".?AV", ".?AU"}) {
            std::string mangled = std::string(prefix) + cls + "@@";
            // Inclusief de afsluitende NUL, anders matcht "Player" ook binnen
            // "PlayerList" en krijgen we vals-positieve descriptors.
            std::vector<uint64_t> hits =
                findBytesInImage(t, mangled.c_str(), mangled.size() + 1);
            for (uint64_t nameAddr : hits) {
                uint64_t td = is64 ? nameAddr - 0x10 : nameAddr - 0x08;
                tds.push_back({cls, mangled, td});
            }
        }
    }
    if (tds.empty()) return out;

    // Stap 2: van TypeDescriptor naar COL.
    std::set<uint32_t> wantTd;
    for (const TdHit& h : tds) {
        uint32_t key = is64 ? (uint32_t)(h.td - base) : (uint32_t)h.td;
        wantTd.insert(key);
    }
    auto tdRefs = is64 ? findDwordsInImageAligned8(t, wantTd)
                       : findDwordsInImage(t, wantTd);

    struct ColHit { const TdHit* td; uint64_t col; uint32_t offset; };
    std::vector<ColHit> cols;
    for (const TdHit& h : tds) {
        uint32_t key = is64 ? (uint32_t)(h.td - base) : (uint32_t)h.td;
        auto it = tdRefs.find(key);
        if (it == tdRefs.end()) continue;
        for (uint64_t ref : it->second) {
            uint64_t col = ref - 0x0C;   // pTypeDescriptor staat op COL+0x0C
            uint32_t sig = 0, off = 0;
            if (!t.r32(col + 0x00, sig)) continue;
            if (!t.r32(col + 0x04, off)) continue;
            if (is64) {
                if (sig != 1) continue;
                uint32_t self = 0;
                if (!t.r32(col + 0x14, self)) continue;
                if (base + self != col) continue;   // pSelf moet kloppen
            } else {
                if (sig != 0) continue;
            }
            if (off > 0x1000) continue;             // subobject-offset is klein
            cols.push_back({&h, col, off});
        }
    }
    if (cols.empty()) return out;

    // Stap 3: van COL naar vtable. Het woord vlak voor de vtable wijst naar de COL.
    std::set<uint32_t> wantCol;
    for (const ColHit& c : cols) wantCol.insert((uint32_t)c.col);
    // In een 64-bit image staat er een volledige 8-byte pointer voor de vtable.
    std::unordered_map<uint64_t, std::vector<uint64_t>> colRefs64;
    if (is64) {
        for (const Snapshot& s : t.snapshots()) {
            if (!t.inImage(s.base)) continue;
            const uint8_t* d = s.bytes.data();
            for (size_t i = 0; i + 8 <= s.bytes.size(); i += 8) {
                uint64_t v;
                memcpy(&v, d + i, 8);
                if (!v) continue;
                for (const ColHit& c : cols)
                    if (v == c.col) colRefs64[v].push_back(s.base + i);
            }
        }
    }
    auto colRefs32 = is64 ? std::unordered_map<uint32_t, std::vector<uint64_t>>()
                          : findDwordsInImage(t, wantCol);

    std::set<uint64_t> seen;
    for (const ColHit& c : cols) {
        std::vector<uint64_t> refs;
        if (is64) {
            auto it = colRefs64.find(c.col);
            if (it != colRefs64.end()) refs = it->second;
        } else {
            auto it = colRefs32.find((uint32_t)c.col);
            if (it != colRefs32.end()) refs = it->second;
        }
        for (uint64_t ref : refs) {
            uint64_t vt = ref + t.ptrSize();
            uint64_t slot0 = 0;
            if (!t.rptr(vt, slot0)) continue;
            if (!slot0 || !t.isExecutable(slot0)) continue;   // geen echte vtable
            if (!seen.insert(vt).second) continue;

            VtableInfo v;
            v.cls = c.td->cls;
            v.mangled = c.td->mangled;
            v.typeDesc = c.td->td;
            v.col = c.col;
            v.subobjectOffset = c.offset;
            v.vtable = vt;
            v.slotCount = (uint32_t)readVtable(t, vt, 256).size();
            out.push_back(v);
        }
    }

    std::sort(out.begin(), out.end(), [](const VtableInfo& a, const VtableInfo& b) {
        if (a.cls != b.cls) return a.cls < b.cls;
        return a.subobjectOffset < b.subobjectOffset;
    });
    return out;
}

} // namespace zp
