# Onderzoek: waar zit "health" in Generals / Zero Hour

Alle bevindingen komen uit de door EA vrijgegeven broncode
(`electronicarts/CnC_Generals_Zero_Hour`, map `GeneralsMD/Code/GameEngine`).
De 2025-heruitgave is uit diezelfde code gebouwd, dus de klassenindeling
klopt; alleen de exacte byte-offsets moeten per binary gemeten worden.

## 1. Het schadepad

Alle schade in de game loopt via één virtuele functie:

```cpp
// Include/GameLogic/Module/BodyModule.h
class BodyModuleInterface
{
public:
    virtual void attemptDamage( DamageInfo *damageInfo ) = 0;   // <-- slot 0
    virtual void attemptHealing( DamageInfo *healingInfo ) = 0;
    ...
};
```

De concrete implementatie voor alles wat kapot kan:

```cpp
// Source/GameLogic/Object/Body/ActiveBody.cpp
void ActiveBody::attemptDamage( DamageInfo *damageInfo )
{
    validateArmorAndDamageFX();
    if( damageInfo == NULL )
        return;
    if( m_indestructible )      // <-- de engine heeft zelf al een god-mode
        return;
    ...
}
```

**Dit is het belangrijkste punt van het hele project.** De engine kent al een
onkwetsbaarheidsvlag, die bovenaan `attemptDamage` direct returnt. Het spel
gebruikt die zelf (scripted invulnerability). We hoeven dus geen eigen,
onbekend gedrag te verzinnen: we activeren een codepad dat de game al
dagelijks uitvoert.

Twee manieren om dat te bereiken, beide ondersteund door dit project:

| Methode | Wat het doet | Voordeel | Nadeel |
|---|---|---|---|
| **vtable-hook op slot 0** | Vervangt de `attemptDamage`-pointer in de `BodyModuleInterface`-vtable | Dekt automatisch elk object, ook nieuwgebouwde units. Geen polling. Omkeerbaar met één schrijfactie. | Raakt alle objecten, dus de hook moet zelf eigenaarschap bepalen |
| **`m_indestructible = 1` per object** | Zet de bestaande vlag | Nul code-patching, 100% engine-eigen gedrag | Moet herhaald worden voor nieuwe units |

Dit project hookt de vtable (keuze: betrouwbaarder, geen flikkering) en
gebruikt de `m_indestructible`-semantiek als model voor het detour-gedrag.

### Waarom geen inline detour / trampoline

Een klassieke trampoline vereist instructielengte-decodering op een binary
die we niet kunnen inspecteren. Een vtable-slot is één pointer op een bekend
adres: schrijven, en teruggeven bij uitschakelen. Geen disassembler nodig,
geen halve instructie overschreven, geen crash bij een andere compilerversie.

## 2. Meervoudige overerving (belangrijk voor `this`)

```cpp
class Module        : public MemoryPoolObject, public Snapshot { ... };
class ObjectModule  : public Module { Object *m_object; };
class BodyModule    : public BehaviorModule, public BodyModuleInterface { ... };
class ActiveBody    : public BodyModule { ... };
```

`BodyModule` erft van twee bases, dus een `ActiveBody` heeft meerdere vptrs.
`attemptDamage` staat in de vtable van het **`BodyModuleInterface`-subobject**,
niet in de primaire vtable.

Gevolg voor de hook: de `this` die de detour binnenkomt is de
`BodyModuleInterface`-subobject-pointer, niet het begin van de `ActiveBody`.
Dat is geen probleem, want `Object` bewaart exact diezelfde pointer:

```cpp
// Include/GameLogic/Object.h
BodyModuleInterface* m_body;
```

Daarmee geldt `object->m_body == this`, en dat is een harde
validatie-invariant die de probe gebruikt om offsets te bevestigen.

## 3. De eigenaarsketen

```
BodyModuleInterface* this
  -> (this - colOffset) = ActiveBody*
  -> ObjectModule::m_object      -> Object*
       Object::m_body            -> terug naar 'this'      [validatie]
       Object::m_team            -> Team*
         Team::m_proto           -> TeamPrototype*
           TeamPrototype::m_owningPlayer -> Player*
```

Vergelijken met de lokale speler:

```cpp
// Include/Common/PlayerList.h
class PlayerList : public SubsystemInterface, public Snapshot
{
private:
    Player *m_local;
    Int     m_playerCount;
    Player *m_players[MAX_PLAYER_COUNT];   // MAX_PLAYER_COUNT == 16
};
```

