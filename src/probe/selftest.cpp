// selftest - controleert de afleidingslogica tegen een synthetische 32-bit
// adresruimte.
//
// De echte game is 32-bit. Op een Linux-bouwmachine is een 32-bit Windows-
// proces niet altijd te draaien, dus bouwen we de adresruimte hier na in een
// buffer: vier-byte pointers, een image met vtables, een heap met objecten,
// en precies dezelfde structuurvorm als Generals. Daarmee ligt het pad dat de
// gebruiker straks raakt onder test in plaats van onder aanname.

#include "../common/target.h"
#include "../common/derive.h"

#include <cstdio>
#include <cstring>

namespace zp {

namespace {

// --- de layout die we nabouwen, en meteen het verwachte antwoord -----------
enum : uint32_t {
    IMAGE_BASE   = 0x00400000,
    IMAGE_SIZE   = 0x00010000,
    CODE_ADDR    = 0x00401000,   // "uitvoerbaar", doel van elk vtable-slot
    VT_AREA      = 0x00408000,   // hier leggen we de vtables neer

    HEAP_BASE    = 0x10000000,
    HEAP_SIZE    = 0x00100000,

    BODY_SIZE       = 0x00C0,
    BODY_IFACE_VPTR = 0x0060,    // 'this' voor attemptDamage
    BODY_M_OBJECT   = 0x0008,    // -0x58 vanaf 'this'
    BODY_HEALTH     = 0x0030,    // -0x30 vanaf 'this'

    OBJECT_SIZE     = 0x0180,
    OBJECT_M_BODY   = 0x0020,
    OBJECT_M_TEAM   = 0x00A0,

    TEAM_SIZE       = 0x0040,
    TEAM_M_PROTO    = 0x0010,

    PROTO_SIZE      = 0x0080,
    PROTO_M_OWNER   = 0x0024,

    PLAYER_SIZE     = 0x0100,

    NUM_PLAYERS     = 5,
    LOCAL_PLAYER    = 2,
    NUM_OBJECTS     = 32,
};

struct Space {
    std::vector<uint8_t> image;
    std::vector<uint8_t> heap;
    uint32_t             heapCursor = 0x1000;   // laat wat ruimte vooraan

    Space() : image(IMAGE_SIZE, 0), heap(HEAP_SIZE, 0) {}

    uint8_t* imgAt(uint32_t addr) { return image.data() + (addr - IMAGE_BASE); }
    uint8_t* heapAt(uint32_t addr) { return heap.data() + (addr - HEAP_BASE); }

