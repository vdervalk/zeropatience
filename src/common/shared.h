// shared.h - het gedeelde geheugen tussen de DLL in het spel en de GUI.
//
// De DLL maakt de mapping, de GUI opent hem. Die richting is met opzet zo:
// de GUI draait verhoogd (anders kan hij het spel niet openen), het spel
// meestal niet. Windows staat een proces met een hoger integriteitsniveau toe
// om bij objecten van een lager niveau te komen, andersom niet. Zou de GUI de
// mapping maken, dan kon de DLL er niet bij.
//
// Er zit geen synchronisatie op. Dat hoeft ook niet: de GUI schrijft alleen
// enabled en requestUnhook, de DLL schrijft alles behalve die twee, en alle
// velden zijn 32-bit en dus op x86 ondeelbaar te lezen en te schrijven.
#pragma once

#include <cstdint>
#include <cstdio>

namespace zp {

enum : uint32_t {
    SHARED_MAGIC   = 0x5A504652u,   // "ZPFR"
    SHARED_VERSION = 2,
    SHARED_LOG_BYTES = 16384,
};

enum SharedState : uint32_t {
    STATE_STARTING  = 0,
    STATE_CHECKING  = 1,   // thunk controleren
    STATE_RESOLVING = 2,   // offsets bepalen
    STATE_READY     = 3,   // hook staat
    STATE_FAILED    = 4,
    STATE_DETACHED  = 5,   // hook verwijderd, DLL doet niets meer
};

struct Shared {
    uint32_t magic;
    uint32_t version;

    // Door de DLL geschreven.
    uint32_t state;
    uint32_t hooked;
    uint32_t blockedCount;      // aantal geblokkeerde schade-events
    uint32_t bodyVtable;
    uint32_t objectVtable;
    int32_t  thisToObject;
    int32_t  healthOffset;
    uint32_t objectToTeam;
    uint32_t teamToProto;
    uint32_t protoToPlayer;
    uint32_t chainPlayers;
    uint32_t healthConfirmations;
    uint32_t linkConfirmations;
    uint32_t logLength;

    // Door de GUI geschreven.
    uint32_t enabled;           // 0 of 1
    uint32_t requestUnhook;     // 1 = verzoek om de hook te verwijderen

    // 1 = verzoek om het opnieuw te proberen na een mislukking of na
    // loskoppelen. Zonder dit is een mislukte poging definitief: de DLL zit
    // al in het proces, dus opnieuw injecteren doet niets (LoadLibrary geeft
    // de bestaande module terug en DllMain draait niet nog een keer), en de
    // enige uitweg was het spel herstarten.
    uint32_t requestRetry;

    // Virtual-key code van de sneltoets die in het spel schakelt. Instelbaar,
    // want de voor de hand liggende toetsen zijn al bezet: F12 is standaard
    // Steam's screenshot, en F10 opent in Windows het venstermenu. Nul
    // betekent: geen sneltoets, alleen schakelen via de GUI.
    uint32_t hotkeyVk;

    // Wie er beschermd wordt. -1 = de lokale speler, zoals de engine hem zelf
    // kent; -2 = alle spelers, alleen als test; 0..15 = een vast spelernummer.
    //
    // Die laatste twee staan er omdat "van jou" een afgeleide is en dus fout
    // kan zijn. Met alle spelers is in een tel te zien of de hook zelf werkt,
    // en met een vast nummer kun je uit het log aflezen welke speler jij bent
    // en dat instellen, zonder op de afleiding te wachten.
    int32_t  protectPlayer;

    char log[SHARED_LOG_BYTES];
};

// Een naam per spelproces, zodat twee draaiende spellen elkaar niet in de weg
// zitten en een oude mapping niet blijft rondslingeren.
inline void sharedName(uint32_t pid, char* out, size_t n) {
    snprintf(out, n, "Local\\zeropatience.v1.%u", pid);
}

} // namespace zp
