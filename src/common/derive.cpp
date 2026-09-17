#include "derive.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace zp {

// Is dit adres een geloofwaardige vtable? Vier opeenvolgende slots moeten
// naar uitvoerbare code wijzen. Met slechts een slot glipt opvulling erdoor:
// in een echte meting haalde 0x00555555 het histogram, puur omdat een blok
// 0x55-bytes toevallig naar uitvoerbaar geheugen wees.
static bool looksLikeVtable(const Target& t, uint64_t vt) {
    if (!vt || !t.inImage(vt)) return false;
    if (vt & (t.ptrSize() - 1)) return false;
    for (int i = 0; i < 4; ++i) {
        uint64_t fn = 0;
        if (!t.rptr(vt + (uint64_t)i * t.ptrSize(), fn)) return false;
        if (!fn || !t.isExecutable(fn)) return false;
    }
    return true;
}

// Wijst dit woord naar een object met een geloofwaardige vtable?
static bool looksLikeInstance(const Target& t, uint64_t p, uint64_t* vtableOut) {
    if (!p || (p & (t.ptrSize() - 1)) != 0) return false;
    uint64_t vt = 0;
    if (!t.rptr(p, vt)) return false;
    if (!looksLikeVtable(t, vt)) return false;
    if (vtableOut) *vtableOut = vt;
    return true;
}

std::vector<PlayerListHit> findPlayerLists(const Target& t) {
    std::vector<PlayerListHit> out;
    const size_t ps = t.ptrSize();

    // m_playerCount is een Int van 4 bytes, gevolgd door een pointer-array.
    // In een 32-bit build sluit dat naadloos aan; in een 64-bit build schuift
    // de compiler er 4 bytes opvulling tussen. Beide varianten proberen.
    std::vector<size_t> arrayOffsets = {ps + 4};
    if (ps == 8) arrayOffsets.push_back(ps + 8);

    for (const Snapshot& s : t.snapshots()) {
        if (!s.region || !s.region->writable()) continue;
        const size_t len = s.bytes.size();

        for (size_t ao : arrayOffsets) {
            const size_t need = ao + 16 * ps;
            if (len < need) continue;

            for (size_t i = 0; i + need <= len; i += 4) {
                const uint64_t a = s.base + i;

                uint32_t count = 0;
                if (!t.r32(a + ps, count)) continue;
                if (count < 1 || count > 16) continue;

                uint64_t local = 0;
                if (!t.rptr(a, local) || !local) continue;

                // Alle zestien slots zijn altijd gealloceerd; m_playerCount
                // zegt alleen hoeveel er meedoen. Eisen dat de achterste NULL
                // zijn is fout en vindt in een echt potje nooit iets.
                const uint64_t arr = a + ao;
                std::vector<uint64_t> players(16, 0);
                bool ok = true;
                uint64_t vt0 = 0;
                for (int k = 0; k < 16 && ok; ++k) {
                    if (!t.rptr(arr + (uint64_t)k * ps, players[k])) { ok = false; break; }
                    uint64_t vt = 0;
                    if (!looksLikeInstance(t, players[k], &vt)) { ok = false; break; }
                    if (k == 0) vt0 = vt;
                    else if (vt != vt0) ok = false;   // allemaal dezelfde klasse
                }
                if (!ok) continue;

                // Zestien verschillende objecten, en m_local moet een van de
                // spelers zijn die daadwerkelijk meedoen.
                std::set<uint64_t> uniq(players.begin(), players.end());
                if (uniq.size() != 16) continue;
                int localIdx = -1;
                for (uint32_t k = 0; k < count; ++k)
                    if (players[k] == local) { localIdx = (int)k; break; }
                if (localIdx < 0) continue;

                PlayerListHit h;
                h.addr = a;
                h.arrayOffset = (uint32_t)ao;
                h.playerCount = count;
                h.localIndex = localIdx;
                h.localPlayer = local;
                h.players = players;
                h.playerVtable = vt0;
                out.push_back(h);
            }
        }
    }
    return out;
}

std::vector<VtableCount> vtableHistogram(const Target& t, size_t topN) {
    std::unordered_map<uint64_t, uint64_t> counts;
    std::unordered_set<uint64_t> good, bad;   // cache: is dit een echte vtable?

    const size_t ps = t.ptrSize();
    for (const Snapshot& s : t.snapshots()) {
        if (!s.region || s.region->type != MEM_PRIVATE) continue;
        if (!s.region->writable()) continue;
        const uint8_t* d = s.bytes.data();
        const size_t len = s.bytes.size();
        for (size_t i = 0; i + ps <= len; i += ps) {
            uint64_t v = 0;
            if (ps == 4) { uint32_t x; memcpy(&x, d + i, 4); v = x; }
            else         { memcpy(&v, d + i, 8); }
            if (!v || !t.inImage(v)) continue;
            if (bad.count(v)) continue;
            if (!good.count(v)) {
                if (!looksLikeVtable(t, v)) { bad.insert(v); continue; }
                good.insert(v);
            }
            counts[v]++;
        }
    }

    std::vector<VtableCount> out;
    out.reserve(counts.size());
    for (auto& kv : counts) out.push_back({kv.first, kv.second});
    std::sort(out.begin(), out.end(), [](const VtableCount& a, const VtableCount& b) {
        return a.count > b.count;
    });
    if (out.size() > topN) out.resize(topN);
    return out;
}

