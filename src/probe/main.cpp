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
#include <map>
#include <set>
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
            // Bij de EA-uitgave draait er naast de engine ook een launcher
            // (Generals.exe start Game.dat). Alleen de engine houdt honderden
            // MB aan objecten vast, dus daar is hij aan te herkennen.
            std::vector<std::pair<uint64_t, ProcEntry>> ranked;
            for (const auto& m : matches)
                ranked.push_back({privateCommitBytes(m.pid), m});
            std::sort(ranked.begin(), ranked.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

            printf("Meerdere kandidaten gevonden:\n");
            for (const auto& r : ranked)
                printf("  --pid %-8lu %-16s %6llu MB geheugen\n",
                       (unsigned long)r.second.pid, r.second.name.c_str(),
                       (unsigned long long)(r.first / (1024 * 1024)));

            const uint64_t big = ranked[0].first;
            const uint64_t next = ranked[1].first;
            if (big == 0 || (next && big < next * 2)) {
                printf("\nDe kandidaten liggen te dicht bij elkaar om automatisch te\n"
                       "kiezen. Kies er zelf een met --pid.\n");
                return 1;
            }
            pid = ranked[0].second.pid;
            printf("\nGekozen: %s (pid %lu). Dat is de engine; de andere is de\n"
                   "launcher. Overrulen kan met --pid.\n\n",
                   ranked[0].second.name.c_str(), (unsigned long)pid);
        } else {
            pid = matches[0].pid;
        }
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
    std::map<uint64_t, uint64_t> counts = vtableCounts(t);
    std::vector<VtableCount> allVtables;
    for (auto& kv : counts) allVtables.push_back({kv.first, kv.second});
    std::sort(allVtables.begin(), allVtables.end(),
              [](const VtableCount& a, const VtableCount& b) { return a.count > b.count; });

    std::vector<VtableCount> hist(allVtables.begin(),
                                  allVtables.begin() + std::min<size_t>(40, allVtables.size()));
    say("%zu vtables met instanties in totaal; de veertig grootste hieronder.\n"
        "De rest wordt niet weggegooid: de zoektocht hieronder gebruikt alles\n"
        "met minstens 32 instanties. Een top-40 volstaat niet, want de meest\n"
        "voorkomende klassen zijn kleine talrijke objecten en de klasse die we\n"
        "zoeken valt daar makkelijk buiten.\n\n", allVtables.size());
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

    // ------------------------------------------------------- vptr-groepen --
    //
    // Een klasse met meervoudige overerving heeft meerdere vptrs, en die
    // komen per definitie even vaak voor. Vtables met een identiek aantal
    // instanties horen dus waarschijnlijk bij hetzelfde object. ActiveBody is
    // zo'n klasse, dus dit geeft houvast over welke adressen bij elkaar horen.
    rule("VPTR-GROEPEN (zelfde aantal instanties = zelfde klasse)");
    {
        std::map<uint64_t, std::vector<uint64_t>> byCount;
        for (const VtableCount& v : hist) byCount[v.count].push_back(v.vtable);
        bool any = false;
        for (auto it = byCount.rbegin(); it != byCount.rend(); ++it) {
            if (it->second.size() < 2) continue;
            any = true;
            say("%llu instanties, %zu vptrs:", (unsigned long long)it->first,
                it->second.size());
            for (uint64_t v : it->second) say("  0x%llx", (unsigned long long)v);
            say("\n");
        }
        if (!any) say("Geen groepen gevonden.\n");
    }

    // ----------------------------------------------------- namen uit de engine --
    //
    // Het enige anker dat niet op statistiek steunt. De engine registreert
    // elke pool onder een naam, en die naam gaat als string naar
    // createMemoryPool:
    //
    //   MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( Object,     "ObjectPool" )
    //   MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( ActiveBody, "ActiveBody" )
    //
    // We zoeken die strings, dan de code die ernaar verwijst, en vlak daarbij
    // de constructor die de vtables wegschrijft.
    rule("MODULENAAM-ANKERS");
    static const std::vector<std::string> kNames = {
        "ActiveBody", "ObjectPool", "StructureBody", "InactiveBody",
        "HighlanderBody", "ImmortalBody", "UndeadBody",
    };
    std::vector<NameAnchor> anchors =
        findNameAnchors(t, kNames, 0x1000, counts, /*minInstances=*/8);

    std::set<uint64_t> namedActiveBody, namedObject;
    for (const NameAnchor& a : anchors) {
        say("\n-- \"%s\" --\n", a.name.c_str());
        if (a.stringAddrs.empty()) {
            say("   string niet in de binary gevonden\n");
            continue;
        }
        say("   string op:");
        for (uint64_t sa : a.stringAddrs) say(" 0x%llx", (unsigned long long)sa);
        say("   (%u verwijzingen)\n", a.xrefCount);
        if (a.candidates.empty()) {
            say("   geen vtable-kandidaten met live instanties in de buurt\n");
            continue;
        }
        say("   %-14s %-10s %-8s %s\n", "VTABLE", "AFSTAND", "XREFS", "INSTANTIES");
        for (const NameAnchor::Candidate& c : a.candidates) {
            say("   0x%-12llx %-10s %-8u %llu\n",
                (unsigned long long)c.vtable, soff(c.distance).c_str(),
                c.xrefsSeen, (unsigned long long)c.instances);
            if (a.name == "ActiveBody") namedActiveBody.insert(c.vtable);
            if (a.name == "ObjectPool") namedObject.insert(c.vtable);
        }
    }

    // ------------------------------------------- body-module aan zijn inhoud --
    rule("BODY-MODULE HERKENNEN AAN HITPOINTS");

    // Alles met een noemenswaardig aantal instanties komt in aanmerking, niet
    // alleen de kop van het histogram. De naamankers gaan er zeker in.
    std::vector<uint64_t> cand;
    for (const VtableCount& v : allVtables) {
        if (v.count < 32) break;
        cand.push_back(v.vtable);
        if (cand.size() >= 400) break;
    }
    for (uint64_t v : namedActiveBody) cand.push_back(v);
    for (uint64_t v : namedObject)     cand.push_back(v);
    if (activeBodyIface) cand.push_back(activeBodyIface);
    if (objectVtable)    cand.push_back(objectVtable);
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
    say("%zu kandidaat-vtables onderzocht.\n\n", cand.size());

    std::vector<HealthVtable> hv = findHealthVtables(t, cand, 256, -0x80, 0x400);
    std::map<uint64_t, const HealthVtable*> healthByVtable;
    for (const HealthVtable& h : hv) healthByVtable[h.vtable] = &h;

    if (hv.empty()) {
        say("Geen kandidaten onderzocht.\n");
    } else {
        say("Een hoog aandeel alleen is geen bewijs: een veld dat overal 1.0\n"
            "bevat haalt ook 100%%. Echte hitpoints verschillen per objecttype,\n"
            "dus VARIATIE (aantal verschillende max-waarden) telt zwaar mee.\n\n");
        say("%-14s %-9s %-11s %-9s %-9s %-11s %s\n",
            "VTABLE", "OFFSET", "BEVESTIGD", "VARIATIE", "MEDIAAN", "BESCHADIGD", "OORDEEL");
        size_t shown = 0;
        for (const HealthVtable& h : hv) {
            // Geslaagde kandidaten altijd, afgewezen alleen de eerste twintig:
            // met honderden kandidaten wordt de tabel anders onleesbaar.
            if (!h.passed && ++shown > 20) continue;
            char conf[32];
            snprintf(conf, sizeof(conf), "%u/%u", h.confirmations, h.sampled);
            char verdict[48];
            if (h.passed) snprintf(verdict, sizeof(verdict), "score %.2f", h.score);
            else          snprintf(verdict, sizeof(verdict), "afgewezen: %s", h.reject);
            say("0x%-12llx %-9s %-11s %-9u %-9.0f %-11u %s\n",
                (unsigned long long)h.vtable, soff(h.offset).c_str(), conf,
                h.distinctMax, h.medianMax, h.damaged, verdict);
        }
    }

    // -------------------------------------------------- objectlijst als anker --
    //
    // Object is de klasse met m_next en m_prev. Een vtable die op twee
    // verschillende offsets naar zichzelf wijst, met hoge bevestiging, is dus
    // Object. Dat is het betrouwbaarste anker dat we zonder RTTI hebben, en
    // vanaf daar zoeken we gericht in plaats van breed.
    rule("OBJECTLIJST-VINGERAFDRUK (m_next / m_prev)");

    std::vector<uint64_t> linkSources;
    for (const HealthVtable& h : hv) linkSources.push_back(h.vtable);
    for (const VtableCount& v : hist) linkSources.push_back(v.vtable);
    std::sort(linkSources.begin(), linkSources.end());
    linkSources.erase(std::unique(linkSources.begin(), linkSources.end()),
                      linkSources.end());

    std::vector<LinkPair> allLinks =
        findDoubleLinks(t, linkSources, 32, 0x400, 0x200, /*requireDistinct=*/false);

    std::map<uint64_t, uint32_t> listScore;
    {
        std::vector<LinkPair> selfLinks;
        for (const LinkPair& p : allLinks)
            if (p.vtableA == p.vtableB && p.offsetAtoB != p.offsetBtoA)
                selfLinks.push_back(p);

        if (selfLinks.empty()) {
            say("Geen dubbelgelinkte lijst gevonden.\n");
        } else {
            say("%-14s %-10s %-10s %s\n", "VTABLE", "m_next", "m_prev", "BEVESTIGD");
            for (size_t i = 0; i < selfLinks.size() && i < 12; ++i) {
                const LinkPair& p = selfLinks[i];
                say("0x%-12llx %-10s %-10s %u\n",
                    (unsigned long long)p.vtableA, soff(p.offsetAtoB).c_str(),
                    soff(p.offsetBtoA).c_str(), p.confirmations);
            }
            for (const LinkPair& p : selfLinks)
                listScore[p.vtableA] = std::max(listScore[p.vtableA], p.confirmations);
        }
    }

    std::vector<uint64_t> objectCandidates;
    for (auto& kv : listScore)
        if (kv.second >= 8) objectCandidates.push_back(kv.first);
    std::sort(objectCandidates.begin(), objectCandidates.end(),
              [&](uint64_t a, uint64_t b) { return listScore[a] > listScore[b]; });
    if (objectCandidates.size() > 4) objectCandidates.resize(4);

    // Wat "ObjectPool" op naam aanwijst hoort er altijd bij, ook als de
    // objectlijst het niet oppikte.
    for (uint64_t v : namedObject) objectCandidates.push_back(v);
    std::sort(objectCandidates.begin(), objectCandidates.end());
    objectCandidates.erase(std::unique(objectCandidates.begin(),
                                       objectCandidates.end()),
                           objectCandidates.end());
    if (!namedObject.empty())
        say("\nOp naam aangewezen via \"ObjectPool\":");
    for (uint64_t v : namedObject) say(" 0x%llx", (unsigned long long)v);
    if (!namedObject.empty()) say("\n");

    // ------------------------------------------------- gericht zoeken vanaf Object --
    //
    // Object is groot, dus m_body kan honderden bytes ver liggen. Een breed
    // opgezette zoektocht moet die reikwijdte laag houden om betaalbaar te
    // blijven; een gerichte zoektocht vanaf enkele bekende ankers kan veel
    // ruimer kijken. Dat is precies waar de vorige versie op strandde.
    rule("GERICHT ZOEKEN: Object -> body-module");
    std::vector<LinkPair> targeted;
    if (objectCandidates.empty()) {
        say("Geen Object-anker, dus niets om gericht vanaf te zoeken.\n");
    } else {
        say("Ankers: ");
        for (uint64_t v : objectCandidates) say("0x%llx ", (unsigned long long)v);
        say("\n64 instanties per anker, reikwijdte +/-0x800 vanaf Object.\n\n");

        targeted = findDoubleLinks(t, objectCandidates, 64, 0x800, 0x100,
                                   /*requireDistinct=*/true);
        if (targeted.empty()) {
            say("Geen wederzijdse verwijzing naar een andere klasse gevonden.\n");
        } else {
            say("%-14s %-12s %-14s %-12s %-10s %s\n",
                "Object", "m_body", "body-vtable", "m_object", "BEVESTIGD", "HITPOINTS");
            for (size_t i = 0; i < targeted.size() && i < 20; ++i) {
                const LinkPair& p = targeted[i];
                auto it = healthByVtable.find(p.vtableB);
                const char* hp = (it == healthByVtable.end()) ? "niet onderzocht"
                               : it->second->passed            ? "ja"
                                                               : it->second->reject;
                say("0x%-12llx %-12s 0x%-12llx %-12s %-10u %s\n",
                    (unsigned long long)p.vtableA, soff(p.offsetAtoB).c_str(),
                    (unsigned long long)p.vtableB, soff(p.offsetBtoA).c_str(),
                    p.confirmations, hp);
            }
        }
    }

    // ------------------------------------------------------------- resolutie --
    rule("RESOLUTIE");

    uint64_t resolvedBodyVtable   = 0;
    uint64_t resolvedObjectVtable = 0;
    int32_t  resolvedThisToObject = 0;
    int32_t  resolvedObjectToBody = 0;
    int32_t  resolvedHealthOffset = 0;
    uint32_t resolvedHealthConf   = 0;
    uint32_t resolvedLinkConf     = 0;
    const char* resolvedNote      = "";

    // Voorkeursvolgorde: eerst een link waarvan de body-kant ook op hitpoints
    // slaagt, want dan wijzen twee onafhankelijke aanwijzingen dezelfde kant
    // op. Pas als die er niet is, de sterkste link zonder die bevestiging.
    for (int pass = 0; pass < 2 && !resolvedBodyVtable; ++pass) {
        for (const LinkPair& p : targeted) {
            auto it = healthByVtable.find(p.vtableB);
            const bool hasHealth = (it != healthByVtable.end() && it->second->passed);
            const bool named = namedActiveBody.count(p.vtableB) > 0;
            if (pass == 0 && !hasHealth && !named) continue;
            if (p.confirmations < 4) continue;

            resolvedObjectVtable = p.vtableA;
            resolvedBodyVtable   = p.vtableB;
            resolvedObjectToBody = p.offsetAtoB;
            resolvedThisToObject = p.offsetBtoA;
            resolvedLinkConf     = p.confirmations;
            if (hasHealth && named) {
                resolvedHealthOffset = it->second->offset;
                resolvedHealthConf   = it->second->confirmations;
                resolvedNote = "modulenaam, objectlijst en hitpoints bevestigen elkaar";
            } else if (named) {
                resolvedNote = "modulenaam en objectlijst bevestigen elkaar";
            } else if (hasHealth) {
                resolvedHealthOffset = it->second->offset;
                resolvedHealthConf   = it->second->confirmations;
                resolvedNote = "objectlijst en hitpoints bevestigen elkaar";
            } else {
                resolvedNote = "alleen op de objectlijst; niets bevestigt dit";
            }
            break;
        }
    }

    if (!resolvedBodyVtable) {
        say("De body-module is niet vastgesteld.\n\n");
        if (objectCandidates.empty())
            say("Er is geen Object-anker gevonden. Zat je wel in een geladen potje?\n");
        else
            say("Het Object-anker staat vast, maar er loopt geen bevestigde\n"
                "wederzijdse verwijzing naar een andere klasse. De tabellen\n"
                "hierboven laten zien hoe ver het kwam; stuur ze terug.\n");
    } else {
        say("grondslag         : %s\n\n", resolvedNote);
        say("ActiveBody-vtable : 0x%llx  (%s)\n",
            (unsigned long long)resolvedBodyVtable, rva(t, resolvedBodyVtable).c_str());
        say("Object-vtable     : 0x%llx  (%s)\n",
            (unsigned long long)resolvedObjectVtable, rva(t, resolvedObjectVtable).c_str());
        say("bewijs            : %u link-bevestigingen", resolvedLinkConf);
        if (resolvedHealthConf) say(", %u hitpoint-bevestigingen", resolvedHealthConf);
        say("\n");

        // ActiveBody erft van vier polymorfe bases, dus het object hoort
        // precies vier vptrs te hebben. Dat is een onafhankelijke controle.
        // In het volledige histogram opzoeken, niet in de top-40: de
        // body-module staat daar juist vaak niet in.
        uint64_t bodyCount = 0;
        {
            auto it = counts.find(resolvedBodyVtable);
            if (it != counts.end()) bodyCount = it->second;
        }
        // Object niet meetellen: klassen met toevallig hetzelfde aantal
        // instanties zouden anders als vptrs van hetzelfde object gelden.
        size_t siblings = 0;
        for (auto& kv : counts)
            if (kv.second == bodyCount && kv.first != resolvedObjectVtable) siblings++;
        say("vptr-groep        : %zu vtables met %llu instanties\n",
            siblings, (unsigned long long)bodyCount);
        say("                    ActiveBody erft van vier polymorfe bases en heeft\n"
            "                    dus vier vptrs. Tellen op gelijk aantal instanties\n"
            "                    is echter grofmazig: het voegt klassen samen die\n"
            "                    toevallig even vaak voorkomen en splitst klassen\n"
            "                    waarvan niet elke vptr in de lijst staat. Vier is\n"
            "                    een bevestiging, een ander getal geen tegenbewijs.%s\n",
            siblings == 4 ? "\n                    Hier: vier, dus bevestigd." : "");

        say("\nOffsets vanaf 'this', de pointer die de detour binnenkrijgt:\n");
        say("  this -> Object          %s\n", soff(resolvedThisToObject).c_str());
        if (resolvedHealthConf)
            say("  this -> m_currentHealth %s\n", soff(resolvedHealthOffset).c_str());
        else
            say("  this -> m_currentHealth onbekend\n");
        say("\nOffset vanaf het begin van Object:\n");
        say("  Object::m_body          %s\n", soff(resolvedObjectToBody).c_str());

        say("\nControle op de verwachte vorm:\n");
        say("  this -> Object is negatief    %s\n",
            resolvedThisToObject < 0 ? "ja, zoals verwacht"
                                     : "NEE -- dit is verdacht");
        if (resolvedHealthConf)
            say("  this -> hitpoints is positief %s\n",
                resolvedHealthOffset > 0 ? "ja, zoals verwacht"
                                         : "NEE -- dit is verdacht");
    }

    // ----------------------------------------------------------- body-klassen --
    //
    // Er is niet een body-klasse maar zeven. StructureBody (alle gebouwen) en
    // ImmortalBody overschrijven attemptDamage niet, dus hun vtable bevat
    // hetzelfde functie-adres als die van ActiveBody -- maar het is een andere
    // tabel, en een hook op slot 0 van de ene raakt de andere niet. De trainer
    // moet ze dus allemaal hooken, en dit is de meting die dat onderbouwt.
    if (resolvedObjectVtable && resolvedObjectToBody) {
        rule("BODY-KLASSEN");
        std::vector<uint64_t> objs = instancesOf(t, resolvedObjectVtable, 512, true);
        std::vector<BodyVtableHit> hits =
            findBodyVtables(t, objs, (uint32_t)resolvedObjectToBody,
                            resolvedThisToObject);

        say("%zu Objecten bemonsterd, %zu body-vtables met een sluitende "
            "dubbele link\n\n", objs.size(), hits.size());
        say("  %-12s %-14s %-24s %s\n",
            "VTABLE", "BEVESTIGINGEN", "SLOT 0 (attemptDamage)", "OP NAAM");
        for (const BodyVtableHit& h : hits)
            say("  0x%-10llx %-14u 0x%-22llx %s\n",
                (unsigned long long)h.vtable, h.confirmations,
                (unsigned long long)h.slot0,
                h.vtable == resolvedBodyVtable ? "ActiveBody" : "-");
        say("\nDe dubbele link is het bewijs: elk Object wijst op +0x%x naar zijn\n"
            "body-module, en die module wijst op %s terug naar datzelfde Object.\n"
            "\nEen poolnaam staat er alleen bij de vtable die de resolutie op naam\n"
            "heeft aangewezen. De andere body-klassen zijn met de naamzoektocht\n"
            "niet uit elkaar te houden: hun vtables liggen in de binary vlak bij\n"
            "elkaar, dus de straal rond de ene naam vangt ook de andere vtable.\n"
            "Voor het hooken maakt dat niets uit -- de dubbele link volstaat --\n"
            "en een verzonnen naam zou alleen maar misleiden.\n",
            (unsigned)resolvedObjectToBody, soff(resolvedThisToObject).c_str());
    }

    // ------------------------------------------------- ActiveBody-instanties --
    std::vector<uint64_t> bodyInstances;
    if (resolvedBodyVtable) {
        bodyInstances = instancesOf(t, resolvedBodyVtable, 4096);

        rule("GEZONDHEIDSVELDEN IN ACTIVEBODY");
        say("gevonden: %zu body-instanties\n\n", bodyInstances.size());
        std::vector<HealthBlock> hb = findHealthBlocks(t, bodyInstances, -0x80, 0x400);
        if (hb.empty()) {
            say("Niets gevonden.\n");
        } else {
            say("%-12s %-12s %-10s %-12s %s\n",
                "OFFSET", "BEVESTIGD", "VARIATIE", "current", "max");
            for (const HealthBlock& b : hb)
                say("%-12s %-12u %-10u %-12.1f %.1f\n",
                    soff(b.offset).c_str(), b.confirmations, b.distinctMax,
                    b.sampleCurrent, b.sampleMax);
        }

        rule("HEXDUMP VAN DRIE ACTIVEBODY-INSTANTIES");
        say("Ruwe bytes rond 'this', zodat de resterende velden (met name\n"
            "m_indestructible) handmatig na te lopen zijn.\n");
        for (size_t i = 0; i < bodyInstances.size() && i < 3; ++i) {
            say("\n-- instantie %zu: this = 0x%llx --\n", i,
                (unsigned long long)bodyInstances[i]);
            say("%s", hexDump(t, bodyInstances[i], -0x60, 0x180).c_str());
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
            uint64_t localPlayer = pls.empty() ? 0 : pls[0].localPlayer;
            std::vector<OwnerChain> chains =
                findOwnerChains(t, objects, knownPlayers, localPlayer,
                                0x400, 0x80, 0x200);
            if (chains.empty()) {
                say("Geen keten gevonden die uitkomt op een bekende Player.\n");
            } else {
                say("\nSPELERS is het aantal verschillende spelers dat de keten\n"
                    "oplevert. Een keten die altijd dezelfde speler geeft haalt\n"
                    "evenveel bevestigingen als de juiste, dus spreiding weegt\n"
                    "zwaarder, en uitkomen bij jou het zwaarst.\n\n");
                say("%-14s %-14s %-20s %-11s %-9s %s\n",
                    "Object::m_team", "Team::m_proto", "Proto::m_owningPlayer",
                    "BEVESTIGD", "SPELERS", "JIJ EROP");
                for (const OwnerChain& c : chains)
                    say("+0x%-11x +0x%-11x +0x%-17x %-11u %-9u %s\n",
                        c.objectToTeam, c.teamToProto, c.protoToPlayer,
                        c.confirmations, c.distinctPlayers,
                        c.localSeen ? "ja" : "nee");
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
