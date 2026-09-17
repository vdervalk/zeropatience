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

#include <windows.h>
#include <cstdarg>
#include <cstdio>

using namespace zp;

// ------------------------------------------------------------------ status --

static Resolved   g_r;
static bool       g_enabled = false;
static bool       g_hooked = false;
static HANDLE     g_console = nullptr;

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

extern "C" int zp_should_block(void* self, void* damageInfo) {
    if (g_forceBlock) return 1;         // alleen tijdens de zelfcontrole
    if (!g_enabled) return 0;
    if (!damageInfo) return 0;          // het origineel returnt hier zelf ook
    if (!ptrOk(self)) return 0;

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

    return owner == local ? 1 : 0;
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
    logf("\n");
    logf("  zeropatience - health-freeze voor Generals / Zero Hour\n");
    logf("  ------------------------------------------------------\n");
    logf("  F10  onkwetsbaarheid aan of uit\n");
    logf("  F11  status tonen\n");
    logf("  F12  hook verwijderen en dit venster loskoppelen\n");
    logf("\n");
    logf("  Alleen bedoeld voor skirmish en campagne. In een potje tegen\n");
    logf("  andere mensen verpest dit hun match.\n");
    logf("\n");
    logf("  LET OP: er zit geen automatische controle op netwerkpotjes in.\n");
    logf("  Die zou een adres vereisen dat nog niet betrouwbaar te vinden is,\n");
    logf("  en een controle die soms werkt is erger dan geen controle. Het is\n");
    logf("  dus aan jou om dit uit te laten in multiplayer.\n");
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
}

static DWORD WINAPI worker(LPVOID) {
    AllocConsole();
    SetConsoleTitleA("zeropatience");
    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    banner();

    logf("[zp] thunk controleren...\n");
    if (!selfCheckThunk()) {
        logf("\n[zp] AFGEBROKEN: de aanroepconventie van de thunk klopt niet.\n");
        logf("[zp] Er wordt niets gehookt. Dit zou het spel laten crashen.\n\n");
        return 0;
    }
    logf("[zp] thunk in orde: doorgeven en blokkeren laten de stack terecht.\n\n");

    logf("[zp] offsets bepalen...\n");
    // Bewust bescheiden: dit is een 32-bit proces dat zelf al honderden MB
    // gebruikt. Minder geheugen betekent minder instanties in de steekproef,
    // en dat is hier prima: de afleiding heeft tientallen instanties nodig,
    // geen duizenden.
    g_r = resolveInProcess(192, logf);
    if (!g_r.ok) {
        logf("\n[zp] MISLUKT: %s\n\n", g_r.error.c_str());
        logf("[zp] Meest waarschijnlijke oorzaak: geinjecteerd terwijl je in een\n");
        logf("[zp] menu zat. Laad eerst een skirmish met een paar eigen units,\n");
        logf("[zp] en injecteer dan.\n");
        return 0;
    }

    logf("\n[zp] gelukt. Bewijs: %u hitpoint-bevestigingen, %u link-bevestigingen,\n"
         "[zp] keten over %u spelers.\n\n",
         g_r.healthConfirmations, g_r.linkConfirmations, g_r.chainPlayers);

    if (!installHook()) {
        logf("[zp] MISLUKT: kan het vtable-slot niet schrijven\n");
        return 0;
    }
    g_hooked = true;
    g_enabled = true;
    logf("[zp] hook geplaatst, onkwetsbaarheid staat AAN. F10 schakelt.\n");
    status();

    bool f10 = false, f11 = false, f12 = false;
    for (;;) {
        Sleep(30);

        bool now10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (now10 && !f10) {
            g_enabled = !g_enabled;
            logf("[zp] onkwetsbaarheid: %s\n", g_enabled ? "AAN" : "uit");
        }
        f10 = now10;

        bool now11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (now11 && !f11) status();
        f11 = now11;

        bool now12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (now12 && !f12) {
            g_enabled = false;
            removeHook();
            logf("[zp] hook verwijderd. Je kunt dit venster sluiten.\n");
            return 0;
        }
        f12 = now12;
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
