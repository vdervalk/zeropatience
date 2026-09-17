// zp-inject - laadt zp-freeze.dll in het draaiende spel.
//
// De standaardroute: geheugen reserveren in het doelproces, daar het pad van
// de DLL neerzetten, en een thread starten op LoadLibraryA. Kernel32 staat in
// elk 32-bit proces op hetzelfde adres, dus het adres van LoadLibraryA uit
// ons eigen proces is ook daar geldig.

#include "../common/target.h"
#include "../common/inject.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace zp;

static void usage() {
    printf(
        "zp-inject - laadt zp-freeze.dll in Generals of Zero Hour\n"
        "\n"
        "  --list          toon alle draaiende processen en stop\n"
        "  --pid <n>       injecteer in dit proces-id\n"
        "  --dll <pad>     pad naar de DLL (standaard zp-freeze.dll ernaast)\n"
        "\n"
        "Start het spel, laad een SKIRMISH met een paar eigen units, en draai\n"
        "dit dan als administrator. In een menu bestaan de structuren nog niet\n"
        "en kan de DLL de offsets niet bepalen.\n");
}

int main(int argc, char** argv) {
    DWORD pid = 0;
    std::string dll;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list") {
            printf("%-8s %s\n", "PID", "NAAM");
            for (const auto& p : listProcesses())
                printf("%-8lu %s%s\n", (unsigned long)p.pid, p.name.c_str(),
                       looksLikeGenerals(p.name) ? "   <-- ziet eruit als Generals" : "");
            return 0;
        }
        if (a == "--pid" && i + 1 < argc) { pid = (DWORD)strtoul(argv[++i], nullptr, 0); continue; }
        if (a == "--dll" && i + 1 < argc) { dll = argv[++i]; continue; }
        usage();
        return 2;
    }

    if (dll.empty()) dll = pathNextToExe("zp-freeze.dll");
    if (GetFileAttributesA(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("zp-freeze.dll niet gevonden op:\n  %s\n\n"
               "Zet de DLL naast de injector, of geef --dll <pad>.\n", dll.c_str());
        return 1;
    }

    if (!pid) {
        std::vector<std::pair<uint64_t, ProcEntry>> ranked;
        for (const auto& p : listProcesses())
            if (looksLikeGenerals(p.name))
                ranked.push_back({privateCommitBytes(p.pid), p});
        if (ranked.empty()) {
            printf("Geen Generals-proces gevonden. Start het spel en laad een\n"
                   "skirmish, of gebruik --list en --pid.\n");
            return 1;
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        if (ranked.size() > 1) {
            printf("Kandidaten:\n");
            for (const auto& r : ranked)
                printf("  --pid %-8lu %-16s %6llu MB\n",
                       (unsigned long)r.second.pid, r.second.name.c_str(),
                       (unsigned long long)(r.first / (1024 * 1024)));
            const uint64_t big = ranked[0].first, next = ranked[1].first;
            if (big == 0 || (next && big < next * 2)) {
                printf("\nTe dicht bij elkaar om te kiezen; gebruik --pid.\n");
                return 1;
            }
            printf("\nGekozen: %s (de engine; de andere is de launcher).\n\n",
                   ranked[0].second.name.c_str());
        }
        pid = ranked[0].second.pid;
    }

    printf("Injecteren in pid %lu...\n", (unsigned long)pid);
    std::string err;
    if (!injectDll(pid, dll, &err)) {
        printf("MISLUKT: %s\n", err.c_str());
        return 1;
    }
    printf("Gelukt. Er hoort nu een venster \"zeropatience\" te zijn met het\n"
           "resultaat. Duurt een paar seconden: de DLL bepaalt eerst de\n"
           "offsets in het draaiende spel.\n");
    return 0;
}
