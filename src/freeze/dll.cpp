// zp-freeze.dll - health-freeze voor Generals en Zero Hour.
//
// Wat de hook doet, is precies wat de engine zelf doet als m_indestructible
// aan staat: bovenaan ActiveBody::attemptDamage terugkeren voordat er iets
// met de schade gebeurt. Het verschil is dat wij die keuze per object maken,
// op basis van wie de eigenaar is.
//
//   void ActiveBody::attemptDamage( DamageInfo *damageInfo )
//   {
//       validateArmorAndDamageFX();
//       if( damageInfo == NULL )  return;
//       if( m_indestructible )    return;   // <-- dit gedrag bootsen we na
//       ...
//   }
//
// De hook zit in de vtable, niet in de code. Een vtable-slot is een pointer
// op een bekend adres: schrijven om aan te zetten, terugzetten om uit te
// zetten. Geen instructies decoderen, geen halve instructie overschreven.
//
// Het zijn meerdere vtables, geen een. De engine heeft zeven body-klassen en
// elke klasse heeft zijn eigen tabel, ook als hij attemptDamage helemaal niet
// overschrijft: StructureBody (alle gebouwen) erft die functie van ActiveBody,
// dus het functie-adres is hetzelfde maar de tabel niet. Wie een tabel hookte,
// beschermde de helft van wat hij bezat -- en welke helft hing af van welke
// kandidaat de zoektocht toevallig als eerste opleverde.

#include "resolve.h"
#include "../common/shared.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace zp;

// ------------------------------------------------------------------ status --

static Resolved   g_r;
static bool       g_enabled = false;
static bool       g_hooked = false;
static HANDLE     g_console = nullptr;   // alleen als de mapping niet lukt
static HANDLE     g_mapping = nullptr;
static Shared*    g_shared = nullptr;

// De GUI leest dit. Lukt de mapping niet, dan valt alles terug op een console,
// zodat er nooit een stille mislukking is.
static bool openShared() {
    char name[128];
    sharedName(GetCurrentProcessId(), name, sizeof(name));
    g_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(Shared), name);
    if (!g_mapping) return false;
    g_shared = (Shared*)MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                      sizeof(Shared));
    if (!g_shared) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }
    memset(g_shared, 0, sizeof(Shared));
    g_shared->magic = SHARED_MAGIC;
    g_shared->version = SHARED_VERSION;
    g_shared->state = STATE_STARTING;
    // Nul zou "speler 0" betekenen, en dat is de neutrale speler. De stand
    // moet expliciet "de lokale speler" zijn, anders beschermt een verse
    // mapping stilzwijgend het verkeerde.
    g_shared->protectPlayer = -1;
    return true;
}

static void setState(uint32_t st) {
    if (g_shared) g_shared->state = st;
}

// Een thunk per gehookte vtable, elk met zijn eigen bewaarde origineel.
//
// Dat is niet uit weelde. ActiveBody en StructureBody delen hetzelfde
// attemptDamage-adres, maar HighlanderBody, UndeadBody en HiveStructureBody
// hebben elk een eigen versie. Met een enkele g_original zou de tweede hook
// het origineel van de eerste overschrijven, en dan roept de ene klasse de
// attemptDamage van de andere aan. Dat is precies het soort fout dat pas
// later en ergens anders opvalt.
//
// De thunks zijn statisch gegenereerde code, geen zelf geschreven bytes: de
// assembler maakt ze, de linker plaatst ze, en er komt geen uitvoerbaar
// geheugen aan te pas dat we zelf alloceren.
#define ZP_ORIGINALS(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)

extern "C" {
#define ZP_DECL(n) void* g_original_##n = nullptr; void zp_detour_##n();
ZP_ORIGINALS(ZP_DECL)
#undef ZP_DECL

int   zp_should_block(void* self, void* damageInfo);

// Alleen voor de zelfcontrole van de thunk, hieronder.
int   g_forceBlock = 0;
void* g_testThis = nullptr;
void* g_testDi = nullptr;
int   g_testCalls = 0;
void  zp_test_original();
int   zp_test_call(void* fn, void* self, void* di);
}

#define ZP_ENTRY(n) (void*)&zp_detour_##n,
static void* const kDetour[ZP_MAX_BODY_VTABLES] = { ZP_ORIGINALS(ZP_ENTRY) };
#undef ZP_ENTRY