std::vector<uint64_t> instancesOf(const Target& t, uint64_t vtable, size_t maxHits) {
    std::vector<uint64_t> out;
    const size_t ps = t.ptrSize();
    for (const Snapshot& s : t.snapshots()) {
        if (!s.region || s.region->type != MEM_PRIVATE) continue;
        if (!s.region->writable()) continue;
        const uint8_t* d = s.bytes.data();
        const size_t len = s.bytes.size();
        for (size_t i = 0; i + ps <= len; i += ps) {
            uint64_t v = 0;
            if (ps == 4) { uint32_t x; memcpy(&x, d + i, 4); v = x; }
            else         { memcpy(&v, d + i, 8); }
            if (v != vtable) continue;
            out.push_back(s.base + i);
            if (out.size() >= maxHits) return out;
        }
    }
    return out;
}

std::vector<LinkPair> findDoubleLinks(const Target& t,
                                      const std::vector<uint64_t>& vtables,
                                      size_t instancesPerVtable,
                                      int32_t reachA,
                                      int32_t reachB,
                                      bool requireDistinct) {
    std::unordered_set<uint64_t> vtSet(vtables.begin(), vtables.end());
    std::map<std::tuple<uint64_t, int32_t, uint64_t, int32_t>, uint32_t> tally;
    const int32_t ps = (int32_t)t.ptrSize();

    for (uint64_t vtA : vtables) {
        std::vector<uint64_t> insts = instancesOf(t, vtA, instancesPerVtable);
        for (uint64_t A : insts) {
            for (int32_t oa = -reachA; oa <= reachA; oa += ps) {
                uint64_t B = 0;
                if (!t.rptr((uint64_t)((int64_t)A + oa), B) || !B) continue;
                if (B == A) continue;
                uint64_t vtB = 0;
                if (!t.rptr(B, vtB)) continue;
                if (!vtSet.count(vtB)) continue;
                // Object heeft m_next en m_prev. Zo'n dubbelgelinkte lijst
                // geeft perfecte wederzijdse verwijzingen en verdringt de
                // echte Object <-> BodyModule-relatie volledig.
                if (requireDistinct && vtB == vtA) continue;
                for (int32_t ob = -reachB; ob <= reachB; ob += ps) {
                    uint64_t back = 0;
                    if (!t.rptr((uint64_t)((int64_t)B + ob), back)) continue;
                    if (back != A) continue;
                    tally[{vtA, oa, vtB, ob}]++;
                }
            }
        }
    }

    std::vector<LinkPair> out;
    for (auto& kv : tally) {
        LinkPair p;
        p.vtableA    = std::get<0>(kv.first);
        p.offsetAtoB = std::get<1>(kv.first);
        p.vtableB    = std::get<2>(kv.first);
        p.offsetBtoA = std::get<3>(kv.first);
        p.confirmations = kv.second;
        out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const LinkPair& a, const LinkPair& b) {
        return a.confirmations > b.confirmations;
    });
    return out;
}

std::vector<OwnerChain> findOwnerChains(const Target& t,
                                        const std::vector<uint64_t>& objectInstances,
                                        const std::vector<uint64_t>& knownPlayers,
                                        uint32_t maxObjectOffset,
                                        uint32_t maxTeamOffset,
                                        uint32_t maxProtoOffset) {
    std::unordered_set<uint64_t> playerSet(knownPlayers.begin(), knownPlayers.end());
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> tally;
    const size_t ps = t.ptrSize();

    for (uint64_t obj : objectInstances) {
        for (uint32_t o1 = 0; o1 <= maxObjectOffset; o1 += (uint32_t)ps) {
            uint64_t team = 0;
            if (!t.rptr(obj + o1, team) || !team) continue;
            if (!looksLikeInstance(t, team, nullptr)) continue;

            for (uint32_t o2 = 0; o2 <= maxTeamOffset; o2 += (uint32_t)ps) {
                uint64_t proto = 0;
                if (!t.rptr(team + o2, proto) || !proto) continue;
                if (!t.isMapped(proto)) continue;

                for (uint32_t o3 = 0; o3 <= maxProtoOffset; o3 += (uint32_t)ps) {
                    uint64_t player = 0;
                    if (!t.rptr(proto + o3, player) || !player) continue;
                    if (!playerSet.count(player)) continue;
                    tally[{o1, o2, o3}]++;
                }
            }
        }
    }

    std::vector<OwnerChain> out;
    for (auto& kv : tally) {
        OwnerChain c;
        c.objectToTeam  = std::get<0>(kv.first);
        c.teamToProto   = std::get<1>(kv.first);
        c.protoToPlayer = std::get<2>(kv.first);
        c.confirmations = kv.second;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const OwnerChain& a, const OwnerChain& b) {
        return a.confirmations > b.confirmations;
    });
    if (out.size() > 32) out.resize(32);
    return out;
}

