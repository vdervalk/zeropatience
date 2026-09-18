# zeropatience

Health-freeze trainer voor **Command & Conquer: Generals** en **Zero Hour**.

Voor mensen die weinig tijd hebben om te spelen: je eigen units en gebouwen
worden onkwetsbaar, zodat een potje geen half uur micromanagen wordt.

> **Alleen voor skirmish en campagne.** In een online potje zou dit de match
> van je tegenstanders slopen.
>
> Er zit **geen automatische controle** op netwerkpotjes in. Die zou een
> adres vereisen dat in deze build niet betrouwbaar te vinden is, en een
> controle die soms werkt is erger dan geen controle: dan vertrouw je erop.
> Het is dus aan jou om dit uit te laten in multiplayer. F10 schakelt het uit
> zonder het spel te herstarten.

---

## Waar dit project nu staat

| Fase | Onderdeel | Status |
|---|---|---|
| 1 | Onderzoek naar het schadepad in de EA-broncode | **klaar** |
| 1 | `zp-probe`: leest het draaiende spel uit en meet de offsets | **klaar, getest** |
| 1 | Eerste meting op Generals, drie heuristiekfouten hersteld | **klaar** |
| 1 | Tweede meting, vier vervolgfouten hersteld | **klaar** |
| 1 | Derde en vierde meting: sorteerfout en te smalle kandidatenlijst hersteld | **klaar** |
| 1 | Klassen op naam aanwijzen via de memory-pool namen | **klaar, getest** |
| 2 | `zp-freeze.dll`: de damage-hook | **klaar** |
| 2 | `zp-inject.exe` + F10-toggle | **klaar** |
| 2 | Getest in de echte game (Generals) | **klaar** |
| 3 | Compacte GUI met spelherkenning | **klaar** |
| 3 | Zero Hour: dezelfde DLL, getest in het spel | **klaar** |

De trainer bepaalt zijn offsets zelf bij het injecteren, met dezelfde code
die de probe gebruikt. Dat is geen luxe: de vtable-adressen liggen vast
(deze build heeft geen ASLR), maar `ThePlayerList` staat elk potje elders.