#define ZP_SLOT(n) &g_original_##n,
static void** const kOriginal[ZP_MAX_BODY_VTABLES] = { ZP_ORIGINALS(ZP_SLOT) };
#undef ZP_SLOT

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_console) {
        DWORD written = 0;
        WriteConsoleA(g_console, buf, (DWORD)strlen(buf), &written, nullptr);
    }
    if (g_shared) {
        // Aanvullen tot de buffer vol is. Afkappen in plaats van rondschrijven:
        // wat er misgaat staat aan het begin, niet aan het eind.
        size_t n = strlen(buf);
        uint32_t used = g_shared->logLength;
        if (used < SHARED_LOG_BYTES - 1) {
            size_t room = SHARED_LOG_BYTES - 1 - used;
            if (n > room) n = room;
            memcpy(g_shared->log + used, buf, n);
            g_shared->log[used + n] = 0;
            g_shared->logLength = (uint32_t)(used + n);
        }
    }
    OutputDebugStringA(buf);
}

// -------------------------------------------------------------- de detour --
//
// attemptDamage is __thiscall: 'this' in ECX, het argument op de stack, en de
// callee ruimt dat argument op (ret 4). GCC kent geen naked functies op x86,
// dus de omweg staat in een assembly-blok. Zo is er geen twijfel over wat de
// compiler ervan maakt.
#define ZP_THUNK(n)                                                           \
    ".globl _zp_detour_" #n "\n"                                              \
    "_zp_detour_" #n ":\n"                                                    \
    "  pushl %ebp\n"                                                          \
    "  movl  %esp, %ebp\n"                                                    \
    "  pushl %ecx\n"                  /* this bewaren; de call klobbert ecx */\
    "  pushl 8(%ebp)\n"               /* damageInfo */                        \
    "  pushl %ecx\n"                  /* this */                              \
    "  call  _zp_should_block\n"                                              \
    "  addl  $8, %esp\n"                                                      \
    "  movl  -4(%ebp), %ecx\n"        /* this herstellen */                   \
    "  testl %eax, %eax\n"                                                    \
    "  jnz   1f\n"                                                            \
    "  pushl 8(%ebp)\n"               /* doorgeven aan het origineel */       \
    "  call  *_g_original_" #n "\n"   /* dat doet zelf ret 4 */               \
    "1:\n"                                                                    \
    "  movl  %ebp, %esp\n"                                                    \
    "  popl  %ebp\n"                                                          \
    "  ret   $4\n"

__asm__(
    ".text\n"
    ZP_THUNK(0) ZP_THUNK(1) ZP_THUNK(2) ZP_THUNK(3)
    ZP_THUNK(4) ZP_THUNK(5) ZP_THUNK(6) ZP_THUNK(7)
);

// ------------------------------------------------- zelfcontrole van de thunk --
//
// Dit is het gevaarlijkste stuk code in het project: klopt de stack-discipline
// niet, dan crasht het spel bij de eerste kogel. De thunk wordt daarom eerst
// tegen een nep-vtable uitgeprobeerd, hier in het proces, voordat de echte
// vtable wordt aangeraakt.
//
// zp_test_original gedraagt zich als een thiscall-functie: 'this' in ECX, het
// argument op de stack, en de callee ruimt dat argument op.
//
// zp_test_call roept een thiscall-functie aan en geeft terug hoeveel de stack
// pointer is verschoven. Nul betekent dat de conventie klopt.
__asm__(
    ".text\n"
    ".globl _zp_test_original\n"
    "_zp_test_original:\n"
    "  movl  %ecx, _g_testThis\n"
    "  movl  4(%esp), %eax\n"
    "  movl  %eax, _g_testDi\n"
    "  incl  _g_testCalls\n"
    "  ret   $4\n"
    "\n"
    ".globl _zp_test_call\n"
    "_zp_test_call:\n"
    "  pushl %ebp\n"
    "  movl  %esp, %ebp\n"
    "  pushl %ebx\n"
    "  movl  %esp, %ebx\n"            /* stand voor het argument */
    "  movl  16(%ebp), %eax\n"        /* di */
    "  pushl %eax\n"
    "  movl  12(%ebp), %ecx\n"        /* this */
    "  movl  8(%ebp), %eax\n"         /* fn */
    "  call  *%eax\n"                 /* thiscall: callee doet ret 4 */
    "  subl  %esp, %ebx\n"            /* 0 als de stack terecht is */
    "  movl  %ebx, %eax\n"
    "  popl  %ebx\n"
    "  movl  %ebp, %esp\n"
    "  popl  %ebp\n"
    "  ret\n"
);

