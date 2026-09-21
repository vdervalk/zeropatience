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
#include <vector>

namespace zp {

// Hoeveel vtables we tegelijk kunnen hooken. Zeven body-klassen bestaan er:
// ActiveBody, StructureBody, HiveStructureBody, UndeadBody, HighlanderBody,
// ImmortalBody en InactiveBody. InactiveBody slaan we over (zie resolve.cpp),
// dus zes is genoeg; acht geeft lucht.
enum : uint32_t { ZP_MAX_BODY_VTABLES = 8 };

// Een body-klasse die we gevonden en bevestigd hebben.
//
// Elke klasse heeft zijn eigen vtable, ook als hij attemptDamage niet
// overschrijft. StructureBody erft die functie van ActiveBody: hetzelfde
// functie-adres, maar in een andere tabel. Een hook op de ene tabel raakt de
// andere dus niet, en daarom is dit een lijst en geen enkel adres.
struct BodyVtable {
    uintptr_t vtable = 0;          // subobject BodyModuleInterface
    uintptr_t attemptDamage = 0;   // slot 0 van die vtable
    uint32_t  instances = 0;       // live instanties in het geheugen
    uint32_t  confirmations = 0;   // Objecten met een sluitende dubbele link

    // Waar of dit de vtable is die ook op poolnaam is aangewezen. Voor de
    // andere klassen kunnen we geen naam geven: zie derive.h.
    bool      nameConfirmed = false;
};

struct Resolved {
    bool        ok = false;
    std::string error;

    // Vast in het image, dus stabiel tussen sessies.
    uintptr_t bodyVtable = 0;      // ActiveBody, subobject BodyModuleInterface
    uintptr_t attemptDamage = 0;   // slot 0 van die vtable
    uintptr_t objectVtable = 0;

    // Alle body-klassen die de dubbele-link-toets doorstaan, ActiveBody
    // voorop. Hierop komen de hooks te staan.
    std::vector<BodyVtable> bodies;

    // Offsets vanaf de 'this' die de detour binnenkrijgt.
    int32_t thisToObject = 0;      // ObjectModule::m_object, verwacht negatief
    int32_t healthOffset = 0;      // ActiveBody::m_currentHealth, verwacht positief

    // De weg terug: Object::m_body, gemeten vanaf het begin van Object. Samen
    // met thisToObject vormt dit de dubbele link, en die link is de toets
    // waarmee we een tweede body-klasse herkennen zonder te gokken.
    uint32_t objectToBody = 0;

    // Eigenaarsketen, offsets vanaf het begin van Object.
    uint32_t objectToTeam = 0;
    uint32_t teamToProto = 0;
    uint32_t protoToPlayer = 0;

    // Waar in DamageInfo het schadetype staat, gemeten in plaats van
    // aangenomen. Nul betekent: niet vastgesteld, en dan valt de hook terug
    // op alles blokkeren.
    //
    // Dit is nodig omdat Object::kill() geen schade is maar de opruimfunctie
    // van de engine, en toch over attemptDamage loopt:
    //
    //   void Object::kill() {
    //       DamageInfo d;
    //       d.in.m_damageType = DAMAGE_UNRESISTABLE;
    //       ...
    //       attemptDamage( &d );
    //   }
    //
    // Blokkeer je dat, dan kan een parachute zichzelf niet meer opruimen en
    // blijft hij in beeld hangen.
    uint32_t damageTypeOffset = 0;
    uint32_t damageInfoInSize = 0;   // 0x18 = Generals, 0x40 = Zero Hour

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