static bool plausibleHealth(float v) {
    return std::isfinite(v) && v > 0.0f && v < 200000.0f;
}

std::vector<HealthBlock> findHealthBlocks(const Target& t,
                                          const std::vector<uint64_t>& bodyObjects,
                                          int32_t fromOffset,
                                          int32_t toOffset) {
    std::map<int32_t, HealthBlock> tally;

    for (uint64_t body : bodyObjects) {
        for (int32_t off = fromOffset; off + 16 <= toOffset; off += 4) {
            const uint64_t a = (uint64_t)((int64_t)body + off);
            float cur = 0, prev = 0, mx = 0, init = 0;
            if (!t.rf32(a + 0,  cur))  continue;
            if (!t.rf32(a + 4,  prev)) continue;
            if (!t.rf32(a + 8,  mx))   continue;
            if (!t.rf32(a + 12, init)) continue;

            if (!plausibleHealth(cur) || !plausibleHealth(mx) || !plausibleHealth(init))
                continue;
            if (!std::isfinite(prev) || prev < 0.0f) continue;
            if (cur > mx * 1.001f) continue;            // current mag max niet overstijgen
            if (std::fabs(mx - init) > 0.01f) continue; // max == initial in de regel

            HealthBlock& b = tally[off];
            b.offset = off;
            b.confirmations++;
            b.sampleCurrent = cur;
            b.sampleMax = mx;
        }
    }

    std::vector<HealthBlock> out;
    for (auto& kv : tally) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const HealthBlock& a, const HealthBlock& b) {
        return a.confirmations > b.confirmations;
    });
    if (out.size() > 16) out.resize(16);
    return out;
}

std::vector<HealthVtable> findHealthVtables(const Target& t,
                                            const std::vector<uint64_t>& candidates,
                                            size_t samplesPerVtable,
                                            int32_t fromOffset,
                                            int32_t toOffset) {
    std::vector<HealthVtable> out;

    for (uint64_t vt : candidates) {
        std::vector<uint64_t> insts = instancesOf(t, vt, samplesPerVtable);
        if (insts.size() < 8) continue;   // te weinig om iets over te zeggen

        std::vector<HealthBlock> hb = findHealthBlocks(t, insts, fromOffset, toOffset);
        if (hb.empty()) continue;

        HealthVtable h;
        h.vtable = vt;
        h.offset = hb[0].offset;
        h.confirmations = hb[0].confirmations;
        h.sampled = (uint32_t)insts.size();
        h.ratio = (double)h.confirmations / (double)h.sampled;
        out.push_back(h);
    }

    // De body-module valt op doordat vrijwel elke instantie hitpoints heeft
    // op dezelfde offset. Een toevallige treffer haalt die consistentie niet.
    std::sort(out.begin(), out.end(), [](const HealthVtable& a, const HealthVtable& b) {
        if (a.ratio != b.ratio) return a.ratio > b.ratio;
        return a.confirmations > b.confirmations;
    });
    return out;
}

std::string hexDump(const Target& t, uint64_t anchor, int32_t fromOff, int32_t toOff) {
    std::string s;
    char line[256];
    for (int32_t off = fromOff; off < toOff; off += 16) {
        snprintf(line, sizeof(line), "  %c0x%04x  ",
                 off < 0 ? '-' : '+', (unsigned)(off < 0 ? -off : off));
        s += line;
        std::string ascii;
        for (int k = 0; k < 16; ++k) {
            uint8_t b = 0;
            const uint64_t a = (uint64_t)((int64_t)anchor + off + k);
            if (off + k < toOff && t.r8(a, b)) {
                snprintf(line, sizeof(line), "%02x ", b);
                ascii += (b >= 32 && b < 127) ? (char)b : '.';
            } else {
                snprintf(line, sizeof(line), "?? ");
                ascii += '.';
            }
            s += line;
            if (k == 7) s += ' ';
        }
        s += " |" + ascii + "|\n";
    }
    return s;
}

} // namespace zp
