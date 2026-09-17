// zp-probe - leest een draaiende Generals / Zero Hour uit en rapporteert
// alles wat nodig is om de health-freeze betrouwbaar te kunnen inbouwen.
//
// De probe schrijft alleen; hij verandert niets aan het spel. Er wordt geen
// geheugen geschreven, geen code gepatcht en geen DLL geinjecteerd.
//
// Gebruik:
//   zp-probe.exe                 zoekt het spel zelf
//   zp-probe.exe --list          toont alle processen
//   zp-probe.exe --pid 1234      koppelt aan een specifiek proces
//   zp-probe.exe --out rap.txt   schrijft het rapport ergens anders heen

#include "../common/target.h"
#include "../common/rtti.h"
#include "../common/derive.h"
#include "../common/sha256.h"

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <string>
#include <vector>


using namespace zp;

// ---------------------------------------------------------------- rapport --

static FILE*       g_out = nullptr;
static std::string g_outPath;

static void say(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    if (g_out) fputs(buf, g_out);
}

static void rule(const char* title) {
    say("\n============================================================\n");
    say("  %s\n", title);
    say("============================================================\n");
}

// ------------------------------------------------------------ hulpmiddelen --

static std::string fileVersionOf(const std::string& path) {
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeA(path.c_str(), &dummy);
    if (!sz) return "(geen versie-informatie)";
    std::vector<uint8_t> buf(sz);
    if (!GetFileVersionInfoA(path.c_str(), 0, sz, buf.data()))
        return "(geen versie-informatie)";
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueA(buf.data(), "\\", (LPVOID*)&ffi, &len) || !ffi)
        return "(geen versie-informatie)";
    char out[128];
    snprintf(out, sizeof(out), "%u.%u.%u.%u",
             HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
             HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return out;
}

// PE-timestamp uit de headers in het geheugen: de sterkste build-identificatie.
static bool peStamp(const Target& t, uint32_t* stamp, uint16_t* machine) {
    uint16_t mz = 0;
    if (!t.r16(t.imageBase(), mz) || mz != 0x5A4D) return false;
    uint32_t e_lfanew = 0;
    if (!t.r32(t.imageBase() + 0x3C, e_lfanew)) return false;
    uint64_t nt = t.imageBase() + e_lfanew;
    uint32_t sig = 0;
    if (!t.r32(nt, sig) || sig != 0x00004550) return false;
    if (!t.r16(nt + 4, *machine)) return false;
    if (!t.r32(nt + 8, *stamp)) return false;
    return true;
}

// Signed offsets leesbaar weergeven: "+0x28" of "-0x1c".
static std::string soff(int32_t v) {
    char b[32];
    snprintf(b, sizeof(b), "%c0x%x", v < 0 ? '-' : '+', (unsigned)(v < 0 ? -v : v));
    return b;
}

static std::string rva(const Target& t, uint64_t addr) {
    char b[64];
    if (t.inImage(addr))
        snprintf(b, sizeof(b), "%s+0x%llx", "image",
                 (unsigned long long)(addr - t.imageBase()));
    else
        snprintf(b, sizeof(b), "(buiten image)");
    return b;
}

// ------------------------------------------------------------------- main --

namespace zp { int runSelfTest(); }

struct Args {
    bool        selftest = false;
    DWORD       pid = 0;
    std::string name;
    std::string out = "zeropatience-probe.txt";
    uint64_t    maxSnapshotMB = 2048;
    bool        list = false;
};

static bool parseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { printf("ontbrekende waarde na %s\n", what); exit(2); }
            return argv[++i];
        };
        if (s == "--selftest")          a.selftest = true;
        else if (s == "--list")         a.list = true;
        else if (s == "--pid")          a.pid = (DWORD)strtoul(next("--pid"), nullptr, 0);
        else if (s == "--name")         a.name = next("--name");
        else if (s == "--out")          a.out = next("--out");
        else if (s == "--max-snapshot") a.maxSnapshotMB = strtoull(next("--max-snapshot"), nullptr, 0);
        else if (s == "--help" || s == "-h") return false;
        else { printf("onbekende optie: %s\n", s.c_str()); return false; }
    }
    return true;
}

