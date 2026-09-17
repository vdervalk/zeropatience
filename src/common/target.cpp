#include "target.h"

#include <tlhelp32.h>
#include <psapi.h>

namespace zp {

static bool processIs64(HANDLE h) {
#if defined(_WIN64)
    BOOL wow64 = FALSE;
    if (!IsWow64Process(h, &wow64)) return false;
    // Op een 64-bit OS: draait het onder WOW64, dan is het een 32-bit proces.
    return !wow64;
#else
    (void)h;
    return false;  // een 32-bit probe kan toch alleen 32-bit doelen inspecteren
#endif
}

bool Target::attach(DWORD pid, std::string* err) {
    pid_ = pid;
    h_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h_) {
        DWORD e = GetLastError();
        if (err) {
            *err = "OpenProcess faalde (code " + std::to_string(e) + ")";
            if (e == ERROR_ACCESS_DENIED)
                *err += ". Start de probe als administrator.";
        }
        return false;
    }
    is64_ = processIs64(h_);
    queryImage();
    enumRegions();
    if (regions_.empty()) {
        if (err) *err = "geen geheugenregio's gevonden; draait het proces nog?";
        return false;
    }
    return true;
}

void Target::queryImage() {
    HMODULE mods[16];
    DWORD needed = 0;
    if (EnumProcessModules(h_, mods, sizeof(mods), &needed) && needed >= sizeof(HMODULE)) {
        imageBase_ = (uint64_t)(uintptr_t)mods[0];
        MODULEINFO mi{};
        if (GetModuleInformation(h_, mods[0], &mi, sizeof(mi)))
            imageSize_ = mi.SizeOfImage;
        char path[MAX_PATH] = {0};
        if (GetModuleFileNameExA(h_, mods[0], path, sizeof(path)))
            imagePath_ = path;
    }
    if (!imagePath_.empty() && imageSize_) return;

    // Terugval: hoofdmodulepad los opvragen.
    char path[MAX_PATH] = {0};
    DWORD n = sizeof(path);
    if (QueryFullProcessImageNameA(h_, 0, path, &n)) imagePath_ = path;
}

void Target::enumRegions() {
    regions_.clear();
    uint64_t addr = 0;
    const uint64_t limit = is64_ ? 0x00007FFFFFFFFFFFull : 0xFFFFFFFFull;
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < limit) {
        SIZE_T got = VirtualQueryEx(h_, (LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi));
        if (got != sizeof(mbi)) break;
        Region r;
        r.base    = (uint64_t)(uintptr_t)mbi.BaseAddress;
        r.size    = (uint64_t)mbi.RegionSize;
        r.protect = mbi.Protect;
        r.state   = mbi.State;
        r.type    = mbi.Type;
        if (r.size == 0) break;
        regions_.push_back(r);
        uint64_t next = r.base + r.size;
        if (next <= addr) break;
        addr = next;
    }
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.base < b.base; });
}

const Region* Target::findRegion(uint64_t addr) const {
    auto it = std::upper_bound(regions_.begin(), regions_.end(), addr,
                               [](uint64_t v, const Region& r) { return v < r.base; });
    if (it == regions_.begin()) return nullptr;
    --it;
    if (addr >= it->base && addr < it->base + it->size && it->readable()) return &*it;
    return nullptr;
}

const Snapshot* Target::findSnap(uint64_t addr) const {
    auto it = std::upper_bound(snaps_.begin(), snaps_.end(), addr,
                               [](uint64_t v, const Snapshot& s) { return v < s.base; });
    if (it == snaps_.begin()) return nullptr;
    --it;
    if (addr >= it->base && addr < it->base + it->bytes.size()) return &*it;
    return nullptr;
}

void Target::buildSnapshot(uint64_t maxTotalBytes, bool includeMappedFiles) {
    snaps_.clear();
    snapTotal_ = 0;

    // Het hoofdmodule-image eerst: daar staan vtables en RTTI, dat willen we
    // sowieso hebben, ongeacht het budget.
    std::vector<const Region*> ordered;
    for (const auto& r : regions_) {
        if (!r.readable()) continue;
        if (r.type == MEM_MAPPED && !includeMappedFiles) continue;
        ordered.push_back(&r);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [this](const Region* a, const Region* b) {
                         bool ia = inImage(a->base), ib = inImage(b->base);
                         if (ia != ib) return ia;          // image wint
                         if (a->type != b->type)
                             return a->type == MEM_PRIVATE; // dan de heap
                         return a->size < b->size;          // kleine regio's eerst
                     });

    for (const Region* r : ordered) {
        if (snapTotal_ + r->size > maxTotalBytes) continue;
        Snapshot s;
        s.base = r->base;
        s.region = r;
        s.bytes.resize((size_t)r->size);
        SIZE_T got = 0;
        if (!ReadProcessMemory(h_, (LPCVOID)(uintptr_t)r->base, s.bytes.data(),
                               (SIZE_T)r->size, &got) || got == 0) {
            continue;  // regio kan tussentijds zijn vrijgegeven; overslaan
        }
        s.bytes.resize((size_t)got);
        snapTotal_ += got;
        snaps_.push_back(std::move(s));
    }
    std::sort(snaps_.begin(), snaps_.end(),
              [](const Snapshot& a, const Snapshot& b) { return a.base < b.base; });
}

void Target::injectTestMemory(bool is64, uint64_t imageBase, uint64_t imageSize,
                              std::vector<Region> regions,
                              std::vector<std::vector<uint8_t>> contents) {
    is64_ = is64;
    imageBase_ = imageBase;
    imageSize_ = imageSize;
    regions_ = std::move(regions);
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.base < b.base; });

    snaps_.clear();
    snapTotal_ = 0;
    for (size_t i = 0; i < regions_.size() && i < contents.size(); ++i) {
        Snapshot s;
        s.base = regions_[i].base;
        s.bytes = std::move(contents[i]);
        s.region = &regions_[i];   // regions_ wordt hierna niet meer aangepast
        snapTotal_ += s.bytes.size();
        snaps_.push_back(std::move(s));
    }
    std::sort(snaps_.begin(), snaps_.end(),
              [](const Snapshot& a, const Snapshot& b) { return a.base < b.base; });
    // Na het sorteren kloppen de region-pointers nog, want regions_ zelf is
    // niet verplaatst; alleen de snapshot-volgorde is veranderd.
    for (Snapshot& s : snaps_) s.region = findRegion(s.base);
}

std::vector<ProcEntry> listProcesses() {
    std::vector<ProcEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            ProcEntry e;
            e.pid = pe.th32ProcessID;
            e.name = pe.szExeFile;
            out.push_back(e);
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

bool looksLikeGenerals(const std::string& exeName) {
    std::string n;
    n.reserve(exeName.size());
    for (char c : exeName) n += (char)tolower((unsigned char)c);

    // game.dat is de hernoemde Zero Hour-executable van de retailversie.
    static const char* kNames[] = {
        "generals.exe", "generalszh.exe", "game.dat",
        "cncgenerals.exe", "cncgeneralszh.exe",
        "commandandconquergenerals.exe",
        "commandandconquergeneralszerohour.exe",
    };
    for (const char* k : kNames)
        if (n == k) return true;

    // Ruimere match voor de heruitgave, waarvan de naam per platform verschilt.
    bool hasGenerals = n.find("generals") != std::string::npos;
    bool isExe = n.size() > 4 && n.compare(n.size() - 4, 4, ".exe") == 0;
    return hasGenerals && isExe;
}

} // namespace zp