// Probeert de thunks uit met een nep-origineel. Geeft true als zowel het
// doorgeven als het blokkeren klopt en de stack in beide gevallen terecht is.
//
// Alle acht worden getest, niet alleen de eerste. Ze komen uit dezelfde macro
// en zijn dus per constructie gelijk, maar wat hier fout kan gaan is juist de
// koppeling tussen thunk n en g_original_n -- en dat is precies wat een test
// van alleen nummer nul niet ziet.
static bool selfCheckThunk() {
    const bool savedEnabled = g_enabled;
    bool good = true;

    for (uint32_t i = 0; i < ZP_MAX_BODY_VTABLES; ++i) {
        char fakeSelf[64] = {0};
        char fakeDi[16] = {0};

        void* savedOriginal = *kOriginal[i];
        *kOriginal[i] = (void*)&zp_test_original;

        // Geval 1: niet blokkeren, dus doorgeven aan het origineel.
        g_enabled = false;
        g_forceBlock = 0;
        g_testCalls = 0;
        g_testThis = nullptr;
        g_testDi = nullptr;
        int drift = zp_test_call(kDetour[i], fakeSelf, fakeDi);
        if (drift != 0) {
            logf("[zp] thunk %u: stack verschoof %d bytes bij doorgeven\n", i, drift);
            good = false;
        }
        if (g_testCalls != 1) {
            logf("[zp] thunk %u: origineel %d keer aangeroepen, verwacht 1\n",
                 i, g_testCalls);
            good = false;
        }
        if (g_testThis != fakeSelf || g_testDi != fakeDi) {
            logf("[zp] thunk %u: this of argument kwam verkeerd door\n", i);
            good = false;
        }

        // Geval 2: blokkeren, dus het origineel niet aanroepen.
        g_enabled = true;
        g_forceBlock = 1;
        g_testCalls = 0;
        drift = zp_test_call(kDetour[i], fakeSelf, fakeDi);
        if (drift != 0) {
            logf("[zp] thunk %u: stack verschoof %d bytes bij blokkeren\n", i, drift);
            good = false;
        }
        if (g_testCalls != 0) {
            logf("[zp] thunk %u: origineel werd toch aangeroepen\n", i);
            good = false;
        }

        g_forceBlock = 0;
        *kOriginal[i] = savedOriginal;
    }

    g_enabled = savedEnabled;
    return good;
}

// Goedkope plausibiliteitstoets. Een volledige VirtualQuery per schade-event
// is te duur, en de faalwijze hier is veilig: bij een onbetrouwbare pointer
// zeggen we "niet van mij", en dan verloopt de schade gewoon normaal.
//
// De bovengrens komt van het systeem, niet uit mijn hoofd. Hier stond
// 0x80000000, de klassieke 2 GB-grens voor een 32-bit proces -- maar een
// proces dat als LARGEADDRESSAWARE is gelinkt krijgt op 64-bit Windows de
// volle 4 GB, en de heap van dit spel loopt daar ruim overheen. Elke pointer
// boven de 2 GB werd dus afgekeurd, en dan zegt de eigenaarscontrole bij elk
// object "niet van jou" en blokkeert er nooit iets. Dat is precies het
// symptoom: de hook draait, maar er wordt niets tegengehouden.
//
// GetSystemInfo geeft de echte grens voor dit proces: 0x7FFEFFFF zonder
// LARGEADDRESSAWARE, 0xFFFEFFFF met. Zo blijft de toets streng waar dat kan
// en ruim waar dat moet.
static uintptr_t g_maxAddr = 0x7FFEFFFFu;

static void measureAddressSpace() {
    SYSTEM_INFO si;
    memset(&si, 0, sizeof(si));
    GetSystemInfo(&si);
    const uintptr_t hi = (uintptr_t)si.lpMaximumApplicationAddress;
    if (hi > 0x10000) g_maxAddr = hi;
}

static inline bool ptrOk(const void* p) {
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000 && v <= g_maxAddr && (v & 3) == 0;
}

// Welke schadetypes zijn wapens en welke zijn besturing van de engine?
//
// Dit onderscheid is niet cosmetisch. Object::kill() is de opruimfunctie van
// de engine en loopt over hetzelfde pad als een kogel:
//
//   void Object::kill() {
//       DamageInfo d;
//       d.in.m_damageType = DAMAGE_UNRESISTABLE;
//       d.in.m_amount     = getBodyModule()->getMaxHealth();
//       attemptDamage( &d );
//   }
//
// Blokkeer je dat ook, dan kan een parachute zichzelf niet meer opruimen en
// blijft hij boven het slagveld hangen. Hetzelfde geldt voor een transport
// dat lost, voor verdrinken, en voor het opruimen van straling- en gifvelden.
//
// De nummers 0 tot en met 30 betekenen in Generals en Zero Hour hetzelfde;
// pas daarboven lopen de enums uiteen. Deze lijst blijft dus binnen dat
// bereik en werkt voor allebei.
enum : uint32_t {
    DMG_HEALING         = 10,   // zinloos als je toch geen schade oploopt
    DMG_UNRESISTABLE    = 11,   // Object::kill() en scripting
    DMG_WATER           = 12,   // verdrinken, ook parachutes in het water
    DMG_DEPLOY          = 13,   // transport lost zijn lading
    DMG_SURRENDER       = 14,
    DMG_HACK            = 15,
    DMG_DISARM          = 20,   // mijnen en bommen onschadelijk maken
    DMG_HAZARD_CLEANUP  = 21,   // straling- en gifvelden opruimen
    DMG_NUM_TYPES       = 38,
};