    uint32_t alloc(uint32_t size) {
        uint32_t a = HEAP_BASE + heapCursor;
        heapCursor += (size + 15) & ~15u;
        return a;
    }
    void put(uint32_t addr, uint32_t value) {
        memcpy(heapAt(addr), &value, 4);
    }
    void putf(uint32_t addr, float value) {
        memcpy(heapAt(addr), &value, 4);
    }
    // Legt een vtable van acht slots neer die allemaal naar CODE_ADDR wijzen.
    uint32_t makeVtable(uint32_t addr) {
        for (int i = 0; i < 8; ++i) {
            uint32_t fn = CODE_ADDR + (uint32_t)i * 0x10;
            memcpy(imgAt(addr) + i * 4, &fn, 4);
        }
        return addr;
    }
};

int g_failed = 0;

void check(const char* what, bool ok, const char* detail = "") {
    printf("  %-44s %s%s%s\n", what, ok ? "OK" : "FOUT",
           detail[0] ? "  " : "", detail);
    if (!ok) g_failed = 1;
}

} // namespace

int runSelfTest() {
    printf("Zelftest: synthetische 32-bit adresruimte\n\n");

    Space sp;

    const uint32_t vtObject = sp.makeVtable(VT_AREA + 0x000);
    const uint32_t vtBodyPr = sp.makeVtable(VT_AREA + 0x100);
    const uint32_t vtBodyIf = sp.makeVtable(VT_AREA + 0x200);
    const uint32_t vtTeam   = sp.makeVtable(VT_AREA + 0x300);
    const uint32_t vtProto  = sp.makeVtable(VT_AREA + 0x400);
    const uint32_t vtPlayer = sp.makeVtable(VT_AREA + 0x500);

    // Spelers.
    std::vector<uint32_t> players;
    for (uint32_t i = 0; i < NUM_PLAYERS; ++i) {
        uint32_t p = sp.alloc(PLAYER_SIZE);
        sp.put(p, vtPlayer);
        players.push_back(p);
    }

    // ThePlayerList: m_local, Int m_playerCount, Player* m_players[16].
    const uint32_t pl = sp.alloc(4 + 4 + 16 * 4);
    sp.put(pl, players[LOCAL_PLAYER]);
    sp.put(pl + 4, NUM_PLAYERS);
    for (uint32_t i = 0; i < 16; ++i)
        sp.put(pl + 8 + i * 4, i < NUM_PLAYERS ? players[i] : 0);

    // Teams en prototypes.
    std::vector<uint32_t> teams;
    for (uint32_t i = 0; i < NUM_PLAYERS; ++i) {
        uint32_t proto = sp.alloc(PROTO_SIZE);
        sp.put(proto, vtProto);
        sp.put(proto + PROTO_M_OWNER, players[i]);

        uint32_t team = sp.alloc(TEAM_SIZE);
        sp.put(team, vtTeam);
        sp.put(team + TEAM_M_PROTO, proto);
        teams.push_back(team);
    }

    // Objecten met hun body-module, dubbel gelinkt.
    for (uint32_t i = 0; i < NUM_OBJECTS; ++i) {
        uint32_t obj  = sp.alloc(OBJECT_SIZE);
        uint32_t body = sp.alloc(BODY_SIZE);
        uint32_t iface = body + BODY_IFACE_VPTR;

        sp.put(obj, vtObject);
        sp.put(body, vtBodyPr);
        sp.put(iface, vtBodyIf);

        sp.put(obj + OBJECT_M_BODY, iface);
        sp.put(body + BODY_M_OBJECT, obj);
        sp.put(obj + OBJECT_M_TEAM, teams[i % NUM_PLAYERS]);

        float mx = 200.0f + (i % 5) * 75.0f;
        sp.putf(body + BODY_HEALTH + 0,  mx * 0.6f);
        sp.putf(body + BODY_HEALTH + 4,  mx);
        sp.putf(body + BODY_HEALTH + 8,  mx);
        sp.putf(body + BODY_HEALTH + 12, mx);
    }

    // --- de adresruimte aan Target aanbieden -----------------------------
    Region imgRegion;
    imgRegion.base = IMAGE_BASE;
    imgRegion.size = IMAGE_SIZE;
    imgRegion.protect = PAGE_EXECUTE_READ;
    imgRegion.state = MEM_COMMIT;
    imgRegion.type = MEM_IMAGE;

    Region heapRegion;
    heapRegion.base = HEAP_BASE;
    heapRegion.size = HEAP_SIZE;
    heapRegion.protect = PAGE_READWRITE;
    heapRegion.state = MEM_COMMIT;
    heapRegion.type = MEM_PRIVATE;

    Target t;
    t.injectTestMemory(/*is64=*/false, IMAGE_BASE, IMAGE_SIZE,
                       {imgRegion, heapRegion},
                       {sp.image, sp.heap});

    char detail[128];

    // --- ThePlayerList ---------------------------------------------------
    std::vector<PlayerListHit> pls = findPlayerLists(t);
    check("ThePlayerList gevonden", !pls.empty());
    if (!pls.empty()) {
        const PlayerListHit& h = pls[0];
        snprintf(detail, sizeof(detail), "adres 0x%llx", (unsigned long long)h.addr);
        check("  op het juiste adres", h.addr == pl, detail);
        snprintf(detail, sizeof(detail), "%u", h.playerCount);
        check("  juiste aantal spelers", h.playerCount == NUM_PLAYERS, detail);
        snprintf(detail, sizeof(detail), "%d", h.localIndex);
        check("  juiste lokale speler", h.localIndex == LOCAL_PLAYER, detail);
        snprintf(detail, sizeof(detail), "+0x%x", h.arrayOffset);
        check("  array-offset zonder opvulling", h.arrayOffset == 8, detail);
    }

    // --- vtable-histogram ------------------------------------------------
    std::vector<VtableCount> hist = vtableHistogram(t, 16);
    bool sawBody = false, sawObj = false;
    for (const VtableCount& v : hist) {
        if (v.vtable == vtBodyIf) sawBody = true;
        if (v.vtable == vtObject) sawObj = true;
    }
    check("histogram vindt de Object-vtable", sawObj);
    check("histogram vindt de body-vtable", sawBody);

    // --- dubbele links ---------------------------------------------------
    std::vector<uint64_t> cand;
    for (const VtableCount& v : hist) cand.push_back(v.vtable);
    std::vector<LinkPair> links = findDoubleLinks(t, cand, 24, 0x400, 0x200);

    const int32_t expectThisToObject = -(int32_t)(BODY_IFACE_VPTR - BODY_M_OBJECT);
    bool foundLink = false;
    for (const LinkPair& p : links) {
        if (p.vtableA == vtBodyIf && p.vtableB == vtObject &&
            p.offsetAtoB == expectThisToObject &&
            p.offsetBtoA == (int32_t)OBJECT_M_BODY) {
            foundLink = true;
            break;
        }
    }
    snprintf(detail, sizeof(detail), "verwacht this->Object = -0x%x, m_body = +0x%x",
             (unsigned)(BODY_IFACE_VPTR - BODY_M_OBJECT), (unsigned)OBJECT_M_BODY);
    check("dubbele link met negatieve m_object", foundLink, detail);

    // --- gezondheidsvelden ------------------------------------------------
    std::vector<uint64_t> bodies = instancesOf(t, vtBodyIf, 256);
    snprintf(detail, sizeof(detail), "%zu", bodies.size());
    check("body-instanties gevonden", bodies.size() == NUM_OBJECTS, detail);

    std::vector<HealthBlock> hb = findHealthBlocks(t, bodies, -0x200, 0x200);
    const int32_t expectHealth = -(int32_t)(BODY_IFACE_VPTR - BODY_HEALTH);
    bool healthOk = !hb.empty() && hb[0].offset == expectHealth;
    snprintf(detail, sizeof(detail), "verwacht -0x%x, kreeg %s",
             (unsigned)(BODY_IFACE_VPTR - BODY_HEALTH),
             hb.empty() ? "niets" : (hb[0].offset < 0 ? "negatief" : "positief"));
    check("hitpoint-blok op de juiste offset", healthOk, detail);

    // --- eigenaarsketen ---------------------------------------------------
    std::vector<uint64_t> objects = instancesOf(t, vtObject, 64);
    std::vector<uint64_t> knownPlayers(players.begin(), players.end());
    std::vector<OwnerChain> chains =
        findOwnerChains(t, objects, knownPlayers, 0x400, 0x80, 0x200);
    bool chainOk = !chains.empty() &&
                   chains[0].objectToTeam  == OBJECT_M_TEAM &&
                   chains[0].teamToProto   == TEAM_M_PROTO &&
                   chains[0].protoToPlayer == PROTO_M_OWNER;
    snprintf(detail, sizeof(detail), "verwacht +0x%x / +0x%x / +0x%x",
             (unsigned)OBJECT_M_TEAM, (unsigned)TEAM_M_PROTO, (unsigned)PROTO_M_OWNER);
    check("eigenaarsketen volledig afgeleid", chainOk, detail);

    printf("\n%s\n", g_failed ? "ZELFTEST MISLUKT" : "ZELFTEST GESLAAGD");
    return g_failed;
}

} // namespace zp
