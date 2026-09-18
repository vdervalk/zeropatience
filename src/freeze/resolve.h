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

    // --- comfortinstellingen ---------------------------------------------
    //
    // TheGlobalData staat op de heap maar de pointer ernaartoe staat vast in
    // de schrijfbare data van het image. Die pointer onthouden we, want het
    // object kan per potje verhuizen.
    //
    // De offsets komen uit de INI-veldtabellen van het spel zelf, waar ze
    // naast de naam staan opgeslagen. Niets geraden.
    bool     qolOk = false;
    uint64_t globalDataPtr = 0;         // waar de globale pointer staat
    uint32_t offMaxCameraHeight = 0;
    uint32_t offFramesPerSecondLimit = 0;
    uint32_t offUseFpsLimit = 0;

    // Oorspronkelijke waarden, zodat we ze kunnen terugzetten.
    float    origMaxCameraHeight = 0;
    int32_t  origFramesPerSecondLimit = 0;
    uint8_t  origUseFpsLimit = 0;       // Bool, dus een byte

    // De View-kopie van de camerabegrenzing. GlobalData aanpassen werkt
    // alleen voor een View die nog gemaakt moet worden; een geladen kaart
    // heeft zijn kopie al. Daarom allebei.
    //
    // Bewaard wordt de GLOBALE pointer, niet het adres van de View zelf.
    // Een heap-adres onthouden en daar blijven schrijven is onveilig:
    // vrijgegeven geheugen houdt zijn oude inhoud, dus een vingerafdruk kan
    // blijven kloppen terwijl het blok allang van iets anders is.
    static const uint32_t kMaxViews = 4;
    uint32_t  viewCount = 0;
    uintptr_t viewGlobal[kMaxViews] = {0};   // waar de pointer staat
    uintptr_t viewVtable[kMaxViews] = {0};   // om het object te herkennen
    uint32_t  viewBlockOffset[kMaxViews] = {0};
    float     viewOrigMax[kMaxViews] = {0};
    float     viewMinHeight = 0;             // == m_minCameraHeight

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
