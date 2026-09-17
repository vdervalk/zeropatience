// derive.h - offsets afleiden uit een draaiend spel in plaats van ze te raden.
//
// Elke afleiding hier heeft een controleerbare invariant. We accepteren nooit
// een offset omdat hij "plausibel" is; alleen omdat een structuur zichzelf
// bevestigt (een dubbele link, een teller die bij een array past, een keten
// die uitkomt op een al bekende pointer).
#pragma once

#include "target.h"
#include "rtti.h"
#include <map>

namespace zp {

// --- ThePlayerList ------------------------------------------------------
//
//   Player *m_local;
//   Int     m_playerCount;        // 1..16
//   Player *m_players[16];        // eerste m_playerCount gevuld, rest NULL
//
// m_local moet gelijk zijn aan een van de gevulde slots. Dat maakt het
// patroon zelf-valideerbaar: toeval dat hieraan voldoet is verwaarloosbaar.
struct PlayerListHit {
    uint64_t              addr = 0;        // adres van m_local
    uint32_t              arrayOffset = 0; // afstand van m_local tot m_players[0]
    uint32_t              playerCount = 0;
    int                   localIndex = -1;
    uint64_t              localPlayer = 0;
    std::vector<uint64_t> players;
    uint64_t              playerVtable = 0; // gedeelde vptr van alle Players
};
std::vector<PlayerListHit> findPlayerLists(const Target& t);

// --- vtable-histogram ---------------------------------------------------
//
// Een vptr is een woord dat naar het image wijst en waarvan het eerste slot
// naar uitvoerbare code wijst. De meest voorkomende zijn in een skirmish de
// game-objecten. Werkt zonder RTTI.
struct VtableCount {
    uint64_t vtable = 0;
    uint64_t count = 0;
};
std::vector<VtableCount> vtableHistogram(const Target& t, size_t topN);

// Adressen waar een bepaalde vptr staat, dus de instanties zelf.
std::vector<uint64_t> instancesOf(const Target& t, uint64_t vtable, size_t maxHits);

// --- dubbele links ------------------------------------------------------
//
// A op offset oa bevat een pointer naar B, en B op offset ob bevat een
// pointer terug naar A. Dat is precies de Object <-> BodyModule-relatie, en
// het is nauwelijks te vervalsen.
//
// De offsets zijn signed en gelden vanaf het adres waar de vptr staat. Dat is
// geen detail: bij meervoudige overerving ligt het BodyModuleInterface-
// subobject achteraan in ActiveBody, terwijl ObjectModule::m_object vooraan
// staat. Vanaf de 'this' die de hook binnenkrijgt is m_object dus negatief.
struct LinkPair {
    uint64_t vtableA = 0;
    int32_t  offsetAtoB = 0;
    uint64_t vtableB = 0;
    int32_t  offsetBtoA = 0;
    uint32_t confirmations = 0;   // aantal instanties dat dit bevestigt
};
std::vector<LinkPair> findDoubleLinks(const Target& t,
                                      const std::vector<uint64_t>& vtables,
                                      size_t instancesPerVtable,
                                      int32_t reachA,
                                      int32_t reachB);

// --- eigenaarsketen -----------------------------------------------------
//
// Object -> m_team -> Team -> m_proto -> TeamPrototype -> m_owningPlayer,
// uitkomend op een pointer die al in ThePlayerList staat. Drie onbekende
// offsets, maar het eindpunt is bekend, dus de keten valideert zichzelf.
struct OwnerChain {
    uint32_t objectToTeam = 0;
    uint32_t teamToProto = 0;
    uint32_t protoToPlayer = 0;
    uint32_t confirmations = 0;
};
std::vector<OwnerChain> findOwnerChains(const Target& t,
                                        const std::vector<uint64_t>& objectInstances,
                                        const std::vector<uint64_t>& knownPlayers,
                                        uint32_t maxObjectOffset,
                                        uint32_t maxTeamOffset,
                                        uint32_t maxProtoOffset);

// --- gezondheidsvelden --------------------------------------------------
//
// ActiveBody heeft vier opeenvolgende floats:
//   m_currentHealth, m_prevHealth, m_maxHealth, m_initialHealth
// waarbij current <= max en max doorgaans gelijk is aan initial.
// De offset is signed en geldt vanaf het meegegeven adres, zodat we hem
// meteen kunnen uitdrukken in de 'this' die de detour krijgt.
struct HealthBlock {
    int32_t  offset = 0;
    uint32_t confirmations = 0;
    float    sampleCurrent = 0;
    float    sampleMax = 0;
};
std::vector<HealthBlock> findHealthBlocks(const Target& t,
                                          const std::vector<uint64_t>& bodyObjects,
                                          int32_t fromOffset,
                                          int32_t toOffset);

// Hexdump rond een instantie, met labels relatief aan 'anchor'. Voor
// handmatige inspectie van velden die de heuristiek niet dekt.
std::string hexDump(const Target& t, uint64_t anchor, int32_t fromOff, int32_t toOff);

} // namespace zp
