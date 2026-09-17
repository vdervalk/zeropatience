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