Deze indeling is zelf-valideerbaar en dus zonder RTTI in het geheugen terug
te vinden: een pointer, dan een int tussen 1 en 16, dan 16 pointers waarvan
de eerste `m_playerCount` geldig zijn en de rest NULL, en waarbij `m_local`
gelijk is aan één van die pointers. `m_players[0]` is altijd de neutrale
speler.

## 4. Schadetypes die aandacht vragen

```cpp
// Include/GameLogic/Damage.h
DAMAGE_HEALING       = 10,   // loopt ook door attemptDamage heen
DAMAGE_UNRESISTABLE  = 11,   // scripted "armorproof" schade
DAMAGE_KILLPILOT     = 16,   // maakt voertuig bemanningloos i.p.v. dood
DAMAGE_DEPLOY        = 13,   // transport lost units
DAMAGE_SURRENDER     = 14,
```

`DAMAGE_HEALING` wordt door de detour doorgelaten naar de originele functie.
Blokkeren zou healing-effecten (ambulance, propaganda, reparatiedrone)
stilletjes breken.

De niet-schade-achtige types (`DAMAGE_DEPLOY`, `DAMAGE_KILLPILOT`,
`DAMAGE_SURRENDER`, `DAMAGE_HACK`, `DAMAGE_DISARM`) sturen spel-logica aan in
plaats van hitpoints. Die doorlaten voorkomt vastlopers, bijvoorbeeld een
transport dat zijn lading niet meer kan lossen. Instelbaar in de config.

## 5. `DamageInfo`-indeling (32-bit)

`DamageInfo`, `DamageInfoInput` en `DamageInfoOutput` erven allemaal van
`Snapshot`, die virtuele functies heeft. Elk heeft dus een eigen vptr.

```
DamageInfo
  +0x00  vptr            (DamageInfo)
  +0x04  in.vptr         (DamageInfoInput)
  +0x08  in.m_sourceID
  +0x0C  in.m_sourceTemplate
  +0x10  in.m_sourcePlayerMask
  +0x14  in.m_damageType          <-- de detour leest dit
  ...
```

De probe meet deze offsets na in plaats van ze aan te nemen: `m_damageType`
moet een waarde 0..37 zijn en `m_amount` een plausibele float.

---

# Wat een echte meting op Generals leerde

De eerste probe-run op de Steam-uitgave van het basisspel (`game.dat`,
sha256 `88b03cfb…`, 32-bit, image base `0x400000`) leverde geen bruikbare
offsets op. Drie oorzaken, alle drie fouten in de heuristiek en niet in de
game. Ze staan hier omdat ze verklaren waarom de probe is zoals hij nu is.

## 1. Alle zestien spelerslots zijn altijd gevuld

De scanner eiste dat `m_players[k]` voor `k >= m_playerCount` NULL was. Dat
is aantoonbaar fout:

```cpp
// Generals/Code/GameEngine/Source/Common/RTS/PlayerList.cpp
PlayerList::PlayerList() : m_local(NULL), m_playerCount(0)
{
    for (Int i = 0; i < MAX_PLAYER_COUNT; i++)
        m_players[ i ] = NEW Player( i );
    init();
}

void PlayerList::init()
{
    m_playerCount = 1;
    m_players[0]->init(NULL);
    for (int i = 1; i < MAX_PLAYER_COUNT; i++)
        m_players[i]->init(NULL);
    setLocalPlayer(m_players[0]);
}
```

Alle zestien `Player`-objecten worden in de constructor gealloceerd en door
`init()` geinitialiseerd. `m_playerCount` zegt uitsluitend hoeveel spelers er
aan dit potje meedoen. De eis dat de rest NULL is, kan in een draaiend spel
dus nooit kloppen, en de scanner vond gegarandeerd niets.

Het gecorrigeerde patroon is nog steeds sterk: zestien onderling
verschillende pointers die allemaal dezelfde vtable delen, voorafgegaan door
een int van 1 tot 16, met een `m_local` die gelijk is aan een van de eerste
`m_playerCount` daarvan.

## 2. Object hangt in een dubbelgelinkte lijst

```cpp
// Generals/Code/GameEngine/Include/GameLogic/Object.h
Object *      m_next;
Object *      m_prev;
```

De "dubbele link"-heuristiek zocht naar A en B die naar elkaar wijzen, in de
veronderstelling dat dat de `Object` ↔ `BodyModule`-relatie zou opleveren.
Maar een dubbelgelinkte lijst voldoet daar perfect aan: `A->m_next->m_prev`
is per definitie `A`. In de meting kwamen de sterkste treffers dan ook op
`+0xb4 / +0xb0` van dezelfde vtable naar zichzelf, met 24 van de 24
bevestigingen, en het resultaat was de absurde conclusie dat `ActiveBody` en
`Object` dezelfde vtable hadden.