static const uint32_t kPassThrough =
    (1u << DMG_HEALING)  | (1u << DMG_UNRESISTABLE) | (1u << DMG_WATER) |
    (1u << DMG_DEPLOY)   | (1u << DMG_SURRENDER)    | (1u << DMG_HACK)  |
    (1u << DMG_DISARM)   | (1u << DMG_HAZARD_CLEANUP);

// Twee eenmalige regels in het log, en samen vertellen ze het hele verhaal
// bij een melding "hij doet niets":
//
//   geen van beide  -> de hook draait niet. Verkeerde vtables, of er is
//                      simpelweg nog niet op je geschoten.
//   alleen "gezien" -> de hook draait wel, maar de controle zegt elke keer
//                      "niet van jou". Dan telt de tabel hieronder waar het
//                      precies strandt.
//   allebei         -> het werkt.
//
// Eenmalig via Interlocked, want de detour draait op de thread van het spel
// en er kunnen er meerdere tegelijk binnenkomen.
static LONG g_sawDamage = 0;
static LONG g_sawBlock = 0;

// Waar de controle afhaakt, geteld per reden. Zonder deze telling is "hij
// blokkeert niets" een doodlopend spoor: elke stap in de keten kan het zijn,
// en ik kan niet bij de machine waar het misgaat.
enum Bail : int {
    BAIL_EVENTS = 0,   // schade langs de hook, met de bescherming aan
    BAIL_PASS,         // doorgelaten schadetype (kill, water, deploy, ...)
    BAIL_SELF,
    BAIL_OBJECT,
    BAIL_TEAM,
    BAIL_PROTO,
    BAIL_OWNER,
    BAIL_PLAYERLIST,
    BAIL_LOCAL,
    BAIL_NOTMINE,
    BAIL_BLOCKED,
    BAIL_COUNT
};
static LONG g_bail[BAIL_COUNT];

// Hoeveel schade er naar welke speler ging. Zonder deze verdeling is "niet
// van jou" een dood spoor: dan is niet te zien of alle schade naar een
// vijand ging (dan klopt de keten en werd er gewoon niet op jou geschoten)
// of dat de schade over meerdere spelers spreidt maar nooit bij jou uitkomt
// (dan klopt onze notie van "jij" niet).
static LONG g_ownerHist[17];   // 0..15 = spelernummer, 16 = onbekend

// De hele keten van het eerste schade-event, stap voor stap bewaard. Een
// telling zegt waar het strandt; deze adressen zeggen waarom. Alles wat niet
// gelezen kon worden blijft nul.
struct ChainSample {
    void* self = nullptr;
    void* object = nullptr;
    void* team = nullptr;
    void* proto = nullptr;
    void* owner = nullptr;
    void* playerList = nullptr;
    void* local = nullptr;
    uint32_t damageType = 0xFFFFFFFFu;
    bool  filled = false;
};
static ChainSample g_first;
static LONG        g_firstTaken = 0;

// Een voorbeeld van de laatste stap, ook als het niet het eerste event was:
// zo is te zien of het om twee geldige maar verschillende spelers gaat.
static void* g_sampleOwner = nullptr;
static void* g_sampleLocal = nullptr;
static uint32_t g_sampleType = 0xFFFFFFFFu;

static inline int bump(Bail b) { InterlockedIncrement(&g_bail[b]); return 0; }

