// fake_game - synthetisch testdoel voor de probe.
//
// Bouwt in het geheugen dezelfde vorm als Generals: een ThePlayerList met
// zestien slots, Objects met een dubbele link naar hun body-module, en een
// eigenaarsketen Object -> Team -> TeamPrototype -> Player.
//
// Dit testdoel bevat bewust de drie valkuilen die een echte meting op
// Generals blootlegde:
//
//  1. Het BodyModuleInterface-subobject zit achteraan, dus m_object staat op
//     een NEGATIEVE offset vanaf de 'this' die de hook krijgt, en de
//     hitpoints op een positieve.
//  2. Objects hangen in een dubbelgelinkte lijst (m_next / m_prev). Zo'n
//     lijst geeft perfecte wederzijdse verwijzingen en verdrong in de echte
//     meting de Object <-> BodyModule-relatie volledig uit de resultaten.
//  3. Alle zestien m_players-slots zijn gevuld, ook de ongebruikte. De
//     scanner eiste eerder dat de achterste NULL waren en vond daardoor in
//     een echt potje nooit een PlayerList.
//  5. Een lokaas-eigenaarsketen die BREDER spreidt dan de echte. In een echte
//     meting won zo'n keten: hij las een pointer diep in TeamPrototype en gaf
//     onder andere de waarnemer op als eigenaar van beschadigde objecten,
//     puur omdat hij meer spelers raakte dan de juiste keten. Wat hem
//     verraadt is zijn vorm: Team en TeamPrototype zijn concrete klassen, dus
//     elke stap hoort op precies een vtable uit te komen.
//  4. Er is niet een body-klasse maar twee. StructureBody erft van ActiveBody
//     en overschrijft attemptDamage niet, dus slot 0 van zijn vtable bevat
//     hetzelfde functie-adres -- in een andere tabel. Een trainer die maar een
//     tabel hookt beschermt daardoor de helft van wat je bezit, en welke helft
//     hangt af van welke kandidaat toevallig als eerste gevonden wordt.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

// --- verwachte offsets, ook de uitkomst waar de test op controleert --------
// De volgorde volgt de MSVC-indeling: eerst de data van de primaire basis
// (met m_object), dan de vptr van het tweede basis-subobject, en daarachter
// de eigen velden van de afgeleide klasse (met de hitpoints).
enum : int {
    // ActiveBody erft van vier polymorfe bases (MemoryPoolObject, Snapshot,
    // BehaviorModuleInterface, BodyModuleInterface) en heeft dus vier vptrs.
    // Het testdoel bootst dat na, zodat de vptr-groepscontrole iets voorstelt.
    BODY_SIZE        = 0x200,
    BODY_VPTR_SNAP   = 0x0008,
    BODY_M_OBJECT    = 0x0010,   // dus -0x70 vanaf 'this'
    BODY_VPTR_BEHAV  = 0x0040,
    BODY_IFACE_VPTR  = 0x0080,   // 'this' voor attemptDamage
    BODY_HEALTH      = 0x00B0,   // dus +0x30 vanaf 'this'

    // Lokaas: vier floats die het hitpoint-patroon halen maar in elke
    // instantie dezelfde waarde hebben. Dit is de valkuil waar de derde
    // meting op strandde: zulke velden scoren het maximale aantal treffers
    // en verdrongen de echte hitpoints uit de lijst voordat die beoordeeld
    // werden. Alleen variatie tussen objecttypes onderscheidt de twee.
    BODY_DECOY_A     = 0x00D0,
    BODY_DECOY_B     = 0x00E0,
    BODY_DECOY_C     = 0x00F0,

    OBJECT_SIZE      = 0x300,
    OBJECT_M_BODY    = 0x0040,
    OBJECT_M_NEXT    = 0x0060,   // valkuil: dubbelgelinkte lijst
    OBJECT_M_PREV    = 0x0068,
    OBJECT_M_TEAM    = 0x0150,

    TEAM_SIZE        = 0x80,
    TEAM_M_PROTO     = 0x0020,

    PROTO_SIZE       = 0x100,
    PROTO_M_OWNER    = 0x0048,

    PLAYER_SIZE      = 0x200,

    // Lokaas: een tweede keten vanuit Object, die bij ALLE vier de spelers
    // uitkomt terwijl de echte keten er maar drie raakt.
    OBJECT_DECOY     = 0x0100,
    DECOY_MID_SIZE   = 0x60,
    DECOY_MID_NEXT   = 0x0010,
    DECOY_PROTO_SIZE = 0x60,
    DECOY_PROTO_OWNER= 0x0030,

    NUM_PLAYERS      = 4,
    LOCAL_PLAYER     = 1,
    NUM_OBJECTS      = 40,

