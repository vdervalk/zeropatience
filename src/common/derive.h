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
//   Int     m_playerCount;        // 1..16, aantal IN GEBRUIK
//   Player *m_players[16];        // ALTIJD alle zestien gevuld
//
// De constructor alloceert alle zestien spelers en init() initialiseert ze
// allemaal; m_playerCount zegt alleen hoeveel er meedoen aan dit potje. Een
// eerdere versie eiste dat de ongebruikte slots NULL waren, en vond daardoor
// in een echt potje nooit iets.
//
// Wat overblijft is nog steeds een sterk patroon: zestien verschillende
// pointers die allemaal dezelfde vtable delen, voorafgegaan door een int van
// 1 tot 16, met een m_local die gelijk is aan een van de eerste zoveel.
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
// Een vptr is een woord dat naar het image wijst en waarvan de eerste slots
// naar uitvoerbare code wijzen. De meest voorkomende zijn in een skirmish de
// game-objecten. Werkt zonder RTTI.
//
// Het aantal gecontroleerde slots is bewust meer dan een: met slechts een
// slot glipt er opvulling doorheen. In een echte meting dook 0x00555555 op
// met 2244 "instanties", puur omdat een blok 0x55-opvulling toevallig naar
// uitvoerbaar geheugen wees.
struct VtableCount {
    uint64_t vtable = 0;
    uint64_t count = 0;
};
std::vector<VtableCount> vtableHistogram(const Target& t, size_t topN);

// Adressen waar een bepaalde vptr staat, dus de instanties zelf.
//
// spread verspreidt de steekproef over het hele geheugen in plaats van de
// eerste maxHits in adresvolgorde te nemen. Dat is geen kosmetiek: de
// laagstgelegen instanties zijn vroeg gealloceerd en vaak van hetzelfde type,
// wat de variatiemeting op hitpoints kunstmatig laag houdt.
std::vector<uint64_t> instancesOf(const Target& t, uint64_t vtable, size_t maxHits,
                                  bool spread = false);

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
//
// requireDistinct sluit A en B van dezelfde klasse uit. Dat is geen detail:
// Object heeft m_next en m_prev, en zo'n dubbelgelinkte lijst levert perfecte
// wederzijdse verwijzingen op die de echte Object <-> BodyModule-relatie
// wegdrukken. Object en de body-module zijn verschillende klassen, dus voor
// die zoektocht mag A nooit gelijk zijn aan B.
std::vector<LinkPair> findDoubleLinks(const Target& t,
                                      const std::vector<uint64_t>& vtables,
                                      size_t instancesPerVtable,
                                      int32_t reachA,
                                      int32_t reachB,
                                      bool requireDistinct);

// --- de body-module herkennen aan zijn inhoud ---------------------------
//
// Sterker dan welke structurele truc ook: alleen een body-module heeft vier
// opeenvolgende floats die zich als hitpoints gedragen, en vrijwel elke
// instantie heeft ze op dezelfde offset. We scoren kandidaat-vtables op die
// consistentie in plaats van op naam of op positie in het histogram.
// Een hoog aandeel alleen is niet genoeg. Een veld dat in elke instantie
// 1.0 / 1.0 / 1.0 / 1.0 bevat haalt ook 100%, en in een echte meting deden
// zestien vtables dat tegelijk. Echte hitpoints varieren per objecttype, dus
// er moeten meerdere verschillende max-waarden voorkomen en die moeten de
// orde van grootte van hitpoints hebben.
struct HealthVtable {
    uint64_t vtable = 0;
    int32_t  offset = 0;
    uint32_t confirmations = 0;
    uint32_t sampled = 0;
    uint32_t distinctMax = 0;
    uint32_t damaged = 0;
    float    medianMax = 0;
    double   ratio = 0.0;       // confirmations / sampled
    double   score = 0.0;       // aandeel gewogen met de variatie
    bool     passed = false;    // haalde de drempels voor variatie en orde van grootte
    const char* reject = "";    // zo niet: waarom niet
};
//
// Geeft altijd een regel per kandidaat terug, ook als die de drempels niet
// haalt. Een lege uitslag is een doodlopend spoor; een uitslag met redenen
// is te lezen en daarmee te herstellen.
std::vector<HealthVtable> findHealthVtables(const Target& t,
                                            const std::vector<uint64_t>& candidates,
                                            size_t samplesPerVtable,
                                            int32_t fromOffset,
                                            int32_t toOffset);

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
    uint32_t distinctMax = 0;   // hoeveel verschillende max-waarden er voorkomen
    uint32_t damaged = 0;       // instanties waar current < max
    float    sampleCurrent = 0;
    float    sampleMax = 0;
    float    medianMax = 0;
};
std::vector<HealthBlock> findHealthBlocks(const Target& t,
                                          const std::vector<uint64_t>& bodyObjects,
                                          int32_t fromOffset,
                                          int32_t toOffset);

// --- klassen op naam aanwijzen -----------------------------------------
//
// De engine registreert elke pool onder een naam:
//
//   MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( Object,     "ObjectPool" )
//   MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( ActiveBody, "ActiveBody" )
//
// Die namen staan als string in de binary, want createMemoryPool krijgt ze
// als argument. Dat maakt ze tot het enige anker dat niet op statistiek
// steunt: we zoeken de string, dan de code die ernaar verwijst, en vlak
// daarbij de constructor die de vtables wegschrijft.
//
// Dit is nodig omdat een histogram-top nooit volstaat: de meest voorkomende
// klassen zijn kleine, talrijke objecten, en de klasse die we zoeken valt
// daar makkelijk buiten.
struct NameAnchor {
    struct Candidate {
        uint64_t vtable = 0;
        int32_t  distance = 0;     // afstand van de xref tot de vtable-verwijzing
        uint32_t xrefsSeen = 0;    // hoeveel xrefs deze kandidaat in bereik hadden
        uint64_t instances = 0;    // live instanties in het geheugen
    };
    std::string            name;
    std::vector<uint64_t>  stringAddrs;
    uint32_t               xrefCount = 0;
    std::vector<Candidate> candidates;
};

std::vector<NameAnchor> findNameAnchors(const Target& t,
                                        const std::vector<std::string>& names,
                                        int32_t radius,
                                        const std::map<uint64_t, uint64_t>& instanceCounts,
                                        uint64_t minInstances);

// Volledig vtable-histogram als map, zodat instantietellingen elders
// opzoekbaar zijn zonder opnieuw te scannen.
std::map<uint64_t, uint64_t> vtableCounts(const Target& t);

// Hexdump rond een instantie, met labels relatief aan 'anchor'. Voor
// handmatige inspectie van velden die de heuristiek niet dekt.
std::string hexDump(const Target& t, uint64_t anchor, int32_t fromOff, int32_t toOff);

} // namespace zp