extern "C" int zp_should_block(void* self, void* damageInfo) {
    if (g_forceBlock) return 1;         // alleen tijdens de zelfcontrole
    if (!g_enabled) return 0;
    InterlockedIncrement(&g_bail[BAIL_EVENTS]);
    if (InterlockedExchange(&g_sawDamage, 1) == 0)
        logf("[zp] eerste schade langs de hook gezien.\n");
    if (!damageInfo) return 0;          // het origineel returnt hier zelf ook

    // Het eerste event leggen we stap voor stap vast. Alleen deze ene keer,
    // want dit draait op de thread van het spel.
    const bool record = InterlockedExchange(&g_firstTaken, 1) == 0;
    if (record) g_first.self = self;

    if (!ptrOk(self)) return bump(BAIL_SELF);

    // Is dit besturing in plaats van een wapen? Dan doorlaten, anders breekt
    // de objectlevenscyclus van het spel.
    //
    // Lukte het niet om de indeling van DamageInfo te meten, dan is
    // damageTypeOffset nul en blokkeren we alles. Dat is het oude gedrag: de
    // bescherming blijft werken, maar de glitches komen terug.
    if (g_r.damageTypeOffset) {
        const uint32_t dt =
            *(const uint32_t*)((const char*)damageInfo + g_r.damageTypeOffset);
        g_sampleType = dt;
        if (record) g_first.damageType = dt;
        if (dt < DMG_NUM_TYPES && (kPassThrough & (1u << dt)))
            return bump(BAIL_PASS);
    }

    const char* s = (const char*)self;

    void* obj = *(void**)(s + g_r.thisToObject);
    if (record) g_first.object = obj;
    if (!ptrOk(obj)) return bump(BAIL_OBJECT);

    void* team = *(void**)((const char*)obj + g_r.objectToTeam);
    if (record) g_first.team = team;
    if (!ptrOk(team)) return bump(BAIL_TEAM);

    void* proto = *(void**)((const char*)team + g_r.teamToProto);
    if (record) g_first.proto = proto;
    if (!ptrOk(proto)) return bump(BAIL_PROTO);

    void* owner = *(void**)((const char*)proto + g_r.protoToPlayer);
    if (record) g_first.owner = owner;
    if (!ptrOk(owner)) return bump(BAIL_OWNER);

    void* pl = *(void**)g_r.playerListGlobal;
    if (record) g_first.playerList = pl;
    if (!ptrOk(pl)) return bump(BAIL_PLAYERLIST);

    void* local = *(void**)((const char*)pl + g_r.localPlayerOffset);
    if (record) { g_first.local = local; g_first.filled = true; }
    if (!ptrOk(local)) return bump(BAIL_LOCAL);

    // Welke speler is dit? m_playerIndex staat op een offset die bij alle
    // zestien spelers klopte, dus dit nummer is te vertrouwen als het in
    // bereik ligt.
    int ownerIdx = -1;
    if (g_r.playerIndexOffset) {
        const uint32_t v =
            *(const uint32_t*)((const char*)owner + g_r.playerIndexOffset);
        if (v < 16) ownerIdx = (int)v;
    }
    InterlockedIncrement(&g_ownerHist[ownerIdx < 0 ? 16 : ownerIdx]);

    // -1 = de lokale speler van de engine, -2 = iedereen (test),
    // 0..15 = een vast nummer dat je zelf koos.
    const int32_t mode = g_shared ? g_shared->protectPlayer : -1;
    bool mine;
    if (mode == -2)      mine = true;
    else if (mode >= 0)  mine = (ownerIdx == mode);
    else                 mine = (owner == local);

    if (!mine) {
        g_sampleOwner = owner;
        g_sampleLocal = local;
        return bump(BAIL_NOTMINE);
    }
    InterlockedIncrement(&g_bail[BAIL_BLOCKED]);
    if (g_shared) g_shared->blockedCount++;
    if (InterlockedExchange(&g_sawBlock, 1) == 0)
        logf("[zp] eerste schade geblokkeerd -- het werkt.\n");
    return 1;
}

// ----------------------------------------------------------------- de hook --

// Wat er per gehookte vtable te onthouden valt om hem terug te kunnen zetten.
struct Patch {
    void** slot = nullptr;      // &vtable[0]
    void*  original = nullptr;  // wat daar stond
    void*  detour = nullptr;    // wat wij erin zetten
    bool   named = false;       // dit is de op naam bevestigde ActiveBody
};
static Patch    g_patch[ZP_MAX_BODY_VTABLES];
static uint32_t g_patchCount = 0;

// Plaatst een hook op slot 0 van elke bevestigde body-vtable. Geeft terug
// hoeveel er gelukt zijn; nul betekent mislukt.
static uint32_t installHooks() {
    g_patchCount = 0;
    for (const BodyVtable& b : g_r.bodies) {
        if (g_patchCount >= ZP_MAX_BODY_VTABLES) break;
        const uint32_t i = g_patchCount;
        void** slot = (void**)b.vtable;

        DWORD prot = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) {
            logf("[zp] 0x%08x: slot niet schrijfbaar, overgeslagen\n",
                 (unsigned)b.vtable);
            continue;
        }
        *kOriginal[i] = *slot;
        *slot = kDetour[i];
        VirtualProtect(slot, sizeof(void*), prot, &prot);

        g_patch[i].slot     = slot;
        g_patch[i].original = *kOriginal[i];
        g_patch[i].detour   = kDetour[i];
        g_patch[i].named    = b.nameConfirmed;
        g_patchCount++;
    }
    if (g_patchCount) FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return g_patchCount;
}

static void removeHook() {
    if (!g_hooked) return;
    for (uint32_t i = 0; i < g_patchCount; ++i) {
        Patch& p = g_patch[i];
        if (!p.slot || !p.original) continue;
        DWORD prot = 0;
        if (!VirtualProtect(p.slot, sizeof(void*), PAGE_READWRITE, &prot)) continue;
        // Alleen terugzetten als onze detour er nog staat. Zit er intussen
        // iets anders, dan is dat van iemand anders en blijft het van hem.
        if (*p.slot == p.detour) *p.slot = p.original;
        VirtualProtect(p.slot, sizeof(void*), prot, &prot);
        p.slot = nullptr;
    }
    g_patchCount = 0;
    g_hooked = false;
}

