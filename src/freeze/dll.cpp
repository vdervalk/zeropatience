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
    return true;
}

static void setState(uint32_t st) {
    if (g_shared) g_shared->state = st;
}

extern "C" {
void* g_original = nullptr;             // de echte attemptDamage
int   zp_should_block(void* self, void* damageInfo);
void  zp_detour();                      // gedefinieerd in assembly, hieronder

// Alleen voor de zelfcontrole van de thunk, hieronder.
int   g_forceBlock = 0;
void* g_testThis = nullptr;
void* g_testDi = nullptr;
int   g_testCalls = 0;
void  zp_test_original();
int   zp_test_call(void* fn, void* self, void* di);
}

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
__asm__(
    ".text\n"
    ".globl _zp_detour\n"
    "_zp_detour:\n"
    "  pushl %ebp\n"
    "  movl  %esp, %ebp\n"
    "  pushl %ecx\n"                  /* this bewaren; de call hieronder klobbert ecx */
    "  pushl 8(%ebp)\n"               /* damageInfo */
    "  pushl %ecx\n"                  /* this */
    "  call  _zp_should_block\n"
    "  addl  $8, %esp\n"
    "  movl  -4(%ebp), %ecx\n"        /* this herstellen */
    "  testl %eax, %eax\n"
    "  jnz   1f\n"
    "  pushl 8(%ebp)\n"               /* niet geblokkeerd: doorgeven aan het origineel */
    "  call  *_g_original\n"          /* dat doet zelf ret 4 */
    "1:\n"
    "  movl  %ebp, %esp\n"
    "  popl  %ebp\n"
    "  ret   $4\n"
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

// Probeert de thunk uit met een nep-origineel. Geeft true als zowel het
// doorgeven als het blokkeren klopt en de stack in beide gevallen terecht is.
static bool selfCheckThunk() {
    void* savedOriginal = g_original;
    const bool savedEnabled = g_enabled;

    char fakeSelf[64] = {0};
    char fakeDi[16] = {0};
    bool good = true;

    g_original = (void*)&zp_test_original;

    // Geval 1: niet blokkeren, dus doorgeven aan het origineel.
    g_enabled = false;
    g_forceBlock = 0;
    g_testCalls = 0;
    g_testThis = nullptr;
    g_testDi = nullptr;
    int drift = zp_test_call((void*)&zp_detour, fakeSelf, fakeDi);
    if (drift != 0) {
        logf("[zp] thunk-controle: stack verschoof %d bytes bij doorgeven\n", drift);
        good = false;
    }
    if (g_testCalls != 1) {
        logf("[zp] thunk-controle: origineel %d keer aangeroepen, verwacht 1\n",
             g_testCalls);
        good = false;
    }
    if (g_testThis != fakeSelf || g_testDi != fakeDi) {
        logf("[zp] thunk-controle: this of argument kwam verkeerd door\n");
        good = false;
    }

    // Geval 2: blokkeren, dus het origineel niet aanroepen.
    g_enabled = true;
    g_forceBlock = 1;
    g_testCalls = 0;
    drift = zp_test_call((void*)&zp_detour, fakeSelf, fakeDi);
    if (drift != 0) {
        logf("[zp] thunk-controle: stack verschoof %d bytes bij blokkeren\n", drift);
        good = false;
    }
    if (g_testCalls != 0) {
        logf("[zp] thunk-controle: origineel werd toch aangeroepen\n");
        good = false;
    }

    g_forceBlock = 0;
    g_original = savedOriginal;
    g_enabled = savedEnabled;
    return good;
}

// Goedkope plausibiliteitstoets. Een volledige VirtualQuery per schade-event
// is te duur, en de faalwijze hier is veilig: bij een onbetrouwbare pointer
// zeggen we "niet van mij", en dan verloopt de schade gewoon normaal.
static inline bool ptrOk(const void* p) {
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000 && v < 0x80000000u && (v & 3) == 0;
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

extern "C" int zp_should_block(void* self, void* damageInfo) {
    if (g_forceBlock) return 1;         // alleen tijdens de zelfcontrole
    if (!g_enabled) return 0;
    if (!damageInfo) return 0;          // het origineel returnt hier zelf ook
    if (!ptrOk(self)) return 0;

    // Is dit besturing in plaats van een wapen? Dan doorlaten, anders breekt
    // de objectlevenscyclus van het spel.
    //
    // Lukte het niet om de indeling van DamageInfo te meten, dan is
    // damageTypeOffset nul en blokkeren we alles. Dat is het oude gedrag: de
    // bescherming blijft werken, maar de glitches komen terug.
    if (g_r.damageTypeOffset) {
        const uint32_t dt =
            *(const uint32_t*)((const char*)damageInfo + g_r.damageTypeOffset);
        if (dt < DMG_NUM_TYPES && (kPassThrough & (1u << dt))) return 0;
    }

    const char* s = (const char*)self;

    void* obj = *(void**)(s + g_r.thisToObject);
    if (!ptrOk(obj)) return 0;

    void* team = *(void**)((const char*)obj + g_r.objectToTeam);
    if (!ptrOk(team)) return 0;

    void* proto = *(void**)((const char*)team + g_r.teamToProto);
    if (!ptrOk(proto)) return 0;

    void* owner = *(void**)((const char*)proto + g_r.protoToPlayer);
    if (!ptrOk(owner)) return 0;

    void* pl = *(void**)g_r.playerListGlobal;
    if (!ptrOk(pl)) return 0;

    void* local = *(void**)((const char*)pl + g_r.localPlayerOffset);
    if (!ptrOk(local)) return 0;

    if (owner != local) return 0;
    if (g_shared) g_shared->blockedCount++;
    return 1;
}

// -------------------------------------------------------- comfortinstellingen --
//
// Eerste poging: de waarden in TheGlobalData zetten. Dat had geen effect in
// het spel, en de broncode zegt waarom. Beide instellingen worden daar
// eenmalig uit gekopieerd:
//
//   View::init()          m_maxHeightAboveGround = TheGlobalData->m_maxCameraHeight;
//   GameEngine::init()    setFramesPerSecondLimit( TheGlobalData->m_framesPerSecondLimit );
//
// en daarna kijkt het spel alleen nog naar die kopieen. Een kaart die al
// geladen is trekt zich dus niets aan van GlobalData.
//
// Nu wordt allebei geschreven: de kopie voor het huidige potje, en de
// INI-waarde voor alles wat er later uit wordt afgeleid.
//
// Een uitzondering is m_useFpsLimit: die wordt wel elke lus opnieuw gelezen,
// dus "onbeperkt" werkt altijd, ook zonder dat we m_maxFPS hebben gevonden.

static uint32_t g_qolLastCamera = 0xFFFFFFFFu;
static uint32_t g_qolLastFps = 0xFFFFFFFFu;

static void* globalData() {
    if (!g_r.qolOk || !g_r.globalDataPtr) return nullptr;
    void* p = *(void**)g_r.globalDataPtr;
    return ptrOk(p) ? p : nullptr;
}

// Is dit adres nu echt te lezen? De adressen zijn bij het injecteren gemeten
// en er kan sindsdien een kaart geladen zijn. Een bereikcontrole alleen is
// hier niet genoeg, want een vrijgegeven pagina ligt in hetzelfde bereik.
static bool readableAt(const void* p, size_t n) {
    if (!ptrOk(p)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    const uintptr_t endOfRegion = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (uintptr_t)p + n <= endOfRegion;
}

// Het adres van m_maxHeightAboveGround, elke keer opnieuw afgeleid vanaf de
// globale pointer. Nooit een onthouden heap-adres: vrijgegeven geheugen
// houdt zijn oude inhoud, dus een vingerafdruk kan blijven kloppen terwijl
// het blok allang van iets anders is.
static float* viewMaxHeight(uint32_t i) {
    if (i >= g_r.viewCount || !g_r.viewGlobal[i]) return nullptr;

    if (!readableAt((const void*)g_r.viewGlobal[i], sizeof(void*))) return nullptr;
    const char* obj = *(const char* const*)g_r.viewGlobal[i];
    if (!ptrOk(obj)) return nullptr;

    // Dezelfde klasse als bij het injecteren? Een View die opnieuw is
    // aangemaakt heeft dezelfde vtable; iets anders heeft dat niet.
    if (!readableAt(obj, sizeof(uintptr_t))) return nullptr;
    if (*(const uintptr_t*)obj != g_r.viewVtable[i]) return nullptr;

    const float* f = (const float*)(obj + g_r.viewBlockOffset[i]);
    if (!readableAt(f, 24)) return nullptr;
    if (f[0] != 1.3f || f[1] != 0.2f) return nullptr;
    if (f[3] != g_r.viewMinHeight) return nullptr;
    return (float*)(f + 2);
}

// Zet de gewenste waarden, of herstelt het origineel bij nul. Draait elke
// tik, want een nieuwe kaart zet de View-kopie terug.
static void applyQol() {
    if (!g_shared || !g_r.qolOk) return;

    const uint32_t wantCam = g_shared->qolCameraMax;
    const uint32_t wantFps = g_shared->qolFpsLimit;
    const bool changed = (wantCam != g_qolLastCamera) || (wantFps != g_qolLastFps);
    const bool unlimited = (wantFps == kFpsUnlimited);

    char* base = (char*)globalData();

    // --- camerahoogte ---------------------------------------------------
    if (base && g_r.offMaxCameraHeight) {
        float* p = (float*)(base + g_r.offMaxCameraHeight);
        const float want = wantCam ? (float)wantCam : g_r.origMaxCameraHeight;
        if (*p != want) *p = want;      // voor een View die nog moet komen
    }
    uint32_t viewsWritten = 0;
    for (uint32_t i = 0; i < g_r.viewCount; ++i) {
        float* p = viewMaxHeight(i);
        if (!p) continue;
        const float want = wantCam ? (float)wantCam : g_r.viewOrigMax[i];
        if (*p != want) *p = want;
        ++viewsWritten;
    }
    if (changed)
        logf("[zp] camerahoogte: %.0f (%u camera%s)\n",
             wantCam ? (double)wantCam : (double)g_r.origMaxCameraHeight,
             viewsWritten, viewsWritten == 1 ? "" : "'s");

    // --- beeldsnelheid ---------------------------------------------------
    //
    // m_useFpsLimit is een Bool, dus een byte. Er vier schrijven zou de vlag
    // ernaast overschrijven.
    if (base && g_r.offUseFpsLimit) {
        uint8_t* p = (uint8_t*)(base + g_r.offUseFpsLimit);
        const uint8_t want = unlimited ? 0u
                           : wantFps   ? 1u
                                       : g_r.origUseFpsLimit;
        if (*p != want) *p = want;
    }

    if (changed)
        logf("[zp] fps-begrenzing: %s\n", unlimited ? "uit" : "aan");

    g_qolLastCamera = wantCam;
    g_qolLastFps = wantFps;
}

static void restoreQol() {
    if (!g_r.qolOk) return;

    for (uint32_t i = 0; i < g_r.viewCount; ++i) {
        float* p = viewMaxHeight(i);
        if (p) *p = g_r.viewOrigMax[i];
    }
    char* base = (char*)globalData();
    if (!base) return;
    if (g_r.offMaxCameraHeight)
        *(float*)(base + g_r.offMaxCameraHeight) = g_r.origMaxCameraHeight;
    if (g_r.offFramesPerSecondLimit)
        *(int32_t*)(base + g_r.offFramesPerSecondLimit) = g_r.origFramesPerSecondLimit;
    if (g_r.offUseFpsLimit)
        *(uint8_t*)(base + g_r.offUseFpsLimit) = g_r.origUseFpsLimit;
}

// ----------------------------------------------------------------- de hook --

static bool installHook() {
    void** slot = (void**)g_r.bodyVtable;     // attemptDamage is slot 0
    DWORD prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) return false;
    g_original = *slot;
    *slot = (void*)&zp_detour;
    VirtualProtect(slot, sizeof(void*), prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

static void removeHook() {
    if (!g_hooked || !g_original) return;
    void** slot = (void**)g_r.bodyVtable;
    DWORD prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) return;
    *slot = g_original;
    VirtualProtect(slot, sizeof(void*), prot, &prot);
    g_hooked = false;
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
    logf("[zp] vtable          0x%08x  (slot 0 -> 0x%08x)\n",
         (unsigned)g_r.bodyVtable, (unsigned)g_r.attemptDamage);
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

    setState(STATE_CHECKING);
    logf("[zp] thunk controleren...\n");
    if (!selfCheckThunk()) {
        logf("\n[zp] AFGEBROKEN: de aanroepconventie van de thunk klopt niet.\n");
        logf("[zp] Er wordt niets gehookt. Dit zou het spel laten crashen.\n\n");
        setState(STATE_FAILED);
        return 0;
    }
    logf("[zp] thunk in orde: doorgeven en blokkeren laten de stack terecht.\n\n");

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
        logf("[zp] Meest waarschijnlijke oorzaak: geinjecteerd terwijl je in een\n");
        logf("[zp] menu zat. Laad eerst een skirmish met een paar eigen units,\n");
        logf("[zp] en injecteer dan.\n");
        setState(STATE_FAILED);
        return 0;
    }

    logf("\n[zp] gelukt. Bewijs: %u hitpoint-bevestigingen, %u link-bevestigingen,\n"
         "[zp] keten over %u spelers.\n\n",
         g_r.healthConfirmations, g_r.linkConfirmations, g_r.chainPlayers);

    if (!installHook()) {
        logf("[zp] MISLUKT: kan het vtable-slot niet schrijven\n");
        setState(STATE_FAILED);
        return 0;
    }
    g_hooked = true;
    g_enabled = true;
    logf("[zp] hook geplaatst, onkwetsbaarheid staat AAN.\n");
    status();

    // Comfortinstellingen aanbieden als de resolutie ze heeft gevonden.
    if (g_shared && g_r.qolOk) {
        g_shared->qolAvailable = 1;
        g_shared->qolOrigCameraMax = (uint32_t)(g_r.origMaxCameraHeight + 0.5f);
        g_shared->qolOrigFpsLimit = (uint32_t)(g_r.origFramesPerSecondLimit < 0
                                               ? 0 : g_r.origFramesPerSecondLimit);
    }

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

    bool hotkeyDown = false;
    for (;;) {
        Sleep(30);

        if (g_shared) {
            // De GUI mag aan- en uitzetten. Alleen overnemen als het echt
            // verandert, anders overschrijft de GUI elke ronde de sneltoets.
            const bool want = g_shared->enabled != 0;
            if (want != g_enabled) {
                g_enabled = want;
                logf("[zp] onkwetsbaarheid: %s\n", g_enabled ? "AAN" : "uit");
            }

            if (g_shared->requestUnhook) {
                g_enabled = false;
                restoreQol();
                removeHook();
                g_shared->hooked = 0;
                g_shared->enabled = 0;
                g_shared->requestUnhook = 0;
                setState(STATE_DETACHED);
                logf("[zp] hook verwijderd op verzoek van de GUI.\n");
                return 0;
            }
        }

        applyQol();

        // De sneltoets is instelbaar omdat de voor de hand liggende toetsen
        // bezet zijn: F12 is standaard Steam's screenshot en F10 opent het
        // venstermenu van Windows.
        const uint32_t vk = g_shared ? g_shared->hotkeyVk : VK_F10;
        if (vk) {
            const bool down = (GetAsyncKeyState((int)vk) & 0x8000) != 0;
            if (down && !hotkeyDown) {
                g_enabled = !g_enabled;
                if (g_shared) g_shared->enabled = g_enabled ? 1 : 0;
                logf("[zp] onkwetsbaarheid: %s\n", g_enabled ? "AAN" : "uit");
            }
            hotkeyDown = down;
        } else {
            hotkeyDown = false;
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // Het echte werk hoort niet in DllMain: daar geldt de loader-lock.
        CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_enabled = false;
        restoreQol();
        removeHook();
    }
    return TRUE;
}