    // De echte keten raakt maar drie van de vier spelers. Zo wint het lokaas
    // op spreiding, en moet de vorm de doorslag geven.
    TEAMS_IN_USE     = 3,

    // Elk vijfde object is een "gebouw" en krijgt de StructureBody-vtables.
    STRUCTURE_EVERY  = 5,
};

// Echte functies, zodat de vtable-slots naar uitvoerbaar geheugen wijzen.
static void fn0() {} static void fn1() {} static void fn2() {}
static void fn3() {} static void fn4() {} static void fn5() {}

typedef void (*Fn)();

// Naamanker: de engine geeft poolnamen als string aan createMemoryPool, dus
// "ActiveBody" staat in de binary met vlakbij een verwijzing naar de vtable.
// Hier in een struct, zodat de twee gegarandeerd binnen bereik van elkaar
// liggen in plaats van afhankelijk van de indeling die de linker kiest.
static const char g_poolName[] = "ActiveBody";
static const char g_poolNameStruct[] = "StructureBody";
// Vtables als globals: die landen in het image, net als bij de echte game.
static Fn g_vtObject[]      = {fn0, fn1, fn2, fn3, fn4, fn5};
static Fn g_vtBodyPrimary[] = {fn1, fn2, fn3, fn0, fn4, fn5};
static Fn g_vtBodySnap[]    = {fn0, fn3, fn2, fn5, fn1, fn4};
static Fn g_vtBodyBehav[]   = {fn4, fn1, fn5, fn3, fn0, fn2};
static Fn g_vtBodyIface[]   = {fn2, fn3, fn0, fn1, fn5, fn4};
// StructureBody: eigen tabellen, maar slot 0 van de interface-vtable wijst
// naar dezelfde functie als die van ActiveBody. Precies zoals bij de echte
// klasse, die attemptDamage niet overschrijft.
static Fn g_vtStructPrimary[] = {fn3, fn2, fn1, fn0, fn5, fn4};
static Fn g_vtStructSnap[]    = {fn1, fn0, fn4, fn5, fn2, fn3};
static Fn g_vtStructBehav[]   = {fn5, fn2, fn0, fn4, fn3, fn1};
static Fn g_vtStructIface[]   = {fn2, fn0, fn4, fn3, fn1, fn5};
static Fn g_vtTeam[]        = {fn3, fn0, fn1, fn2, fn4, fn5};
static Fn g_vtProto[]       = {fn4, fn5, fn0, fn1, fn2, fn3};
static Fn g_vtPlayer[]      = {fn5, fn4, fn3, fn2, fn1, fn0};

struct NameAnchorBlob {
    const char* nameRef;
    const void* filler[8];
    const void* vtableRef;
};
extern const NameAnchorBlob g_nameAnchor;
extern const NameAnchorBlob g_nameAnchorStruct;

static void put(void* base, int off, const void* val) {
    memcpy((uint8_t*)base + off, &val, sizeof(void*));
}
static void putf(void* base, int off, float v) {
    memcpy((uint8_t*)base + off, &v, sizeof(float));
}

const NameAnchorBlob g_nameAnchor = {g_poolName, {nullptr}, g_vtBodyIface};
const NameAnchorBlob g_nameAnchorStruct = {g_poolNameStruct, {nullptr}, g_vtStructIface};