// Wat de detour de afgelopen seconden zag. Wordt een paar seconden na de
// eerste schade een keer afgedrukt, en daarna alleen nog als er iets
// verandert dat ertoe doet.
static void dumpBails() {
    static const char* kName[BAIL_COUNT] = {
        "schade-events", "doorgelaten type", "self ongeldig",
        "Object ongeldig", "Team ongeldig", "TeamPrototype ongeldig",
        "eigenaar ongeldig", "PlayerList ongeldig", "lokale speler ongeldig",
        "niet van jou", "GEBLOKKEERD"
    };
    logf("[zp] wat de hook zag:\n");
    for (int i = 0; i < BAIL_COUNT; ++i) {
        const LONG n = g_bail[i];
        if (!n && i != BAIL_EVENTS && i != BAIL_BLOCKED) continue;
        logf("[zp]   %-24s %ld\n", kName[i], (long)n);
    }
    if (g_bail[BAIL_NOTMINE])
        logf("[zp]   voorbeeld: eigenaar 0x%08x, jij 0x%08x\n",
             (unsigned)(uintptr_t)g_sampleOwner,
             (unsigned)(uintptr_t)g_sampleLocal);

    // De verdeling over de spelers. Hier is in een oogopslag te zien of de
    // schade naar een vijand ging of dat jouw nummer gewoon nooit voorkomt.
    {
        bool any = false;
        for (int i = 0; i < 17; ++i) if (g_ownerHist[i]) any = true;
        if (any) {
            logf("[zp]   schade per speler:\n");
            for (int i = 0; i < 17; ++i) {
                if (!g_ownerHist[i]) continue;
                if (i == 16) {
                    logf("[zp]     onbekend   %ld\n", (long)g_ownerHist[i]);
                    continue;
                }
                logf("[zp]     speler %-2d  %-6ld 0x%08x%s\n", i,
                     (long)g_ownerHist[i], (unsigned)g_r.players[i],
                     i == g_r.localIndex ? "   <-- volgens ons ben jij dit" : "");
            }
        }
    }
    if (g_shared && g_shared->protectPlayer != -1)
        logf("[zp]   beschermd wordt nu: %s\n",
             g_shared->protectPlayer == -2 ? "alle spelers (test)"
                                           : "een vast spelernummer");
    if (g_sampleType != 0xFFFFFFFFu)
        logf("[zp]   laatst geziene schadetype: %u\n", (unsigned)g_sampleType);
    logf("[zp]   adresgrens van dit proces: 0x%08x\n", (unsigned)g_maxAddr);

    // De keten van het eerste event, stap voor stap. Een telling zegt waar
    // het strandt, deze adressen zeggen waarom: een nul betekent dat die stap
    // niet meer gelezen is, en een adres boven de grens hierboven verklaart
    // meteen waarom de toets hem afwees.
    if (g_firstTaken) {
        logf("[zp]   eerste keten: self 0x%08x -> Object 0x%08x -> Team 0x%08x\n",
             (unsigned)(uintptr_t)g_first.self,
             (unsigned)(uintptr_t)g_first.object,
             (unsigned)(uintptr_t)g_first.team);
        logf("[zp]                 -> Proto 0x%08x -> eigenaar 0x%08x\n",
             (unsigned)(uintptr_t)g_first.proto,
             (unsigned)(uintptr_t)g_first.owner);
        logf("[zp]                 PlayerList 0x%08x, jij 0x%08x, type %u\n",
             (unsigned)(uintptr_t)g_first.playerList,
             (unsigned)(uintptr_t)g_first.local,
             (unsigned)g_first.damageType);
    }
}

// ------------------------------------------------------------ hoofdverloop --

static void banner() {
    logf("zeropatience - health-freeze voor Generals / Zero Hour\n");
    logf("\n");
    logf("Alleen bedoeld voor skirmish en campagne. In een potje tegen andere\n");
    logf("mensen verpest dit hun match, en er zit geen automatische controle\n");
    logf("op netwerkpotjes in. Die zou een adres vereisen dat in deze build\n");
    logf("niet betrouwbaar te vinden is, en een controle die soms werkt is\n");
    logf("erger dan geen controle: dan ga je erop vertrouwen.\n");
    logf("\n");
}

