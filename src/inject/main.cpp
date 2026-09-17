// zp-inject - laadt zp-freeze.dll in het draaiende spel.
//
// De standaardroute: geheugen reserveren in het doelproces, daar het pad van
// de DLL neerzetten, en een thread starten op LoadLibraryA. Kernel32 staat in
// elk 32-bit proces op hetzelfde adres, dus het adres van LoadLibraryA uit
// ons eigen proces is ook daar geldig.

#include "../common/target.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace zp;

static std::string dllPathNextToMe(const char* name) {
    char self[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, self, sizeof(self));
    std::string p = self;
    size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos) p.resize(slash + 1);
    return p + name;
}

static bool inject(DWORD pid, const std::string& dll, std::string* err) {
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                           PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        *err = "OpenProcess faalde (code " + std::to_string(GetLastError()) +
               "). Start de injector als administrator.";
        return false;
    }

    const SIZE_T bytes = dll.size() + 1;
    void* remote = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        *err = "VirtualAllocEx faalde (code " + std::to_string(GetLastError()) + ")";
        CloseHandle(h);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(h, remote, dll.c_str(), bytes, &written) ||
        written != bytes) {
        *err = "kon het DLL-pad niet schrijven";
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }

    FARPROC loadLibrary = GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                         "LoadLibraryA");
    if (!loadLibrary) {
        *err = "LoadLibraryA niet gevonden";
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }

    // LoadLibraryA neemt een pointer en geeft een handle terug, dus de vorm
    // past op een threadfunctie. De cast is opzet.
    LPTHREAD_START_ROUTINE start =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(loadLibrary));
    HANDLE th = CreateRemoteThread(h, nullptr, 0, start, remote, 0, nullptr);
    if (!th) {
        *err = "CreateRemoteThread faalde (code " +
               std::to_string(GetLastError()) + ")";
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }

    WaitForSingleObject(th, 15000);
    DWORD module = 0;
    GetExitCodeThread(th, &module);
    CloseHandle(th);
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);

    if (module == 0) {
        *err = "LoadLibrary in het spel gaf NULL terug; staat zp-freeze.dll "
               "naast de injector en is het 32-bit?";
        return false;
    }
    return true;
}

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

    if (dll.empty()) dll = dllPathNextToMe("zp-freeze.dll");
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
    if (!inject(pid, dll, &err)) {
        printf("MISLUKT: %s\n", err.c_str());
        return 1;
    }
    printf("Gelukt. Er hoort nu een venster \"zeropatience\" te zijn met het\n"
           "resultaat. Duurt een paar seconden: de DLL bepaalt eerst de\n"
           "offsets in het draaiende spel.\n");
    return 0;
}