`Object` en de body-module zijn verschillende klassen, dus de zoektocht eist
nu dat de vtables van A en B verschillen.

## 3. Opvulling die zich voordoet als vtable

Het histogram telde `0x00555555` als vtable met 2244 "instanties". Dat is een
blok opvulbytes; het adres viel toevallig binnen het image en het woord erop
wees toevallig naar uitvoerbaar geheugen. Een controle op alleen slot 0 is te
zwak. Er worden nu vier opeenvolgende slots gecontroleerd.

## Gevolg voor de opzet

De structurele heuristiek bleek kwetsbaar voor toeval. De inhoudelijke niet.
Daarom is de volgorde omgedraaid: de body-module wordt nu eerst herkend aan
het feit dat vrijwel elk van zijn instanties vier opeenvolgende floats heeft
die zich als hitpoints gedragen, op dezelfde offset. Pas daarna wordt de
structurele link naar `Object` gelegd, geankerd op iets dat al vaststaat.

## Bruikbare bijvangst

- **Geen RTTI.** De retailbuild is zonder `/GR` gecompileerd, dus het
  RTTI-pad levert niets op. De heuristiek moet het alleen kunnen.
- **Geen ASLR.** De image base is `0x400000`, gelijk aan de voorkeursbasis in
  de PE-header. De vtable-adressen zijn daarmee stabiel tussen sessies, en de
  DLL kan met vaste RVA's werken in plaats van elke start opnieuw te zoeken.
- **Dit is het basisspel.** Het pad was `Command and Conquer Generals`, dus de
  `Generals/`-tree van de broncode, niet `GeneralsMD/`. Zero Hour is een
  aparte binary met eigen offsets en heeft zijn eigen meting nodig.

---

# Wat de tweede meting leerde

De v2-probe vond ThePlayerList wel, maar strandde daarna: *"Geen wederzijdse
verwijzingen tussen verschillende klassen gevonden."* Twee oorzaken, en de
eerste had ik zelf veroorzaakt.

## 4. Te streng is net zo fout als te los

Om de opvulling uit fout 3 af te vangen, eiste v2 dat vier opeenvolgende
vtable-slots naar code wezen. Dat was overbodig en schadelijk. De echte
valse positief, `0x00555555`, is niet deelbaar door vier; de
uitlijningscontrole alleen had hem al afgevangen.

Wat de vier-slot-eis wél deed, was echte vtables wegsnijden. In de v1-meting
stond `0x9a726c` met 2000 instanties in het histogram, precies de vtable met
de `m_next`/`m_prev`-lijst. In v2 was hij volledig verdwenen.

De controle is nu: uitgelijnd, slot 0 wijst naar code, en dan één van twee
bevestigingen: het adres ligt in alleen-lezen geheugen, óf er is een tweede
slot dat ook naar code wijst. Allebei eisen breekt op binaries die `.rdata`
in een schrijfbare sectie samenvoegen.

## 5. De tegenpartij hoeft niet in de top van het histogram te staan

De zoektocht naar wederzijdse verwijzingen eiste dat *beide* kanten in de
kandidatenlijst stonden, de veertig meest voorkomende vtables. Dat is een
willekeurige grens: valt de tegenpartij er net buiten, dan is het resultaat
nul, ook al bestaat de relatie gewoon. De B-kant hoeft nu alleen een
geloofwaardige vtable te zijn.

## 6. De dubbelgelinkte lijst is een aanwijzing, geen ruis

Fout 2 loste v2 op door zelf-verwijzende links weg te gooien. Dat is het kind
met het badwater. `Object` is juist de klasse *met* een dubbelgelinkte lijst,
dus een vtable die op twee verschillende offsets naar zichzelf wijst is een
positieve vingerafdruk van `Object`.

De probe rapporteert die lijst nu apart en gebruikt hem als bevestiging: een
resolutie waarbij de hitpoint-kandidaat een link heeft naar precies de klasse
die de objectlijst draagt, steunt op twee onafhankelijke aanwijzingen.

## 7. Een hoog aandeel is geen bewijs

In de v2-meting scoorden zestien vtables tegelijk 100% op het
hitpoint-patroon. Dat patroon, vier floats met `current <= max` en
`max == initial`, wordt ook gehaald door elk veld dat overal `1.0 1.0 1.0
1.0` bevat.

Wat hitpoints onderscheidt is variatie: verschillende objecttypes hebben
verschillende max-health. De score weegt nu mee hoeveel verschillende
max-waarden er voorkomen, eist er minstens vier, en eist dat de mediaan
minstens 10 is. Het aantal beschadigde instanties (`current < max`) staat er
als extra aanwijzing bij.