static void status() {
    logf("[zp] onkwetsbaarheid: %s\n", g_enabled ? "AAN" : "uit");
    logf("[zp] hook geplaatst : %s\n", g_hooked ? "ja" : "nee");
    if (!g_r.ok) return;
    logf("[zp] gehookte klassen: %u\n", g_patchCount);
    for (uint32_t i = 0; i < g_patchCount; ++i)
        logf("[zp]   0x%08x  slot 0 -> 0x%08x%s\n",
             (unsigned)(uintptr_t)g_patch[i].slot,
             (unsigned)(uintptr_t)g_patch[i].original,
             g_patch[i].named ? "  (ActiveBody, op naam bevestigd)" : "");
    logf("[zp] this -> Object  %s0x%x\n",
         g_r.thisToObject < 0 ? "-" : "+",
         (unsigned)(g_r.thisToObject < 0 ? -g_r.thisToObject : g_r.thisToObject));
    logf("[zp] this -> health  +0x%x\n", (unsigned)g_r.healthOffset);
    logf("[zp] eigenaarsketen  +0x%x / +0x%x / +0x%x  (%u spelers)\n",
         g_r.objectToTeam, g_r.teamToProto, g_r.protoToPlayer, g_r.chainPlayers);
    if (g_r.damageTypeOffset)
        logf("[zp] schadetype op   DamageInfo+0x%x  (in-grootte 0x%x, %s)\n",
             g_r.damageTypeOffset, g_r.damageInfoInSize,
             g_r.damageInfoInSize == 0x18 ? "Generals" : "Zero Hour");
    else
        logf("[zp] schadetype      niet gemeten; alles wordt geblokkeerd\n");
}

// Een poging: offsets bepalen en de hook plaatsen. Losgetrokken van de
// worker zodat hij herhaald kan worden zonder de DLL opnieuw te laden. Dat
// laatste kan namelijk niet: de module zit al in het proces, LoadLibrary
// geeft dan de bestaande terug en DllMain draait niet nog een keer.
static bool attachOnce() {
    InterlockedExchange(&g_sawDamage, 0);
    InterlockedExchange(&g_sawBlock, 0);
    for (int i = 0; i < BAIL_COUNT; ++i) InterlockedExchange(&g_bail[i], 0);
    InterlockedExchange(&g_firstTaken, 0);
    for (int i = 0; i < 17; ++i) InterlockedExchange(&g_ownerHist[i], 0);
    g_first = ChainSample();
    g_sampleOwner = g_sampleLocal = nullptr;
    g_sampleType = 0xFFFFFFFFu;
    setState(STATE_RESOLVING);
    logf("[zp] offsets bepalen...\n");
    // Ruim genomen. Een eerdere poging met 192 MB mislukte: het spel houdt
    // honderden MB aan geheugen vast, dus met een krap budget valt precies de
    // heap met de game-objecten buiten de snapshot. Er waren maar 68
    // ActiveBody-instanties, en die stonden er niet meer in.
    //
    // Het kopieren gebeurt per regio en elke allocatie wordt eerst gereserveerd
    // om te kijken of hij past. Lukt dat niet, dan slaan we die regio over in
    // plaats van het geheugen op te maken.
    g_r = resolveInProcess(1024, logf);
    if (!g_r.ok) {
        logf("\n[zp] MISLUKT: %s\n\n", g_r.error.c_str());
        logf("[zp] Meestal is het timing: het spel moet een kaart geladen\n");
        logf("[zp] hebben en jij moet er zelf eenheden op hebben staan. Een\n");
        logf("[zp] menu, een laadscherm of een intro is te vroeg.\n");
        logf("[zp] Klik op Opnieuw proberen zodra je in het potje zit.\n");
        setState(STATE_FAILED);
        return false;
    }

    logf("\n[zp] gelukt. Bewijs: %u hitpoint-bevestigingen, %u link-bevestigingen,\n"
         "[zp] keten over %u spelers, %zu body-klassen.\n\n",
         g_r.healthConfirmations, g_r.linkConfirmations, g_r.chainPlayers,
         g_r.bodies.size());

    if (!installHooks()) {
        logf("[zp] MISLUKT: kan geen enkel vtable-slot schrijven\n");
        setState(STATE_FAILED);
        return false;
    }
    g_hooked = true;
    g_enabled = true;
    logf("[zp] hook geplaatst, onkwetsbaarheid staat AAN.\n");
    status();

    if (g_shared) {
        g_shared->hooked = 1;
        g_shared->enabled = 1;
        g_shared->bodyVtable   = (uint32_t)g_r.bodyVtable;
        g_shared->objectVtable = (uint32_t)g_r.objectVtable;
        g_shared->thisToObject = g_r.thisToObject;
        g_shared->healthOffset = g_r.healthOffset;
        g_shared->objectToTeam = g_r.objectToTeam;
        g_shared->teamToProto  = g_r.teamToProto;
        g_shared->protoToPlayer = g_r.protoToPlayer;
        g_shared->chainPlayers = g_r.chainPlayers;
        g_shared->healthConfirmations = g_r.healthConfirmations;
        g_shared->linkConfirmations = g_r.linkConfirmations;
    }
    setState(STATE_READY);
    return true;
}

