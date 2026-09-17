// target.h - proceskoppeling, regio-enumeratie en een lokale geheugensnapshot.
//
// De probe doet miljoenen kleine "leesacties" tijdens het afleiden van offsets.
// Die allemaal via ReadProcessMemory doen zou uren duren, dus kopieren we de
// interessante regio's een keer naar onze eigen adresruimte en lezen daarna
// lokaal. Alle adressen zijn uint64_t, ook voor 32-bit doelen, zodat dezelfde
// code beide gevallen aankan.
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace zp {

struct Region {
    uint64_t base = 0;
    uint64_t size = 0;
    DWORD    protect = 0;
    DWORD    state = 0;
    DWORD    type = 0;

    bool readable() const {
        if (state != MEM_COMMIT) return false;
        if (protect & PAGE_GUARD) return false;
        if (protect & PAGE_NOACCESS) return false;
        return true;
    }
    bool writable() const {
        const DWORD w = PAGE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return readable() && (protect & w) != 0;
    }
};

struct Snapshot {
    uint64_t             base = 0;
    std::vector<uint8_t> bytes;
    const Region*        region = nullptr;

    bool covers(uint64_t addr, size_t n) const {
        return addr >= base && addr + n <= base + bytes.size();
    }
    const uint8_t* at(uint64_t addr) const { return bytes.data() + (addr - base); }
};

class Target {
public:
    ~Target() { if (h_) CloseHandle(h_); }

    bool attach(DWORD pid, std::string* err);

    DWORD  pid()   const { return pid_; }
    HANDLE handle() const { return h_; }
    bool   is64()  const { return is64_; }
    size_t ptrSize() const { return is64_ ? 8 : 4; }

    uint64_t imageBase() const { return imageBase_; }
    uint64_t imageSize() const { return imageSize_; }
    const std::string& imagePath() const { return imagePath_; }

    const std::vector<Region>& regions() const { return regions_; }

    // --- live lezen, via het OS -------------------------------------------
    bool readRaw(uint64_t addr, void* out, size_t n) const {
        SIZE_T got = 0;
        if (!ReadProcessMemory(h_, (LPCVOID)(uintptr_t)addr, out, n, &got)) return false;
        return got == n;
    }

    // --- snapshot opbouwen -----------------------------------------------
    // Kopieert alle leesbare regio's tot aan maxTotalBytes. Grote regio's die
    // niet meer passen worden overgeslagen, niet half gekopieerd.
    void buildSnapshot(uint64_t maxTotalBytes, bool includeMappedFiles);

    uint64_t snapshotBytes() const { return snapTotal_; }
    size_t   snapshotCount() const { return snaps_.size(); }
    const std::vector<Snapshot>& snapshots() const { return snaps_; }

    // --- lokaal lezen uit de snapshot ------------------------------------
    const uint8_t* local(uint64_t addr, size_t n) const {
        const Snapshot* s = findSnap(addr);
        if (!s || !s->covers(addr, n)) return nullptr;
        return s->at(addr);
    }

    bool r8 (uint64_t a, uint8_t&  v) const { return copyLocal(a, &v, 1); }
    bool r16(uint64_t a, uint16_t& v) const { return copyLocal(a, &v, 2); }
    bool r32(uint64_t a, uint32_t& v) const { return copyLocal(a, &v, 4); }
    bool r64(uint64_t a, uint64_t& v) const { return copyLocal(a, &v, 8); }
    bool rf32(uint64_t a, float&   v) const { return copyLocal(a, &v, 4); }

    // Pointer ter grootte van het doelproces.
    bool rptr(uint64_t a, uint64_t& v) const {
        if (is64_) return r64(a, v);
        uint32_t t = 0;
        if (!r32(a, t)) return false;
        v = t;
        return true;
    }

    // Is dit adres een plausibele pointer naar toegewezen geheugen?
    bool isMapped(uint64_t addr) const { return findRegion(addr) != nullptr; }

    bool inImage(uint64_t addr) const {
        return addr >= imageBase_ && addr < imageBase_ + imageSize_;
    }

    bool isExecutable(uint64_t addr) const {
        const Region* r = findRegion(addr);
        if (!r) return false;
        const DWORD x = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (r->protect & x) != 0;
    }

    const Region* findRegion(uint64_t addr) const;
    const Snapshot* findSnap(uint64_t addr) const;

    // Alleen voor de zelftest: een synthetische adresruimte injecteren in
    // plaats van een echt proces te lezen. Zo is de 32-bit pointerafhandeling
    // te controleren op een machine waar geen 32-bit doel te draaien is.
    void injectTestMemory(bool is64, uint64_t imageBase, uint64_t imageSize,
                          std::vector<Region> regions,
                          std::vector<std::vector<uint8_t>> contents);

private:
    bool copyLocal(uint64_t a, void* out, size_t n) const {
        const uint8_t* p = local(a, n);
        if (!p) return false;
        memcpy(out, p, n);
        return true;
    }

    void enumRegions();
    void queryImage();

    HANDLE      h_ = nullptr;
    DWORD       pid_ = 0;
    bool        is64_ = false;
    uint64_t    imageBase_ = 0;
    uint64_t    imageSize_ = 0;
    std::string imagePath_;

    std::vector<Region>   regions_;
    std::vector<Snapshot> snaps_;
    uint64_t              snapTotal_ = 0;
};

// Processen opsommen.
struct ProcEntry {
    DWORD       pid = 0;
    std::string name;
};
std::vector<ProcEntry> listProcesses();

// Bekende procesnamen van Generals / Zero Hour, inclusief de heruitgave.
bool looksLikeGenerals(const std::string& exeName);

// Privaat vastgelegd geheugen van een proces, in bytes. Nul als het niet op
// te vragen is. Wordt gebruikt om de echte game van zijn launcher te
// onderscheiden: een launcher gebruikt tientallen MB, de engine honderden.
uint64_t privateCommitBytes(DWORD pid);

} // namespace zp
