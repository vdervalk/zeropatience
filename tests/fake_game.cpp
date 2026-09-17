// fake_game - synthetisch testdoel voor de probe.
//
// Bouwt in het geheugen dezelfde vorm als Generals: een ThePlayerList met
// zestien slots, Objects met een dubbele link naar hun body-module, en een
// eigenaarsketen Object -> Team -> TeamPrototype -> Player.
//
// Belangrijk detail dat we hiermee testen: het BodyModuleInterface-subobject
// zit achteraan in het body-object, terwijl m_object vooraan staat. Vanaf de
// 'this' die de hook krijgt is m_object dus een NEGATIEVE offset. Dat is
// precies het geval dat de eerste versie van de scanner miste.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

// --- verwachte offsets, ook de uitkomst waar de test op controleert --------
enum : int {
    BODY_SIZE        = 0x180,
    BODY_IFACE_VPTR  = 0x00C0,   // 'this' voor attemptDamage
    BODY_M_OBJECT    = 0x0010,   // dus -0xB0 vanaf 'this'
    BODY_HEALTH      = 0x0060,   // dus -0x60 vanaf 'this'

    OBJECT_SIZE      = 0x300,
    OBJECT_M_BODY    = 0x0040,
    OBJECT_M_TEAM    = 0x0150,

    TEAM_SIZE        = 0x80,
    TEAM_M_PROTO     = 0x0020,

    PROTO_SIZE       = 0x100,
    PROTO_M_OWNER    = 0x0048,

    PLAYER_SIZE      = 0x200,

    NUM_PLAYERS      = 4,
    LOCAL_PLAYER     = 1,
    NUM_OBJECTS      = 40,
};

// Echte functies, zodat de vtable-slots naar uitvoerbaar geheugen wijzen.
static void fn0() {} static void fn1() {} static void fn2() {}
static void fn3() {} static void fn4() {} static void fn5() {}

typedef void (*Fn)();
// Vtables als globals: die landen in het image, net als bij de echte game.
static Fn g_vtObject[]      = {fn0, fn1, fn2, fn3, fn4, fn5};
static Fn g_vtBodyPrimary[] = {fn1, fn2, fn3, fn0, fn4, fn5};
static Fn g_vtBodyIface[]   = {fn2, fn3, fn0, fn1, fn5, fn4};
static Fn g_vtTeam[]        = {fn3, fn0, fn1, fn2, fn4, fn5};
static Fn g_vtProto[]       = {fn4, fn5, fn0, fn1, fn2, fn3};
static Fn g_vtPlayer[]      = {fn5, fn4, fn3, fn2, fn1, fn0};

static void put(void* base, int off, const void* val) {
    memcpy((uint8_t*)base + off, &val, sizeof(void*));
}
static void putf(void* base, int off, float v) {
    memcpy((uint8_t*)base + off, &v, sizeof(float));
}

int main() {
    // --- spelers ---------------------------------------------------------
    std::vector<void*> players;
    for (int i = 0; i < NUM_PLAYERS; ++i) {
        void* p = calloc(1, PLAYER_SIZE);
        put(p, 0, g_vtPlayer);
        players.push_back(p);
    }

    // --- ThePlayerList ---------------------------------------------------
    // m_local, dan Int m_playerCount, dan Player* m_players[16]. De opvulling
    // tussen de int en de array laat de compiler zelf bepalen.
    struct FakePlayerList {
        void*    m_local;
        int      m_playerCount;
        void*    m_players[16];
    };
    FakePlayerList* pl = (FakePlayerList*)calloc(1, sizeof(FakePlayerList));
    pl->m_local = players[LOCAL_PLAYER];
    pl->m_playerCount = NUM_PLAYERS;
    for (int i = 0; i < NUM_PLAYERS; ++i) pl->m_players[i] = players[i];

    // --- teams en prototypes --------------------------------------------
    std::vector<void*> teams, protos;
    for (int i = 0; i < NUM_PLAYERS; ++i) {
        void* proto = calloc(1, PROTO_SIZE);
        put(proto, 0, g_vtProto);
        put(proto, PROTO_M_OWNER, players[i]);
        protos.push_back(proto);

        void* team = calloc(1, TEAM_SIZE);
        put(team, 0, g_vtTeam);
        put(team, TEAM_M_PROTO, proto);
        teams.push_back(team);
    }

    // --- objecten met body-modules ---------------------------------------
    for (int i = 0; i < NUM_OBJECTS; ++i) {
        void* obj  = calloc(1, OBJECT_SIZE);
        void* body = calloc(1, BODY_SIZE);

        put(obj, 0, g_vtObject);
        put(body, 0, g_vtBodyPrimary);
        put(body, BODY_IFACE_VPTR, g_vtBodyIface);

        uint8_t* iface = (uint8_t*)body + BODY_IFACE_VPTR;   // de 'this' van de hook

        put(obj,  OBJECT_M_BODY, iface);   // Object bewaart de subobject-pointer
        put(body, BODY_M_OBJECT, obj);     // en de module wijst terug
        put(obj,  OBJECT_M_TEAM, teams[i % NUM_PLAYERS]);

        float mx = 100.0f + (i % 7) * 50.0f;
        putf(body, BODY_HEALTH + 0,  mx * (0.4f + 0.05f * (i % 10)));  // current
        putf(body, BODY_HEALTH + 4,  mx);                               // prev
        putf(body, BODY_HEALTH + 8,  mx);                               // max
        putf(body, BODY_HEALTH + 12, mx);                               // initial
    }

    printf("PID=%lu\n", (unsigned long)GetCurrentProcessId());
    printf("VERWACHT this->m_object  = -0x%x\n", BODY_IFACE_VPTR - BODY_M_OBJECT);
    printf("VERWACHT this->health    = -0x%x\n", BODY_IFACE_VPTR - BODY_HEALTH);
    printf("VERWACHT Object::m_body  = +0x%x\n", OBJECT_M_BODY);
    printf("VERWACHT Object::m_team  = +0x%x\n", OBJECT_M_TEAM);
    printf("VERWACHT Team::m_proto   = +0x%x\n", TEAM_M_PROTO);
    printf("VERWACHT Proto::m_owner  = +0x%x\n", PROTO_M_OWNER);
    printf("VERWACHT lokale index    = %d van %d\n", LOCAL_PLAYER, NUM_PLAYERS);
    fflush(stdout);

    Sleep(600000);
    return 0;
}
