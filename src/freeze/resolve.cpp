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

// AsciiString en UnicodeString zijn allebei een enkele pointer naar een blok
// dat begint met twee shorts (refCount, numCharsAllocated); de tekens volgen
// daarachter. Lezen we onzin, dan geven we een lege string terug -- een naam
// is comfort, geen bewijs, en een verzonnen naam is erger dan geen naam.
std::string readGameString(const Target& t, uint64_t at, bool wide) {
    uint64_t data = 0;
    if (!t.rptr(at, data) || !data) return "";
    uint16_t refCount = 0, alloc = 0;
    if (!t.r16(data, refCount) || !t.r16(data + 2, alloc)) return "";
    if (refCount == 0 || refCount > 0x1000) return "";
    if (alloc == 0 || alloc > 512) return "";

    std::string out;
    for (uint32_t i = 0; i < alloc && i < 64; ++i) {
        uint32_t ch = 0;
        if (wide) {
            uint16_t w = 0;
            if (!t.r16(data + 4 + i * 2, w)) return "";
            ch = w;
        } else {
            uint8_t b = 0;
            if (!t.r8(data + 4 + i, b)) return "";
            ch = b;
        }
        if (ch == 0) return out;
        if (ch < 32 || ch > 126) return "";     // geen leesbare naam
        out += (char)ch;
    }
    return "";                                   // geen afsluitende nul
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
        r.objectToBody        = best->first.second;
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

    say("ActiveBody-vtable 0x%llx, this->Object %d, Object->body +0x%x, "
        "hitpoints +0x%x\n",
        (unsigned long long)r.bodyVtable, (int)r.thisToObject,
        (unsigned)r.objectToBody, (unsigned)r.healthOffset);

    // --- de overige body-klassen -------------------------------------------
    //
    // Er is niet een body-klasse maar zeven, elk met een eigen vtable:
    //
    //   ActiveBody
    //     StructureBody          (alle gebouwen)
    //       HiveStructureBody
    //     UndeadBody
    //     HighlanderBody
    //     ImmortalBody
    //   InactiveBody
    //
    // StructureBody en ImmortalBody overschrijven attemptDamage niet. In hun
    // vtable staat dus hetzelfde functie-adres als in die van ActiveBody, maar
    // het is wel een andere tabel. Een hook op slot 0 van de ene tabel raakt
    // de andere niet.
    //
    // Dat is de verklaring voor "werkt de ene keer wel en de andere keer
    // niet": de vorige versie hookte precies een tabel, namelijk de eerste
    // kandidaat die de poolnaam opleverde. Viel die keuze op ActiveBody, dan
    // waren je eenheden beschermd en je gebouwen niet; viel hij op
    // StructureBody, dan andersom.
    //
    // We zoeken ze niet op naam op. Dat hoeft niet, want we kennen de dubbele
    // link al: neem een Object, lees op +objectToBody zijn body-module, lees
    // daar de vptr, en controleer dat die module op thisToObject terugwijst
    // naar datzelfde Object. Wat daar uitkomt is per definitie een body-vtable
    // met het juiste subobject; er valt niets te gokken. Een naam hoort er dan
    // ook niet bij, behalve bij de ene vtable die de resolutie hierboven al op
    // naam heeft aangewezen -- zie derive.h.

    // Een ruime steekproef Objecten. Die dient hier twee doelen: de
    // body-klassen hieronder en straks de eigenaarsketen. Een keer scannen is
    // genoeg, en ruim bemonsteren is nodig -- gebouwen zijn met veel minder
    // dan eenheden, en als er geen enkel gebouw in de steekproef zit vinden we
    // StructureBody niet.
    std::vector<uint64_t> objects = instancesOf(t, r.objectVtable, 512, true);

    {
        std::vector<BodyVtableHit> hits =
            findBodyVtables(t, objects, r.objectToBody, r.thisToObject);

        // ActiveBody voorop, de rest op aantal bevestigingen. Voorop omdat
        // dat de klasse is die hierboven al op drie manieren bevestigd is;
        // raakt de lijst vol, dan valt die er niet als eerste af.
        std::stable_sort(hits.begin(), hits.end(),
                         [&](const BodyVtableHit& x, const BodyVtableHit& y) {
                             return (x.vtable == r.bodyVtable) >
                                    (y.vtable == r.bodyVtable);
                         });

        say("body-klassen met een sluitende dubbele link: %zu\n", hits.size());
        for (const BodyVtableHit& h : hits) {
            if (r.bodies.size() >= ZP_MAX_BODY_VTABLES) {
                say("  (meer dan %u; de rest blijft ongehookt)\n",
                    (unsigned)ZP_MAX_BODY_VTABLES);
                break;
            }
            // Twee bevestigingen is al zo goed als onvervalsbaar: het adres
            // moet heen en terug kloppen. Een enkele treffer kan nog een
            // restant van een vrijgegeven object zijn, dus daar houden we op.
            if (h.confirmations < 2 && h.vtable != r.bodyVtable) {
                say("  0x%-8llx  %u bevestiging, te weinig\n",
                    (unsigned long long)h.vtable, h.confirmations);
                continue;
            }
            BodyVtable b;
            b.vtable        = (uintptr_t)h.vtable;
            b.attemptDamage = (uintptr_t)h.slot0;
            b.confirmations = h.confirmations;
            b.nameConfirmed = h.vtable == r.bodyVtable;
            auto it = counts.find(h.vtable);
            b.instances     = it == counts.end() ? 0 : (uint32_t)it->second;
            say("  0x%-8llx  %4u bevestigingen, %5u instanties, "
                "slot 0 -> 0x%llx%s\n",
                (unsigned long long)b.vtable, b.confirmations, b.instances,
                (unsigned long long)b.attemptDamage,
                b.nameConfirmed ? "  <-- ActiveBody, op naam bevestigd" : "");
            r.bodies.push_back(b);
        }

        // ActiveBody hoort er altijd bij: die is hierboven al op drie manieren
        // bevestigd. Stond hij niet in de steekproef, dan alsnog vooraan.
        bool hasPrimary = false;
        for (const BodyVtable& b : r.bodies)
            if (b.vtable == r.bodyVtable) hasPrimary = true;
        if (!hasPrimary) {
            BodyVtable b;
            b.vtable        = r.bodyVtable;
            b.attemptDamage = r.attemptDamage;
            b.confirmations = r.linkConfirmations;
            b.instances     = (uint32_t)r.bodyInstances;
            b.nameConfirmed = true;
            r.bodies.insert(r.bodies.begin(), b);
            if (r.bodies.size() > ZP_MAX_BODY_VTABLES) r.bodies.resize(ZP_MAX_BODY_VTABLES);
        }
    }

    // --- ThePlayerList -----------------------------------------------------
    std::vector<PlayerListHit> pls = findPlayerLists(t);
    if (pls.empty()) {
        r.error = "ThePlayerList niet gevonden; zit je in een geladen potje?";
        return r;
    }
    const PlayerListHit& pl = pls[0];
    say("ThePlayerList: %u spelers, jij bent index %d\n",
        pl.playerCount, pl.localIndex);

    r.playerCount         = pl.playerCount;
    r.localIndex          = pl.localIndex;
    r.localPlayerAtResolve = (uintptr_t)pl.localPlayer;
    for (size_t i = 0; i < pl.players.size() && i < 16; ++i)
        r.players[i] = (uintptr_t)pl.players[i];

    // --- Player::m_playerIndex ---------------------------------------------
    //
    // PlayerList::PlayerList() doet:
    //
    //   for (Int i = 0; i < MAX_PLAYER_COUNT; i++)
    //       m_players[i] = NEW Player( i );
    //
    // en Player::Player(Int playerIndex) zet m_playerIndex = playerIndex. Er
    // is dus precies een offset waar bij alle zestien spelers hun eigen
    // positie in de array staat. Zestien keer achter elkaar kloppen is geen
    // toeval, dus deze offset is bewezen in plaats van aangenomen.
    //
    // Waarom we hem willen: zonder dit zegt het log alleen dat twee adressen
    // niet gelijk zijn. Met dit zegt het welke speler de eigenaar is en welke
    // jij bent, en dat is het verschil tussen een raadsel en een diagnose.
    {
        size_t n = pl.players.size();
        if (n > 16) n = 16;
        for (uint32_t off = 4; off < 0x80 && !r.playerIndexOffset; off += 4) {
            size_t ok = 0;
            for (size_t i = 0; i < n; ++i) {
                uint32_t v = 0;
                if (!t.r32((uint64_t)r.players[i] + off, v)) break;
                if (v != (uint32_t)i) break;
                ok++;
            }
            if (ok == n && n >= 8) r.playerIndexOffset = off;
        }
    }
    if (r.playerIndexOffset) {
        say("Player::m_playerIndex op +0x%x (zestien van de zestien kloppen)\n",
            r.playerIndexOffset);
    } else {
        say("Player::m_playerIndex niet gevonden; het log kan straks geen "
            "spelernummers noemen\n");
    }
    // --- de rest van Player, afgeleid uit m_playerIndex -------------------
    if (r.playerIndexOffset >= 0x24) {
        const uint32_t base = r.playerIndexOffset;

        // m_playerType: alle zestien moeten 0 of 1 zijn, en er hoort er
        // precies een HUMAN te zijn. Klopt dat niet, dan gebruiken we hem
        // niet -- dan is de offset blijkbaar toch niet wat we denken.
        const uint32_t typeOff = base + 0xC;
        uint32_t humans = 0, bad = 0;
        int32_t  human = -1;
        for (size_t i = 0; i < pl.players.size() && i < 16; ++i) {
            uint32_t v = 0;
            if (!t.r32((uint64_t)r.players[i] + typeOff, v) || v > 1) { bad++; continue; }
            if (v == 0) { humans++; human = (int32_t)i; }
        }
        if (!bad && humans == 1) {
            r.playerTypeOffset = typeOff;
            r.humanIndex = human;
            r.humanPlayer = r.players[human];
        }

        r.playerNameOffset        = base - 0x8;    // m_playerName
        r.playerDisplayNameOffset = base - 0x1C;   // m_playerDisplayName
    }

    for (size_t i = 0; i < pl.players.size() && i < 16; ++i) {
        std::string name, display;
        if (r.playerNameOffset)
            name = readGameString(t, (uint64_t)r.players[i] + r.playerNameOffset, false);
        if (r.playerDisplayNameOffset)
            display = readGameString(
                t, (uint64_t)r.players[i] + r.playerDisplayNameOffset, true);

        const char* kind = "";
        if (r.playerTypeOffset) {
            uint32_t v = 1;
            t.r32((uint64_t)r.players[i] + r.playerTypeOffset, v);
            kind = v == 0 ? "MENS" : "cpu ";
        }

        say("  speler %2zu  0x%08llx  %s  %-14s %-16s%s%s\n", i,
            (unsigned long long)r.players[i], kind,
            name.empty() ? "-" : name.c_str(),
            display.empty() ? "-" : display.c_str(),
            (int)i == pl.localIndex ? "  <-- m_local" : "",
            (int32_t)i == r.humanIndex ? "  <-- de mens" : "");
    }

    // Als de engine iets anders zegt dan de enige menselijke speler, dan is
    // dat het vermelden waard: het is precies het verschil waardoor de
    // bescherming op de verkeerde speler terecht kan komen.
    if (r.humanIndex >= 0 && r.humanIndex != pl.localIndex) {
        say("LET OP: m_local wijst naar speler %d, maar de enige menselijke\n",
            pl.localIndex);
        say("        speler is %d. We beschermen speler %d.\n",
            r.humanIndex, r.humanIndex);
    } else if (r.humanIndex < 0) {
        say("Geen eenduidige menselijke speler gevonden; we houden m_local aan.\n");
    }

    if (!findGlobalPointerNear(t, pl.addr, 0x40,
                               &r.playerListGlobal, &r.localPlayerOffset)) {
        r.error = "de globale pointer naar ThePlayerList is niet gevonden";
        return r;
    }
    say("ThePlayerList-globaal 0x%llx, m_local op +0x%x\n",
        (unsigned long long)r.playerListGlobal, (unsigned)r.localPlayerOffset);

    // --- eigenaarsketen ----------------------------------------------------
    // Ruimer bemonsteren dan de 64 van eerst. De keten moet bij jou uitkomen
    // en over meer dan een speler spreiden, en dat lukt niet als er van jouw
    // eenheden toevallig niets in de steekproef zit. De extra kosten vallen
    // mee: de dure lussen draaien alleen op offsets waar echt een object
    // staat.
    std::vector<OwnerChain> chains =
        findOwnerChains(t, objects, pl.players, pl.localPlayer, 0x400, 0x80, 0x200);
    if (chains.empty() || !chains[0].localSeen || chains[0].distinctPlayers < 2) {
        // Zeggen wat er wel lukte. "Mislukt" zonder meer maakt van de
        // volgende poging weer een gok; dit maakt er een meting van.
        say("eigenaarsketen: %u objecten bemonsterd, %u kandidaat-ketens\n",
            (unsigned)objects.size(), (unsigned)chains.size());
        if (!chains.empty()) {
            say("beste keten +0x%x / +0x%x / +0x%x: %u spelers, jij %s\n",
                chains[0].objectToTeam, chains[0].teamToProto,
                chains[0].protoToPlayer, chains[0].distinctPlayers,
                chains[0].localSeen ? "erbij" : "er niet bij");
        }
        if (!chains.empty() && chains[0].localSeen)
            r.error = "de eigenaarsketen komt wel bij jou uit maar spreidt niet; "
                      "staan er al vijandelijke eenheden op de kaart?";
        else if (!chains.empty())
            r.error = "er is wel een eigenaarsketen maar geen enkel object "
                      "komt bij jou uit; heb je zelf al eenheden?";
        else
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