**Wat er nu van jou nodig is:** injecteren en kijken of het werkt. Zie
[De trainer gebruiken](#de-trainer-gebruiken).

---

## Hoe het werkt

Het korte antwoord: de game heeft zelf al een god-mode, en die zetten we aan.

Alle schade in Generals loopt door één virtuele functie:

```cpp
void ActiveBody::attemptDamage( DamageInfo *damageInfo )
{
    validateArmorAndDamageFX();
    if( damageInfo == NULL )  return;
    if( m_indestructible )    return;     // <-- dit bestaat al in de engine
    ...
}
```

`m_indestructible` is een bestaande vlag die de engine zelf gebruikt voor
scripted onkwetsbaarheid. We hoeven dus geen onbekend gedrag te verzinnen: de
hook doet precies wat die vlag doet, namelijk vroeg returnen, maar dan alleen
voor objecten die van jou zijn.

Dat is ook waarom dit een **vtable-hook** is en geen klassieke trampoline.
Een trampoline moet instructies decoderen in een binary die we niet kunnen
inspecteren; een vtable-slot is één pointer op een bekend adres. Schrijven om
aan te zetten, terugzetten om uit te zetten. Geen disassembler, geen half
overschreven instructie.

De volledige onderbouwing met broncodeverwijzingen staat in
[`docs/research.md`](docs/research.md).

### De keten die de hook gebruikt

```
this (BodyModuleInterface-subobject)   <- wat de detour binnenkrijgt
  -> m_object            -> Object
       m_body            -> terug naar 'this'    [validatie]
       m_team            -> Team
         m_proto         -> TeamPrototype
           m_owningPlayer-> Player
ThePlayerList.m_local    -> Player               [is dit van mij?]
```

Let op de eerste stap: door meervoudige overerving (`BodyModule : public
BehaviorModule, public BodyModuleInterface`) zit het interface-subobject
achteraan in `ActiveBody`, terwijl `m_object` vooraan staat. Vanaf `this` is
`m_object` dus een **negatieve** offset. Dat klinkt fout maar is correct, en
het is precies het geval waar een naïeve scanner op stukloopt.

---

## Waarom een probe, en niet gewoon vaste offsets

De klassenindeling kennen we uit de door EA vrijgegeven broncode. De exacte
byte-offsets niet: die hangen af van compilerversie, structopvulling en
buildvlaggen van jouw specifieke binary. Offsets gokken levert een trainer op
die crasht of stilletjes niets doet.

De probe meet ze daarom in plaats van ze aan te nemen. De volgorde waarin
hij dat doet is het resultaat van een mislukte eerste meting op een echte
Generals-installatie, die liet zien dat structurele trucs kwetsbaar zijn voor
toeval en inhoudelijke niet:

1. **Klassen worden op naam aangewezen.** De engine registreert elke memory
   pool onder een naam (`"ObjectPool"` voor `Object`, `"ActiveBody"` voor
   `ActiveBody`), en die naam gaat als string naar `createMemoryPool`. De
   probe zoekt die string, dan de code die ernaar verwijst, en vlakbij de
   constructor die de vtables wegschrijft. Dit is de enige aanwijzing die
   niet op statistiek steunt.
2. **De body-module wordt herkend aan zijn inhoud.** Alleen een body-module
   heeft vier opeenvolgende floats die zich als hitpoints gedragen
   (`current <= max`, `max == initial`), en vrijwel elke instantie heeft ze op
   dezelfde offset. Een aandeel richting 100% is nauwelijks toevallig te
   halen.
3. **Pas daarna wordt de structurele link naar `Object` gelegd**, geankerd op
   iets dat al vaststaat. Daarbij moeten de vtables van beide kanten
   verschillen, want `Object` heeft `m_next` en `m_prev` en zo'n
   dubbelgelinkte lijst voldoet perfect aan "wijzen naar elkaar".
4. **De eigenaarsketen** moet uitkomen op een pointer die al in ThePlayerList
   staat.
5. **ThePlayerList** zelf is zestien onderling verschillende pointers die
   dezelfde vtable delen, voorafgegaan door een int van 1 tot 16, met een
   `m_local` die gelijk is aan één van de eerste zoveel. Let op: alle zestien
   slots zijn altijd gevuld, ook de ongebruikte.

Wat er bij de eerste meting misging en waarom, staat uitgeschreven in
[`docs/research.md`](docs/research.md).

Daardoor werkt de probe ook als jouw build zonder RTTI is gecompileerd. RTTI
wordt gebruikt als het er is, maar niets hangt ervan af.

De probe **schrijft niets** naar het spel. Geen geheugenwrites, geen
code-patches, geen injectie. Alleen lezen.

---

## De trainer gebruiken

### Bouwen

```sh
sudo apt-get install mingw-w64
make trainer
```

Levert `build/zp-freeze.dll` en `build/zp-inject.exe`, beide 32-bit, want dat
is het spel ook. Statisch gelinkt, dus er hoeft niets naast te staan.

### Draaien

1. Start het spel en **laad een skirmish** met een paar eigen units. In een
   menu bestaan de structuren niet en kan de DLL niets bepalen.
2. Zet `zp-inject.exe` en `zp-freeze.dll` in dezelfde map.
3. Draai `zp-inject.exe` **als administrator**.

Er verschijnt een venster `zeropatience`. Dat doet, in deze volgorde:

| | |
|---|---|
| **Thunk controleren** | De aanroepconventie wordt eerst tegen een nep-vtable geprobeerd. Klopt de stack niet, dan wordt er niets gehookt. |
| **Offsets bepalen** | Duurt een paar seconden. Zelfde afleiding als de probe. |
| **Hook plaatsen** | Eén pointer in de vtable, omkeerbaar. |

Daarna:

```
F10   onkwetsbaarheid aan of uit
F11   status tonen (welke offsets, hoe hard het bewijs)
F12   hook verwijderen
```

### Werkt dit ook op Zero Hour?

Ja, met dezelfde DLL. Een probe-run op Zero Hour
(`sha256 420fba1d...`, image 6 MB) loste alles op, en de hexdump bevestigt
de structuur byte voor byte: vier vptrs op `0x00 / 0x04 / 0x10 / 0x14`,
`m_object` op `0x0c`, en de hitpoints direct achter `m_damageScalar`. Precies
zoals in Generals, alleen op andere adressen.

Ook de gemeten `DamageInfo` klopte: `sizeof(DamageInfoInput)` kwam uit op
`0x40`. De doorslag gaf een `0x0b` op de plek van `m_damageFXOverride`, want
de broncode zet dat veld standaard op `DAMAGE_UNRESISTABLE` (11).

Zero Hour deelt elk anker waar de trainer op steunt, nagelopen in de
`GeneralsMD/`-tree van de EA-broncode:

| | Generals | Zero Hour |
|---|---|---|
| Poolnamen | `"ObjectPool"`, `"ActiveBody"` | gelijk |
| `Module` | `: MemoryPoolObject, Snapshot` | gelijk |
| `BehaviorModule` | `: ObjectModule, BehaviorModuleInterface` | gelijk |
| `BodyModule` | `: BehaviorModule, BodyModuleInterface` | gelijk |
| `PlayerList` | `m_local`, `m_playerCount`, `m_players[16]` | gelijk |

Zero Hour heeft wel extra velden in `ActiveBody` (subdual damage), maar dat
verschuift alleen offsets, en die worden bij het injecteren gemeten in plaats
van vastgelegd. Er is dus geen tweede trainer, en daarmee ook niets om tussen
te kiezen.

De GUI onderscheidt de twee wel in de statusregel. Dat gaat op het
installatiepad, want beide spellen heten `game.dat` en de procesnaam zegt
dus niets.

### Waarom de thunk zich eerst laat controleren

`attemptDamage` is `__thiscall`: `this` in `ECX`, het argument op de stack,
en de *callee* ruimt dat argument op. Zit daar een fout in de stack-discipline,
dan crasht het spel bij de eerste kogel.

Die code kan hier niet gedraaid worden, want dit is een Linux-bouwmachine
zonder werkende 32-bit Windows-omgeving. De assembly is met de hand
nagelopen in de disassembly, maar dat is geen uitvoering. Daarom controleert
de DLL zichzelf op jouw machine: hij roept de thunk twee keer aan met een
nep-origineel, één keer doorgevend en één keer blokkerend, en meet of de
stack pointer in beide gevallen terechtkomt. Pas als dat klopt raakt hij de
echte vtable aan.

### Wat de hook wel en niet doet

De hook blokkeert **wapens** en laat **besturing van de engine** door. Dat
onderscheid is niet cosmetisch, want `Object::kill()` is de opruimfunctie van
het spel en loopt over hetzelfde pad als een kogel:

```cpp
void Object::kill()
{
    DamageInfo damageInfo;
    damageInfo.in.m_damageType = DAMAGE_UNRESISTABLE;
    damageInfo.in.m_amount     = getBodyModule()->getMaxHealth();
    attemptDamage( &damageInfo );
}
```

Een eerdere versie blokkeerde alles, en toen bleven parachutes na een
paradrop in beeld hangen: de parachute kon zichzelf niet meer opruimen.
Hetzelfde gold voor transporten die lossen, voor verdrinken, en voor het
opruimen van straling- en gifvelden.

Deze types komen door: `HEALING`, `UNRESISTABLE`, `WATER`, `DEPLOY`,
`SURRENDER`, `HACK`, `DISARM` en `HAZARD_CLEANUP`. De nummers 0 tot en met 30
betekenen in Generals en Zero Hour hetzelfde, dus die lijst werkt voor
allebei.

**De prijs:** een script dat `kill()` op jouw eenheid aanroept, werkt weer.
In de campagne is dat waarschijnlijk juist de bedoeling, anders loopt de
missie vast. In skirmish komt het nauwelijks voor.

De offset van het schadetype in `DamageInfo` verschilt tussen de twee
spellen, dus die wordt **gemeten** in plaats van vastgelegd. `DamageInfo`
bestaat uit drie delen die elk van `Snapshot` erven en dus elk een vptr
hebben; de afstand tussen de vptr van `in` en die van `out` is precies
`sizeof(DamageInfoInput)`, en die is ondubbelzinnig: `0x18` in Generals,
`0x40` in Zero Hour. Lukt het meten niet, dan valt de hook terug op alles
blokkeren en zegt dat in het log.

---

## Stap voor stap: de probe

### 1. Bouwen

Op Linux, met MinGW-w64:

```sh
sudo apt-get install mingw-w64
make
```

Dat levert `build/zp-probe64.exe` en `build/zp-probe32.exe`. Ze zijn statisch
gelinkt, dus er hoeft niets naast te staan.

### 2. Controleren dat de binary gezond is

```
zp-probe64.exe --selftest
```

Draait de afleidingslogica tegen een ingebouwde synthetische adresruimte.
Slaagt dit niet, dan is er iets mis met de build en heeft stap 3 geen zin.

### 3. De probe draaien

1. Start Generals of Zero Hour.
2. **Laad een skirmish** en speel tot je een paar eigen units en gebouwen op
   de kaart hebt. In het hoofdmenu bestaan de structuren nog niet en vindt de
   probe niets.
3. Alt-tab naar buiten en draai, **als administrator**:

```
zp-probe64.exe
```

Gebruik `zp-probe64.exe`, ook als het spel 32-bit is: een 64-bit probe kan
een 32-bit proces gewoon uitlezen. `zp-probe32.exe` is er alleen voor een
32-bit Windows.

Bij de EA-uitgave draaien er twee processen: `Generals.exe` is een launcher
die `Game.dat` start, en in `Game.dat` zit de engine. De probe herkent dat
aan het geheugengebruik (de launcher zit op tientallen MB, de engine op
honderden) en kiest zelf de juiste, met een regel erbij over wat hij koos.

Vindt hij het spel niet, of wil je zelf kiezen:

```
zp-probe64.exe --list          zoek het spel op in de lijst
zp-probe64.exe --pid 1234      en gebruik dat nummer
```

**Generals en Zero Hour zijn losse binaries met eigen offsets.** Speel je
beide, draai de probe dan één keer per spel en stuur beide rapporten.

### 4. Het rapport terugsturen

De probe schrijft `zeropatience-probe.txt`. Stuur dat bestand terug. Daarin
staan de build-vingerafdruk, de gevonden vtables en de gemeten offsets.

Daarna wordt fase 2 gebouwd: `zp-freeze.dll` met de damage-hook, een injector
en een F10-toggle.

---

## Wat er in het rapport staat

| Sectie | Waarvoor |
|---|---|
| `BUILD-VINGERAFDRUK` | sha256, PE-timestamp en versie, zodat het profiel aan jouw exacte binary hangt |
| `RTTI-SCAN` | vtables per klasse, als de build RTTI heeft |
| `THEPLAYERLIST` | bevestiging dat het spelerspatroon klopt en wie de lokale speler is |
| `VTABLE-HISTOGRAM` | de meest voorkomende objecttypes, werkt zonder RTTI |
| `DUBBELE LINKS` | de wederzijdse verwijzingen tussen Object en body-module |
| `RESOLUTIE` | **de eigenlijke uitkomst**: welke vtable, welke offsets |
| `GEZONDHEIDSVELDEN` | waar de hitpoints staan, met hoeveel bevestigingen |
| `HEXDUMP` | ruwe bytes, om `m_indestructible` handmatig na te lopen |
| `EIGENAARSKETEN` | Object → Team → TeamPrototype → Player |

Het rapport bevat geheugenadressen en bestandshashes van je game-installatie,
geen persoonlijke gegevens.

---

## Tests

```sh
make test
```

Twee niveaus:

1. **Zelftest** — bouwt een synthetische 32-bit adresruimte in een buffer en
   controleert elke afleiding tegen de bekende juiste antwoorden. Dekt het
   pointerpad dat de echte, 32-bit game raakt.
2. **End-to-end** — `tests/fake_game.cpp` bouwt dezelfde structuurvorm in een
   echt Windows-proces (onder Wine), waarna de probe erop wordt losgelaten en
   de uitkomsten worden vergeleken met wat het testdoel daadwerkelijk heeft
   neergezet.

Beide testdoelen bevatten bewust de valkuilen die een echte meting
blootlegde, want een test die alleen de eigen aanname modelleert dekt de fout
juist toe:

- het interface-subobject achteraan, dus `m_object` op een **negatieve**
  offset en de hitpoints op een positieve;
- een **dubbelgelinkte objectlijst**, die perfecte wederzijdse verwijzingen
  oplevert en de echte relatie verdrong;
- **alle zestien spelerslots gevuld**, ook de ongebruikte;
- een blok **opvulling dat zich voordoet als vtable**, met één uitvoerbaar
  slot en verder nullen.

Wat de tests **niet** dekken: de 32-bit MSVC RTTI-parser, want MinGW zendt
Itanium-ABI RTTI uit en geen MSVC-RTTI. Dat pad is optioneel; als het faalt
neemt de heuristiek het over, en die is wel getest.

---

## Projectindeling

```
src/common/target.*    proceskoppeling, regio's, geheugensnapshot
src/common/rtti.*      MSVC RTTI-scanner (32- en 64-bit)
src/common/derive.*    offsets afleiden met zelf-validerende invarianten
src/common/sha256.h    build-vingerafdruk
src/probe/main.cpp     rapportopbouw
src/probe/selftest.cpp zelftest tegen een synthetische adresruimte
tests/fake_game.cpp    synthetisch testdoel
tests/run_test.sh      testrunner
docs/research.md       bevindingen uit de EA-broncode, met verwijzingen
```
