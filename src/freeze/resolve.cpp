#include "resolve.h"

#include "../common/target.h"
#include "../common/derive.h"

#include <algorithm>
#include <map>
#include <vector>

namespace zp {

namespace {

// Zoekt een globale pointer in het image die naar (ongeveer) target wijst.
// De PlayerList staat op de heap en verhuist per potje, maar de globale
// pointer ernaartoe staat vast in .data. Door die te onthouden blijft de
// hook geldig over potjes heen.
//
// "Ongeveer", want we kennen het adres van m_local, niet het begin van het
// PlayerList-object; de globale pointer wijst naar dat begin. Het verschil
// is de grootte van de basisklassen en dus klein.
bool findGlobalPointerNear(const Target& t, uint64_t target, uint32_t maxDelta,
                           uintptr_t* globalOut, uint32_t* deltaOut) {
    const size_t ps = t.ptrSize();
    for (const Snapshot& s : t.snapshots()) {
        if (!t.inImage(s.base)) continue;
        if (!s.region || !s.region->writable()) continue;   // .data, niet .rdata
        const uint8_t* d = s.bytes.data();
        for (size_t i = 0; i + ps <= s.bytes.size(); i += ps) {
            uint64_t v = 0;
            if (ps == 4) { uint32_t x; memcpy(&x, d + i, 4); v = x; }
            else         { memcpy(&v, d + i, 8); }
            if (!v || v > target) continue;
            uint64_t delta = target - v;
            if (delta > maxDelta) continue;
            *globalOut = (uintptr_t)(s.base + i);
            *deltaOut = (uint32_t)delta;
            return true;
        }
    }
    return false;
}

} // namespace

Resolved resolveInProcess(uint64_t snapshotBudgetMB,
                          void (*log)(const char* fmt, ...)) {
    Resolved r;
    auto say = [&](const char* fmt, ...) {
        if (!log) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log("%s", buf);
    };

    Target t;
    std::string err;
    if (!t.attach(GetCurrentProcessId(), &err)) {
        r.error = "kan het eigen proces niet lezen: " + err;
        return r;
    }

    say("geheugen kopieren (budget %llu MB)...\n",
        (unsigned long long)snapshotBudgetMB);
    t.buildSnapshot(snapshotBudgetMB * 1024ull * 1024ull, false);
    if (t.snapshotBytes() == 0) {
        r.error = "geen geheugen gekopieerd";
        return r;
    }
    say("  %zu regio's, %llu MB\n", t.snapshotCount(),
        (unsigned long long)(t.snapshotBytes() / (1024 * 1024)));

    std::map<uint64_t, uint64_t> counts = vtableCounts(t);
    say("vtables met instanties: %zu\n", counts.size());

    // --- ActiveBody op naam ------------------------------------------------
    std::vector<NameAnchor> anchors =
        findNameAnchors(t, {"ActiveBody"}, 0x1000, counts, 8);
    if (anchors.empty() || anchors[0].candidates.empty()) {
        r.error = "de poolnaam \"ActiveBody\" leverde geen kandidaten op";
        return r;
    }
    say("poolnaam \"ActiveBody\": %zu kandidaten\n", anchors[0].candidates.size());

    // De juiste kandidaat is die waarvan de instanties zowel hitpoints hebben
    // als een wederzijdse verwijzing naar hun Object. Beide eisen samen laten
    // geen ruimte voor een toevallige treffer.
    // Per kandidaat vertellen waar het strandt. Zonder dat is een mislukking
    // een doodlopend spoor, en deze code draait op een machine waar ik niet
    // bij kan.
    say("  %-10s %-10s %-10s %s\n", "VTABLE", "INSTANTIES", "HITPOINTS", "OBJECT-LINK");
    for (const NameAnchor::Candidate& c : anchors[0].candidates) {
        std::vector<uint64_t> insts = instancesOf(t, c.vtable, 256, true);
        if (insts.size() < 8) {
            say("  0x%-8llx %-10zu %-10s %s\n", (unsigned long long)c.vtable,
                insts.size(), "-", "te weinig instanties");
            continue;
        }

        std::vector<HealthBlock> hb = findHealthBlocks(t, insts, -0x40, 0x60);
        const HealthBlock* health = nullptr;
        for (const HealthBlock& b : hb) {
            if (b.offset <= 0) continue;              // hoort achter 'this'
            if (b.medianMax < 10.0f) continue;
            if (b.distinctMax < 2) continue;
            health = &b;
            break;
        }
        if (!health) {
            say("  0x%-8llx %-10zu %-10s %s\n", (unsigned long long)c.vtable,
                insts.size(), "nee", "-");
            continue;
        }

        // De dubbele link: this + d wijst naar een Object, en dat Object
        // wijst op een vaste offset terug naar deze 'this'.
        std::map<std::pair<int32_t, uint32_t>, uint32_t> links;
        std::map<std::pair<int32_t, uint32_t>, uint64_t> linkVtable;
        const int32_t ps = (int32_t)t.ptrSize();
        for (uint64_t self : insts) {
            for (int32_t d = -0x40; d < 0; d += ps) {
                uint64_t obj = 0;
                if (!t.rptr((uint64_t)((int64_t)self + d), obj) || !obj) continue;
                uint64_t objVt = 0;
                if (!t.rptr(obj, objVt)) continue;
                if (!counts.count(objVt)) continue;
                for (uint32_t back = 0; back <= 0x400; back += (uint32_t)ps) {
                    uint64_t p = 0;
                    if (!t.rptr(obj + back, p)) continue;
                    if (p != self) continue;
                    links[{d, back}]++;
                    linkVtable[{d, back}] = objVt;
                }
            }
        }
        if (links.empty()) {
            say("  0x%-8llx %-10zu +0x%-7x %s\n", (unsigned long long)c.vtable,
                insts.size(), (unsigned)health->offset, "geen wederzijdse link");
            continue;
        }

        auto best = std::max_element(links.begin(), links.end(),
                                     [](const auto& a, const auto& b) {
                                         return a.second < b.second;
                                     });
        if (best->second < 8) {
            say("  0x%-8llx %-10zu +0x%-7x link te zwak (%u bevestigingen)\n",
                (unsigned long long)c.vtable, insts.size(),
                (unsigned)health->offset, best->second);
            continue;
        }
        say("  0x%-8llx %-10zu +0x%-7x %d bevestigingen  <-- gekozen\n",
            (unsigned long long)c.vtable, insts.size(),
            (unsigned)health->offset, (int)best->second);

        r.bodyVtable          = (uintptr_t)c.vtable;
        r.objectVtable        = (uintptr_t)linkVtable[best->first];
        r.thisToObject        = best->first.first;
        r.healthOffset        = health->offset;
        r.healthConfirmations = health->confirmations;
        r.linkConfirmations   = best->second;
        r.bodyInstances       = c.instances;
        break;
    }
    if (!r.bodyVtable) {
        r.error = "geen kandidaat met zowel hitpoints als een Object-link";
        return r;
    }

    // --- indeling van DamageInfo meten ------------------------------------
    //
    // ActiveBody bevat m_lastDamageInfo als gewoon veld, en DamageInfo bestaat
    // uit drie stukken die elk van Snapshot erven en dus elk een vptr hebben:
    // DamageInfo zelf, dan 'in', dan 'out'. De afstand tussen de vptr van 'in'
    // en die van 'out' is precies sizeof(DamageInfoInput), en die verschilt
    // ondubbelzinnig tussen de twee spellen:
    //
    //   Generals   0x18   vptr, m_sourceID, m_sourcePlayerMask,
    //                     m_damageType, m_deathType, m_amount
    //   Zero Hour  0x40   idem plus m_sourceTemplate, m_damageStatusType,
    //                     m_damageFXOverride, m_kill en de shockwave-velden
    //
    // De vptrs staan er ook als een object nog nooit schade heeft gehad, want
    // de constructor zet ze. Meten kan dus meteen.
    {
        auto isVtable = [&](uint64_t v) { return v && counts.count(v) != 0; };
        std::map<uint32_t, uint32_t> sizeVotes;

        std::vector<uint64_t> insts = instancesOf(t, r.bodyVtable, 64, true);
        for (uint64_t self : insts) {
            const int32_t from = r.healthOffset + 16;
            for (int32_t off = from; off < from + 0x80; off += 4) {
                uint64_t a = 0, b = 0;
                if (!t.rptr((uint64_t)((int64_t)self + off), a)) break;
                if (!t.rptr((uint64_t)((int64_t)self + off + 4), b)) break;
                if (!isVtable(a) || !isVtable(b)) continue;

                // Gevonden: DamageInfo op off, 'in' op off+4. Nu 'out'.
                for (int32_t o2 = off + 8; o2 < off + 0x80; o2 += 4) {
                    uint64_t c = 0;
                    if (!t.rptr((uint64_t)((int64_t)self + o2), c)) break;
                    if (!isVtable(c)) continue;
                    sizeVotes[(uint32_t)(o2 - (off + 4))]++;
                    break;
                }
                break;
            }
        }

        if (!sizeVotes.empty()) {
            auto best = std::max_element(sizeVotes.begin(), sizeVotes.end(),
                                         [](const auto& x, const auto& y) {
                                             return x.second < y.second;
                                         });
            const uint32_t inSize = best->first;
            // m_damageType staat na vptr, m_sourceID, eventueel
            // m_sourceTemplate, en m_sourcePlayerMask.
            uint32_t inOffset = 0;
            if (inSize == 0x18)      inOffset = 0x0C;   // Generals
            else if (inSize == 0x40) inOffset = 0x10;   // Zero Hour

            if (inOffset && best->second >= 8) {
                r.damageInfoInSize = inSize;
                r.damageTypeOffset = 4 + inOffset;      // vanaf DamageInfo zelf
                say("DamageInfo: in-grootte 0x%x (%s), schadetype op +0x%x\n",
                    inSize, inSize == 0x18 ? "Generals" : "Zero Hour",
                    r.damageTypeOffset);
            } else {
                say("DamageInfo: in-grootte 0x%x herken ik niet; de hook "
                    "blokkeert straks alles.\n", inSize);
            }
        } else {
            say("DamageInfo: indeling niet te meten; de hook blokkeert "
                "straks alles.\n");
        }
    }

    uint64_t slot0 = 0;
    if (!t.rptr(r.bodyVtable, slot0) || !t.isExecutable(slot0)) {
        r.error = "slot 0 van de vtable wijst niet naar code";
        return r;
    }
    r.attemptDamage = (uintptr_t)slot0;

    say("ActiveBody-vtable 0x%llx, this->Object %d, hitpoints +0x%x\n",
        (unsigned long long)r.bodyVtable, (int)r.thisToObject,
        (unsigned)r.healthOffset);

    // --- ThePlayerList -----------------------------------------------------
    std::vector<PlayerListHit> pls = findPlayerLists(t);
    if (pls.empty()) {
        r.error = "ThePlayerList niet gevonden; zit je in een geladen potje?";
        return r;
    }
    const PlayerListHit& pl = pls[0];
    say("ThePlayerList: %u spelers, jij bent index %d\n",
        pl.playerCount, pl.localIndex);

    if (!findGlobalPointerNear(t, pl.addr, 0x40,
                               &r.playerListGlobal, &r.localPlayerOffset)) {
        r.error = "de globale pointer naar ThePlayerList is niet gevonden";
        return r;
    }
    say("ThePlayerList-globaal 0x%llx, m_local op +0x%x\n",
        (unsigned long long)r.playerListGlobal, (unsigned)r.localPlayerOffset);

    // --- eigenaarsketen ----------------------------------------------------
    std::vector<uint64_t> objects = instancesOf(t, r.objectVtable, 64, true);
    std::vector<OwnerChain> chains =
        findOwnerChains(t, objects, pl.players, pl.localPlayer, 0x400, 0x80, 0x200);
    if (chains.empty() || !chains[0].localSeen || chains[0].distinctPlayers < 2) {
        r.error = "geen eigenaarsketen die spreidt en bij jou uitkomt";
        return r;
    }
    r.objectToTeam   = chains[0].objectToTeam;
    r.teamToProto    = chains[0].teamToProto;
    r.protoToPlayer  = chains[0].protoToPlayer;
    r.chainPlayers   = chains[0].distinctPlayers;
    say("eigenaarsketen +0x%x / +0x%x / +0x%x, %u spelers\n",
        r.objectToTeam, r.teamToProto, r.protoToPlayer, r.chainPlayers);

    r.ok = true;
    return r;
}

} // namespace zp