static void usage() {
    printf(
        "zp-probe - leest Generals / Zero Hour uit voor de health-freeze trainer\n"
        "\n"
        "  --selftest             controleer de afleidingslogica en stop\n"
        "  --list                 toon alle draaiende processen en stop\n"
        "  --pid <n>              koppel aan dit proces-id\n"
        "  --name <exe>           koppel aan het eerste proces met deze naam\n"
        "  --out <bestand>        rapportbestand (standaard zeropatience-probe.txt)\n"
        "  --max-snapshot <MB>    geheugenbudget voor de snapshot (standaard 2048)\n"
        "\n"
        "De probe schrijft niets naar het spel. Start hem terwijl je in een\n"
        "skirmish zit met een paar eigen units en gebouwen op de kaart.\n");
}

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) { usage(); return 2; }

    if (args.selftest) return zp::runSelfTest();

    std::vector<ProcEntry> procs = listProcesses();

    if (args.list) {
        printf("%-8s %s\n", "PID", "NAAM");
        for (const auto& p : procs)
            printf("%-8lu %s%s\n", (unsigned long)p.pid, p.name.c_str(),
                   looksLikeGenerals(p.name) ? "   <-- ziet eruit als Generals" : "");
        return 0;
    }

    DWORD pid = args.pid;
    if (!pid) {
        std::vector<ProcEntry> matches;
        for (const auto& p : procs) {
            if (!args.name.empty()) {
                if (_stricmp(p.name.c_str(), args.name.c_str()) == 0) matches.push_back(p);
            } else if (looksLikeGenerals(p.name)) {
                matches.push_back(p);
            }
        }
        if (matches.empty()) {
            printf("Geen Generals-proces gevonden.\n\n"
                   "Start het spel, laad een skirmish, en draai dan opnieuw.\n"
                   "Vindt hij het nog steeds niet, draai dan 'zp-probe.exe --list',\n"
                   "zoek het spel op in de lijst en gebruik '--pid <nummer>'.\n");
            return 1;
        }
        if (matches.size() > 1) {
            printf("Meerdere kandidaten gevonden, kies er een met --pid:\n");
            for (const auto& m : matches)
                printf("  --pid %-8lu %s\n", (unsigned long)m.pid, m.name.c_str());
            return 1;
        }
        pid = matches[0].pid;
        printf("Gevonden: %s (pid %lu)\n", matches[0].name.c_str(), (unsigned long)pid);
    }

    Target t;
    std::string err;
    if (!t.attach(pid, &err)) {
        printf("Koppelen mislukt: %s\n", err.c_str());
        return 1;
    }

    g_out = fopen(args.out.c_str(), "wb");
    if (!g_out) printf("Waarschuwing: kan %s niet schrijven, alleen schermuitvoer.\n",
                       args.out.c_str());
    g_outPath = args.out;

    // --------------------------------------------------------- vingerafdruk --
    rule("BUILD-VINGERAFDRUK");
    {
        time_t now = time(nullptr);
        char ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
        say("rapport        : %s\n", ts);
        say("pid            : %lu\n", (unsigned long)pid);
        say("pad            : %s\n", t.imagePath().c_str());
        say("architectuur   : %s\n", t.is64() ? "64-bit" : "32-bit");
        say("image base     : 0x%llx\n", (unsigned long long)t.imageBase());
        say("image grootte  : 0x%llx (%llu MB)\n",
            (unsigned long long)t.imageSize(),
            (unsigned long long)(t.imageSize() / (1024 * 1024)));
        say("bestandsversie : %s\n", fileVersionOf(t.imagePath()).c_str());

        uint32_t stamp = 0;
        uint16_t machine = 0;
        if (peStamp(t, &stamp, &machine)) {
            time_t st = (time_t)stamp;
            char sb[64];
            strftime(sb, sizeof(sb), "%Y-%m-%d %H:%M:%S UTC", gmtime(&st));
            say("PE timestamp   : 0x%08x (%s)\n", stamp, sb);
            say("PE machine     : 0x%04x (%s)\n", machine,
                machine == 0x014c ? "x86" : machine == 0x8664 ? "x64" : "onbekend");
        }

        uint64_t fsz = 0;
        std::string hash = sha256File(t.imagePath(), &fsz);
        if (!hash.empty()) {
            say("bestandsgrootte: %llu bytes\n", (unsigned long long)fsz);
            say("sha256         : %s\n", hash.c_str());
        } else {
            say("sha256         : (kon het bestand niet lezen)\n");
        }
    }

    // ------------------------------------------------------------- snapshot --
    printf("\nGeheugen kopieren (budget %llu MB)...\n",
           (unsigned long long)args.maxSnapshotMB);
    t.buildSnapshot(args.maxSnapshotMB * 1024ull * 1024ull, /*includeMappedFiles=*/false);
    rule("GEHEUGEN");
    say("regio's totaal : %zu\n", t.regions().size());
    say("gekopieerd     : %zu regio's, %llu MB\n", t.snapshotCount(),
        (unsigned long long)(t.snapshotBytes() / (1024 * 1024)));
    if (t.snapshotBytes() == 0) {
        say("\nEr is niets gekopieerd. Draai de probe als administrator.\n");
        if (g_out) fclose(g_out);
        return 1;
    }

    // ----------------------------------------------------------------- RTTI --
    rule("RTTI-SCAN");
    static const std::vector<std::string> kClasses = {
        "ActiveBody", "BodyModule", "BodyModuleInterface", "InactiveBody",
        "StructureBody", "HighlanderBody", "ImmortalBody", "UndeadBody",
        "Object", "Drawable", "Team", "TeamPrototype", "Player", "PlayerList",
        "GameLogic", "ObjectModule", "BehaviorModule",
    };
    std::vector<VtableInfo> vts = scanRtti(t, kClasses);

    uint64_t activeBodyPrimary = 0;   // vtable van het ActiveBody-subobject zelf
    uint64_t activeBodyIface   = 0;   // vtable van het BodyModuleInterface-subobject
    uint32_t activeBodyIfaceOff = 0;
    uint64_t objectVtable      = 0;

    if (vts.empty()) {
        say("Geen RTTI gevonden. Dat betekent bijna altijd dat deze build zonder\n"
            "/GR is gecompileerd. Geen probleem: de heuristische secties hieronder\n"
            "werken zonder RTTI en leveren dezelfde informatie.\n");
    } else {
        say("%-22s %-10s %-12s %-12s %s\n",
            "KLASSE", "SUBOBJ", "VTABLE", "RVA", "SLOTS");
        for (const VtableInfo& v : vts) {
            say("%-22s +0x%-7x 0x%-10llx %-12s %u\n",
                v.cls.c_str(), v.subobjectOffset,
                (unsigned long long)v.vtable, rva(t, v.vtable).c_str(), v.slotCount);
            if (v.cls == "ActiveBody") {
                if (v.subobjectOffset == 0) activeBodyPrimary = v.vtable;
                else if (!activeBodyIface) {
                    activeBodyIface = v.vtable;
                    activeBodyIfaceOff = v.subobjectOffset;
                }
            }
            if (v.cls == "Object" && v.subobjectOffset == 0 && !objectVtable)
                objectVtable = v.vtable;
        }

        // De vtable van het BodyModuleInterface-subobject is waar attemptDamage
        // in staat. Slot 0, want het is de eerste virtuele die die interface
        // declareert en er is geen virtuele destructor.
        if (activeBodyIface) {
            rule("ActiveBody :: BodyModuleInterface-vtable");
            say("vtable          : 0x%llx  (%s)\n",
                (unsigned long long)activeBodyIface, rva(t, activeBodyIface).c_str());
            say("subobject-offset: +0x%x\n", activeBodyIfaceOff);
            if (activeBodyPrimary)
                say("primaire vtable : 0x%llx  (%s)\n",
                    (unsigned long long)activeBodyPrimary,
                    rva(t, activeBodyPrimary).c_str());
            say("\nVerwachte volgorde uit de broncode:\n"
                "  slot 0  attemptDamage      <-- dit is het doelwit\n"
                "  slot 1  attemptHealing\n"
                "  slot 2  estimateDamage\n"
                "  slot 3  getHealth\n"
                "  slot 4  getMaxHealth\n\n");
            std::vector<uint64_t> slots = readVtable(t, activeBodyIface, 48);
            for (size_t i = 0; i < slots.size(); ++i)
                say("  [%2zu] 0x%-10llx  %s\n", i,
                    (unsigned long long)slots[i], rva(t, slots[i]).c_str());
        }
    }

    // ----------------------------------------------------------- PlayerList --
    rule("THEPLAYERLIST");
    std::vector<PlayerListHit> pls = findPlayerLists(t);
    std::vector<uint64_t> knownPlayers;
    if (pls.empty()) {
        say("Niet gevonden. Zit je wel in een geladen skirmish (niet in het menu)?\n");
    } else {
        say("%zu kandidaat(en); de eerste met een geldige lokale speler telt.\n\n",
            pls.size());
        for (size_t i = 0; i < pls.size() && i < 8; ++i) {
            const PlayerListHit& h = pls[i];
            say("[%zu] m_local @ 0x%llx   spelers=%u   lokale index=%d   "
                "array op +0x%x\n",
                i, (unsigned long long)h.addr, h.playerCount, h.localIndex,
                h.arrayOffset);
            say("    m_local        = 0x%llx\n", (unsigned long long)h.localPlayer);
            say("    Player-vtable  = 0x%llx (%s)\n",
                (unsigned long long)h.playerVtable, rva(t, h.playerVtable).c_str());
            for (size_t k = 0; k < h.players.size(); ++k)
                say("    m_players[%2zu]  = 0x%llx%s\n", k,
                    (unsigned long long)h.players[k],
                    h.players[k] == h.localPlayer ? "   <-- jij" : "");
            say("\n");
        }
        knownPlayers = pls[0].players;
        say("Let op: het adres hierboven ligt op de heap en verschilt per potje.\n"
            "De DLL zoekt dit patroon zelf opnieuw op bij het injecteren; het\n"
            "staat hier alleen ter bevestiging dat het patroon klopt.\n");
    }

    // ------------------------------------------------------------ histogram --
    rule("VTABLE-HISTOGRAM (werkt zonder RTTI)");
    std::vector<VtableCount> hist = vtableHistogram(t, 30);
    say("%-14s %-16s %s\n", "VTABLE", "RVA", "INSTANTIES");
    for (const VtableCount& v : hist) {
        std::string tag;
        for (const VtableInfo& info : vts)
            if (info.vtable == v.vtable) {
                tag = "  <- " + info.cls;
                if (info.subobjectOffset) tag += " (+0x" +
                    std::to_string(info.subobjectOffset) + ")";
                break;
            }
        say("0x%-12llx %-16s %llu%s\n", (unsigned long long)v.vtable,
            rva(t, v.vtable).c_str(), (unsigned long long)v.count, tag.c_str());
    }

    // ---------------------------------------------------------- dubbele link --
    rule("DUBBELE LINKS (Object <-> BodyModule)");
    std::vector<LinkPair> links;
    {
        std::vector<uint64_t> cand;
        for (const VtableCount& v : hist) cand.push_back(v.vtable);
        if (activeBodyIface) cand.push_back(activeBodyIface);
        if (objectVtable)    cand.push_back(objectVtable);
        std::sort(cand.begin(), cand.end());
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

        say("Zoeken in %zu vtables, 24 instanties per vtable.\n"
            "Offsets zijn signed en gelden vanaf het adres waar de vptr staat.\n\n",
            cand.size());
        links = findDoubleLinks(t, cand, 24, 0x400, 0x200);
        if (links.empty()) {
            say("Geen wederzijdse verwijzingen gevonden.\n");
        } else {
            say("%-14s %-10s %-14s %-10s %s\n",
                "A (vtable)", "A->B", "B (vtable)", "B->A", "BEVESTIGD");
            for (size_t i = 0; i < links.size() && i < 40; ++i) {
                const LinkPair& p = links[i];
                say("0x%-12llx %-10s 0x%-12llx %-10s %u\n",
                    (unsigned long long)p.vtableA, soff(p.offsetAtoB).c_str(),
                    (unsigned long long)p.vtableB, soff(p.offsetBtoA).c_str(),
                    p.confirmations);
            }
        }
    }

    // ------------------------------------------------------------- resolutie --
    //
    // Welke van de twee kanten van een dubbele link is de body-module en welke
    // het Object? Dat beslissen we niet op naam maar op inhoud: alleen de
    // body-module bevat vier opeenvolgende floats die zich als hitpoints
    // gedragen. Daardoor werkt dit ook op een build zonder RTTI.
    rule("RESOLUTIE");

    uint64_t resolvedBodyVtable   = 0;
    uint64_t resolvedObjectVtable = 0;
    int32_t  resolvedThisToObject = 0;   // this -> Object      (vaak negatief)
    int32_t  resolvedObjectToBody = 0;   // Object::m_body
    int32_t  resolvedHealthOffset = 0;   // this -> m_currentHealth
    uint32_t resolvedHealthConf   = 0;
    const char* resolvedVia = "niets";

    {
        auto healthScore = [&](uint64_t vtable, int32_t* bestOff) -> uint32_t {
            std::vector<uint64_t> insts = instancesOf(t, vtable, 64);
            if (insts.empty()) return 0;
            std::vector<HealthBlock> hb = findHealthBlocks(t, insts, -0x200, 0x200);
            if (hb.empty()) return 0;
            if (bestOff) *bestOff = hb[0].offset;
            return hb[0].confirmations;
        };

        for (size_t i = 0; i < links.size() && i < 24 && !resolvedBodyVtable; ++i) {
            const LinkPair& p = links[i];
            if (p.confirmations < 4) break;   // te weinig bewijs

            int32_t offA = 0, offB = 0;
            uint32_t scoreA = healthScore(p.vtableA, &offA);
            uint32_t scoreB = healthScore(p.vtableB, &offB);
            if (scoreA == 0 && scoreB == 0) continue;

            if (scoreA >= scoreB) {
                // A is de body-module, B het Object.
                resolvedBodyVtable   = p.vtableA;
                resolvedObjectVtable = p.vtableB;
                resolvedThisToObject = p.offsetAtoB;
                resolvedObjectToBody = p.offsetBtoA;
                resolvedHealthOffset = offA;
                resolvedHealthConf   = scoreA;
            } else {
                resolvedBodyVtable   = p.vtableB;
                resolvedObjectVtable = p.vtableA;
                resolvedThisToObject = p.offsetBtoA;
                resolvedObjectToBody = p.offsetAtoB;
                resolvedHealthOffset = offB;
                resolvedHealthConf   = scoreB;
            }
            resolvedVia = "dubbele link + hitpoint-patroon";
        }

        // Als RTTI beschikbaar was, is dat het sterkere bewijs. Bij verschil
        // melden we dat expliciet in plaats van er stilzwijgend een te kiezen.
        if (activeBodyIface) {
            if (resolvedBodyVtable && resolvedBodyVtable != activeBodyIface) {
                say("LET OP: RTTI wijst ActiveBody aan op 0x%llx, de heuristiek op\n"
                    "0x%llx. RTTI wint, maar controleer dit handmatig.\n\n",
                    (unsigned long long)activeBodyIface,
                    (unsigned long long)resolvedBodyVtable);
            }
            resolvedBodyVtable = activeBodyIface;
            resolvedVia = resolvedVia[0] == 'n' ? "RTTI" : "RTTI (bevestigd door heuristiek)";
            if (objectVtable) resolvedObjectVtable = objectVtable;
            int32_t off = 0;
            uint32_t conf = healthScore(activeBodyIface, &off);
            if (conf) { resolvedHealthOffset = off; resolvedHealthConf = conf; }
        }
    }

    if (!resolvedBodyVtable) {
        say("De body-module is niet vastgesteld.\n\n"
            "Meest waarschijnlijke oorzaak: de probe draaide terwijl je in een\n"
            "menu zat in plaats van in een geladen potje. Laad een skirmish,\n"
            "bouw een paar units, en draai opnieuw.\n");
    } else {
        say("vastgesteld via   : %s\n\n", resolvedVia);
        say("ActiveBody-vtable : 0x%llx  (%s)\n",
            (unsigned long long)resolvedBodyVtable, rva(t, resolvedBodyVtable).c_str());
        say("Object-vtable     : 0x%llx  (%s)\n",
            (unsigned long long)resolvedObjectVtable, rva(t, resolvedObjectVtable).c_str());
        say("\nOffsets vanaf 'this', de pointer die de detour binnenkrijgt:\n");
        say("  this -> Object          %s\n", soff(resolvedThisToObject).c_str());
        say("  this -> m_currentHealth %s   (%u bevestigingen)\n",
            soff(resolvedHealthOffset).c_str(), resolvedHealthConf);
        say("\nOffset vanaf het begin van Object:\n");
        say("  Object::m_body          %s\n", soff(resolvedObjectToBody).c_str());
        if (resolvedThisToObject < 0)
            say("\nDe negatieve offset naar Object is verwacht: het\n"
                "BodyModuleInterface-subobject zit achteraan in ActiveBody.\n");
    }

    // ------------------------------------------------- ActiveBody-instanties --
    std::vector<uint64_t> bodyInstances;
    if (resolvedBodyVtable) {
        bodyInstances = instancesOf(t, resolvedBodyVtable, 4096);

        rule("GEZONDHEIDSVELDEN IN ACTIVEBODY");
        say("gevonden: %zu body-instanties\n\n", bodyInstances.size());
        say("Gezocht naar vier opeenvolgende floats die zich gedragen als\n"
            "m_currentHealth / m_prevHealth / m_maxHealth / m_initialHealth.\n"
            "Offsets zijn relatief aan 'this'.\n\n");
        std::vector<HealthBlock> hb = findHealthBlocks(t, bodyInstances, -0x200, 0x200);
        if (hb.empty()) {
            say("Niets gevonden. Staan er wel units op de kaart?\n");
        } else {
            say("%-12s %-14s %-12s %s\n", "OFFSET", "BEVESTIGD", "current", "max");
            for (const HealthBlock& b : hb)
                say("%-12s %-14u %-12.1f %.1f\n",
                    soff(b.offset).c_str(), b.confirmations,
                    b.sampleCurrent, b.sampleMax);
        }

        rule("HEXDUMP VAN DRIE ACTIVEBODY-INSTANTIES");
        say("Ruwe bytes rond 'this', zodat de resterende velden (met name\n"
            "m_indestructible) handmatig na te lopen zijn.\n");
        for (size_t i = 0; i < bodyInstances.size() && i < 3; ++i) {
            say("\n-- instantie %zu: this = 0x%llx --\n", i,
                (unsigned long long)bodyInstances[i]);
            say("%s", hexDump(t, bodyInstances[i], -0x100, 0x80).c_str());
        }
    }

    // ----------------------------------------------------- eigenaarsketen ---
    rule("EIGENAARSKETEN Object -> Team -> TeamPrototype -> Player");
    if (knownPlayers.empty()) {
        say("Overgeslagen: ThePlayerList is niet gevonden, dus er is geen\n"
            "bekend eindpunt om de keten tegen te valideren.\n");
    } else if (!resolvedObjectVtable) {
        say("Overgeslagen: de Object-vtable is niet vastgesteld.\n");
    } else {
        std::vector<uint64_t> objects = instancesOf(t, resolvedObjectVtable, 64);
        say("Object-vtable 0x%llx, %zu instanties doorlopen.\n",
            (unsigned long long)resolvedObjectVtable, objects.size());
        if (objects.empty()) {
            say("Geen instanties om te doorlopen.\n");
        } else {
            std::vector<OwnerChain> chains =
                findOwnerChains(t, objects, knownPlayers, 0x400, 0x80, 0x200);
            if (chains.empty()) {
                say("Geen keten gevonden die uitkomt op een bekende Player.\n");
            } else {
                say("\n%-16s %-16s %-22s %s\n",
                    "Object::m_team", "Team::m_proto", "Proto::m_owningPlayer",
                    "BEVESTIGD");
                for (const OwnerChain& c : chains)
                    say("+0x%-13x +0x%-13x +0x%-19x %u\n",
                        c.objectToTeam, c.teamToProto, c.protoToPlayer,
                        c.confirmations);
                say("\nDe bovenste regel is de keten die de hook gaat gebruiken.\n");
            }
        }
    }

    // ------------------------------------------------------------ afsluiting --
    rule("KLAAR");
    say("Rapport geschreven naar: %s\n", args.out.c_str());
    say("\nStuur dit bestand terug. Daarmee staan de offsets vast en kan de\n"
        "DLL met de damage-hook gebouwd worden.\n");

    if (g_out) fclose(g_out);
    printf("\nRapport: %s\n", args.out.c_str());
    return 0;
}