int main() {
    // Aanraken zodat de linker het blok zeker meeneemt.
    volatile const void* keep = g_nameAnchor.nameRef;
    (void)keep;
    volatile const void* keep2 = g_nameAnchorStruct.nameRef;
    (void)keep2;

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
    // Alle zestien slots worden gealloceerd, net als in de echte engine;
    // m_playerCount zegt alleen hoeveel er meedoen.
    std::vector<void*> allSlots;
    for (int i = 0; i < 16; ++i) {
        if (i < NUM_PLAYERS) { allSlots.push_back(players[i]); continue; }
        void* p = calloc(1, PLAYER_SIZE);
        put(p, 0, g_vtPlayer);
        allSlots.push_back(p);
    }
    FakePlayerList* pl = (FakePlayerList*)calloc(1, sizeof(FakePlayerList));
    pl->m_local = players[LOCAL_PLAYER];
    pl->m_playerCount = NUM_PLAYERS;
    for (int i = 0; i < 16; ++i) pl->m_players[i] = allSlots[i];

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

    // --- lokaasketen -----------------------------------------------------
    // Elke schakel krijgt met opzet een ANDERE vtable, want dat is precies
    // wat een verzonnen keten in het echt ook doet: hij komt op van alles
    // uit. De echte keten heeft per stap precies een vtable.
    Fn* decoyVtables[] = {g_vtTeam, g_vtProto, g_vtPlayer, g_vtObject};
    std::vector<void*> decoyProtos, decoyMids;
    for (int i = 0; i < NUM_PLAYERS; ++i) {
        void* dp = calloc(1, DECOY_PROTO_SIZE);
        put(dp, 0, decoyVtables[i % 4]);
        put(dp, DECOY_PROTO_OWNER, players[i]);
        decoyProtos.push_back(dp);
    }
    for (int i = 0; i < NUM_PLAYERS; ++i) {
        void* dm = calloc(1, DECOY_MID_SIZE);
        put(dm, 0, decoyVtables[(i + 1) % 4]);
        put(dm, DECOY_MID_NEXT, decoyProtos[i]);
        decoyMids.push_back(dm);
    }

    // --- objecten met body-modules ---------------------------------------
    std::vector<void*> objects;
    for (int i = 0; i < NUM_OBJECTS; ++i) {
        void* obj  = calloc(1, OBJECT_SIZE);
        void* body = calloc(1, BODY_SIZE);
        objects.push_back(obj);

        const bool isStructure = (i % STRUCTURE_EVERY) == 0;

        put(obj, 0, g_vtObject);
        put(body, 0, isStructure ? g_vtStructPrimary : g_vtBodyPrimary);
        put(body, BODY_VPTR_SNAP,  isStructure ? g_vtStructSnap  : g_vtBodySnap);
        put(body, BODY_VPTR_BEHAV, isStructure ? g_vtStructBehav : g_vtBodyBehav);
        put(body, BODY_IFACE_VPTR, isStructure ? g_vtStructIface : g_vtBodyIface);

        uint8_t* iface = (uint8_t*)body + BODY_IFACE_VPTR;   // de 'this' van de hook

        put(obj,  OBJECT_M_BODY, iface);   // Object bewaart de subobject-pointer
        put(body, BODY_M_OBJECT, obj);     // en de module wijst terug
        put(obj,  OBJECT_M_TEAM, teams[i % TEAMS_IN_USE]);
        put(obj,  OBJECT_DECOY,  decoyMids[i % NUM_PLAYERS]);

        // Drie lokaasvelden met een constante waarde.
        for (int k = 0; k < 4; ++k) {
            putf(body, BODY_DECOY_A + k * 4, 1.0f);
            putf(body, BODY_DECOY_B + k * 4, 100.0f);
            putf(body, BODY_DECOY_C + k * 4, 5000.0f);
        }

        float mx = 100.0f + (i % 7) * 50.0f;
        putf(body, BODY_HEALTH + 0,  mx * (0.4f + 0.05f * (i % 10)));  // current
        putf(body, BODY_HEALTH + 4,  mx);                               // prev
        putf(body, BODY_HEALTH + 8,  mx);                               // max
        putf(body, BODY_HEALTH + 12, mx);                               // initial
    }

    // Valkuil 2: de objecten in een dubbelgelinkte ring hangen, precies zoals
    // Object::m_next en Object::m_prev dat in de echte engine doen.
    for (int i = 0; i < NUM_OBJECTS; ++i) {
        void* cur  = objects[i];
        void* next = objects[(i + 1) % NUM_OBJECTS];
        void* prev = objects[(i + NUM_OBJECTS - 1) % NUM_OBJECTS];
        put(cur, OBJECT_M_NEXT, next);
        put(cur, OBJECT_M_PREV, prev);
    }

    printf("PID=%lu\n", (unsigned long)GetCurrentProcessId());
    printf("VERWACHT this->m_object  = -0x%x\n", BODY_IFACE_VPTR - BODY_M_OBJECT);
    printf("VERWACHT this->health    = +0x%x\n", BODY_HEALTH - BODY_IFACE_VPTR);
    printf("VERWACHT Object::m_body  = +0x%x\n", OBJECT_M_BODY);
    printf("VERWACHT Object::m_team  = +0x%x\n", OBJECT_M_TEAM);
    printf("VERWACHT Team::m_proto   = +0x%x\n", TEAM_M_PROTO);
    printf("VERWACHT Proto::m_owner  = +0x%x\n", PROTO_M_OWNER);
    printf("VERWACHT lokale index    = %d van %d\n", LOCAL_PLAYER, NUM_PLAYERS);
    printf("VERWACHT body-klassen    = 2 (ActiveBody en StructureBody)\n");
    printf("LOKAAS   keten           = +0x%x/+0x%x/+0x%x (4 spelers, wisselende vtables)\n",
           OBJECT_DECOY, DECOY_MID_NEXT, DECOY_PROTO_OWNER);
    fflush(stdout);

    Sleep(600000);
    return 0;
}