// Draait zolang de hook staat. Keert terug als de GUI om loskoppelen vraagt.
static void runHooked() {
    bool hotkeyDown = false;
    DWORD firstDamageAt = 0;
    int dumps = 0;
    for (;;) {
        Sleep(30);

        // Een paar seconden na de eerste schade de tabel afdrukken, en nog
        // een keer wat later. Niet vaker: dit is diagnose, geen telraam.
        if (g_sawDamage && dumps < 2) {
            const DWORD now = GetTickCount();
            if (!firstDamageAt) firstDamageAt = now;
            const DWORD wait = dumps == 0 ? 4000u : 20000u;
            if (now - firstDamageAt >= wait) {
                dumpBails();
                dumps++;
            }
        }

        if (g_shared) {
            // De GUI mag aan- en uitzetten. Alleen overnemen als het echt
            // verandert, anders overschrijft de GUI elke ronde de sneltoets.
            const bool want = g_shared->enabled != 0;
            if (want != g_enabled) {
                g_enabled = want;
                // Erbij zetten waar het vandaan kwam. Zonder die bron is een
                // log met "uit" als laatste regel niet te lezen: dan is niet
                // te zien of het venster of de sneltoets het omzette, en dat
                // is precies het verschil tussen een bedieningsfout en een
                // bug.
                logf("[zp] onkwetsbaarheid: %s  (via het venster)\n",
                     g_enabled ? "AAN" : "uit");
            }

            if (g_shared->requestUnhook) {
                g_enabled = false;
                removeHook();
                g_shared->hooked = 0;
                g_shared->enabled = 0;
                g_shared->requestUnhook = 0;
                setState(STATE_DETACHED);
                logf("[zp] hook verwijderd op verzoek van de GUI.\n");
                return;
            }
        }

        // De sneltoets is instelbaar omdat de voor de hand liggende toetsen
        // bezet zijn: F12 is standaard Steam's screenshot en F10 opent het
        // venstermenu van Windows.
        const uint32_t vk = g_shared ? g_shared->hotkeyVk : VK_F10;
        if (vk) {
            const bool down = (GetAsyncKeyState((int)vk) & 0x8000) != 0;
            if (down && !hotkeyDown) {
                g_enabled = !g_enabled;
                if (g_shared) g_shared->enabled = g_enabled ? 1 : 0;
                logf("[zp] onkwetsbaarheid: %s  (via sneltoets 0x%02x)\n",
                     g_enabled ? "AAN" : "uit", (unsigned)vk);
            }
            hotkeyDown = down;
        } else {
            hotkeyDown = false;
        }
    }
}

// Wacht tot de GUI om een nieuwe poging vraagt. Zonder gedeeld geheugen is er
// niemand die dat kan vragen, en dan houdt het hier op.
static bool waitForRetry() {
    if (!g_shared) return false;
    for (;;) {
        Sleep(100);
        if (!g_shared->requestRetry) continue;
        g_shared->requestRetry = 0;

        // Het log leeghalen, anders staat de oude mislukking er nog boven en
        // is niet te zien welke regels bij deze poging horen.
        g_shared->logLength = 0;
        g_shared->log[0] = 0;
        return true;
    }
}

static DWORD WINAPI worker(LPVOID) {
    if (!openShared()) {
        // Zonder gedeeld geheugen is er geen GUI om naartoe te praten, dus
        // dan maar een console. Stil mislukken is geen optie.
        AllocConsole();
        SetConsoleTitleA("zeropatience");
        g_console = GetStdHandle(STD_OUTPUT_HANDLE);
        logf("[zp] geen gedeeld geheugen; terugval op dit venster.\n");
    }
    banner();
    measureAddressSpace();

    // De thunkcontrole hoeft maar een keer: die hangt aan de build, niet aan
    // wat er in het spel gebeurt. Faalt hij, dan valt er niets te herhalen.
    setState(STATE_CHECKING);
    logf("[zp] thunk controleren...\n");
    if (!selfCheckThunk()) {
        logf("\n[zp] AFGEBROKEN: de aanroepconventie van de thunk klopt niet.\n");
        logf("[zp] Er wordt niets gehookt. Dit zou het spel laten crashen.\n\n");
        setState(STATE_FAILED);
        return 0;
    }
    logf("[zp] thunk in orde: doorgeven en blokkeren laten de stack terecht.\n\n");

    for (;;) {
        if (attachOnce()) runHooked();
        if (!waitForRetry()) return 0;
        banner();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // Het echte werk hoort niet in DllMain: daar geldt de loader-lock.
        CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_enabled = false;
        removeHook();
    }
    return TRUE;
}
