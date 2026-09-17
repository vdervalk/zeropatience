// resolve.h - de offsets bepalen binnen het spel zelf, bij injectie.
//
// Dit hergebruikt dezelfde afleidingscode als de probe. Dat is opzet: die
// code is getest tegen een synthetische adresruimte en tegen een echt
// Windows-proces, en hij heeft zich op vijf echte metingen laten corrigeren.
// Een tweede, losse implementatie zou dat allemaal opnieuw moeten verdienen.
//
// Waarom niet gewoon vaste offsets uit het proberapport? Omdat de heap-
// adressen per potje verschillen. De vtable-adressen liggen wel vast (deze
// build heeft geen ASLR), maar ThePlayerList staat elke keer elders. Zelf
// opzoeken is daarmee niet alleen robuuster maar ook simpelweg nodig.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

namespace zp {

struct Resolved {
    bool        ok = false;
    std::string error;

    // Vast in het image, dus stabiel tussen sessies.
    uintptr_t bodyVtable = 0;      // ActiveBody, subobject BodyModuleInterface
    uintptr_t attemptDamage = 0;   // slot 0 van die vtable
    uintptr_t objectVtable = 0;

    // Offsets vanaf de 'this' die de detour binnenkrijgt.
    int32_t thisToObject = 0;      // ObjectModule::m_object, verwacht negatief
    int32_t healthOffset = 0;      // ActiveBody::m_currentHealth, verwacht positief

    // Eigenaarsketen, offsets vanaf het begin van Object.
    uint32_t objectToTeam = 0;
    uint32_t teamToProto = 0;
    uint32_t protoToPlayer = 0;

    // De globale pointer naar ThePlayerList, plus waar m_local daarin staat.
    // De PlayerList zelf verhuist per potje; deze globale pointer niet, dus
    // hiermee blijft de hook geldig als je een nieuw potje start.
    uintptr_t playerListGlobal = 0;
    uint32_t  localPlayerOffset = 0;

    // Onderbouwing, zodat het log laat zien hoe hard het bewijs is.
    uint32_t healthConfirmations = 0;
    uint32_t linkConfirmations = 0;
    uint32_t chainPlayers = 0;
    uint64_t bodyInstances = 0;
};

// Draait de volledige afleiding in het huidige proces. Kost een paar seconden
// en tijdelijk geheugen voor de snapshot; snapshotBudgetMB begrenst dat.
Resolved resolveInProcess(uint64_t snapshotBudgetMB,
                          void (*log)(const char* fmt, ...));

} // namespace zp
