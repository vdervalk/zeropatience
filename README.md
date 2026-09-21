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
> Het is dus aan jou om dit uit te laten in multiplayer. De sneltoets (standaard
> F9) schakelt het uit zonder het spel te herstarten.

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
| 4 | Comfortinstellingen (zoom, FPS) | **geschrapt, zie hieronder** |

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

### Zeven body-klassen, dus zeven vtables

Er is niet één body-klasse maar zeven, en elke klasse heeft zijn eigen vtable:

```
ActiveBody
  StructureBody          <- alle gebouwen
    HiveStructureBody
  UndeadBody
  HighlanderBody
  ImmortalBody
InactiveBody
```

`StructureBody` en `ImmortalBody` overschrijven `attemptDamage` helemaal niet.
In hun vtable staat dus hetzelfde functie-adres als in die van `ActiveBody` --
maar het is een **andere tabel**, en een hook op slot 0 van de ene raakt de
andere niet.

Een eerdere versie hookte precies één tabel: de eerste kandidaat die de
poolnaam "ActiveBody" opleverde. Dat verklaart het symptoom waarmee dit aan
het licht kwam: na het laden van een opgeslagen potje leek de trainer niet meer
te werken, terwijl het log een geslaagde resolutie liet zien. De hook stond er
wel, alleen op de tabel van de andere helft van je bezit.

Zoeken doen we niet op naam, want dat hoeft niet. Zodra de dubbele link bekend
is, levert een steekproef Objecten ze allemaal op:

```
voor elk Object:
    body = *(Object + objectToBody)
    als *(body + thisToObject) == Object:        <- link sluit heen en terug
        vtable = *body                           <- dit is een body-vtable
```

Wat daar uitkomt is per definitie een body-vtable met het juiste subobject.
Elke gevonden tabel krijgt zijn eigen thunk met zijn eigen bewaarde origineel,
want de klassen die `attemptDamage` wél overschrijven hebben elk een ander
origineel om naar door te geven.

`InactiveBody` slaan we bewust over: die heeft geen hitpoints en gaat alleen
dood aan `DAMAGE_UNRESISTABLE`, en dat type laat de detour toch al door.

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

## Comfortinstellingen: geschrapt

Er zat een tijdje een zoom- en een FPS-instelling in. Die zijn **verwijderd**
nadat ze twee keer een harde reset van de pc nodig maakten. Wat hier staat is
er niet meer; het staat er zodat niemand, ik incluis, het nog eens probeert
zonder te weten wat er is gebeurd.

De trainer doet nu weer één ding: health-freeze.

### Wat er is geprobeerd, en waarom het misging

**Poging 1: een los `GameData.ini`.** Zero Hour startte niet meer op. Dat
bestand bestaat al binnen het `.big`-archief; een los bestand met die naam
wint daarvan en gooit de rest van het blok weg.

```cpp
if (path1) ini.load(path1, INI_LOAD_OVERWRITE, pXfer );  // Default\GameData.ini
if (path2) ini.load(path2, INI_LOAD_OVERWRITE, pXfer );  // GameData.ini
```

Die tests kijken naar de pointer, niet naar het bestand. Het zijn
string-literals, dus altijd waar, en een ontbrekend bestand gooit
`INI_CANT_OPEN_FILE`.

**Poging 2: schrijven in `TheGlobalData`.** Geen enkel effect. Het zijn
startwaarden, die het spel eenmalig kopieert:

```cpp
View::init()          m_maxHeightAboveGround = TheGlobalData->m_maxCameraHeight;
GameEngine::init()    setFramesPerSecondLimit( TheGlobalData->m_framesPerSecondLimit );
```

**Poging 3: schrijven in die kopieen.** Het spel crashte, en de pc moest
eraan te pas. Twee fouten: `GameEngine::m_maxFPS` werd alleen op zijn vorm
herkend (een gok met een schrijfactie erachter), en van de View werd het
heap-adres onthouden, terwijl vrijgegeven geheugen zijn oude inhoud houdt en
een vingerafdruk dus kan blijven kloppen voor een blok dat allang van iets
anders is.

**Poging 4: de camera verankerd op `View *TheTacticalView`**, de globale
pointer die de engine zelf bijhoudt, met de vtable en de vingerafdruk als
controle voor elke schrijfactie, en niets toegepast tenzij er actief voor
gekozen werd. **Ook dit gaf een harde reset.** Daarmee is de conclusie niet
"de laatste aanwijzing was nog niet scherp genoeg" maar iets fundamentelers.

### Wat hieruit te leren valt

De health-freeze schrijft **één pointer** in een vtable, op een adres dat
uit drie onafhankelijke aanwijzingen komt (poolnaam, hitpoint-patroon,
dubbele link), en die verandering is omkeerbaar. Dat werkt al weken.

De comfortinstellingen schreven **waarden in gewone datavelden** van objecten
die alleen aan hun inhoud te herkennen zijn. Het verschil lijkt klein en is
het niet:

- Een vtable-pointer die fout is, crasht meteen en zichtbaar. Een float of
  een int die fout is, gaat ergens anders stuk, later, en op een plek die
  niets met de schrijfactie te maken heeft.
- De health-freeze schrijft **één keer**. De comfortinstellingen schreven
  **elke tik opnieuw**, want het spel zette ze terug. Een adres dat vandaag
  klopt en over tien seconden niet meer, wordt dan tien keer per seconde een
  nieuwe kans op schade.
- Een verkeerd adres is niet te detecteren zonder het te gebruiken. Er is
  geen manier om vanaf deze kant te controleren of een schrijfactie goed
  terechtkwam, en de enige testmachine is die van de gebruiker.

Wie dit opnieuw wil proberen: doe het niet met een schrijfactie die zichzelf
herhaalt, en niet op een object dat alleen op zijn inhoud te herkennen is.
De INI-route (poging 1) is wel werkbaar, mits je het originele
`GameData.ini` uit `INI.big` of `INIZH.big` haalt, je regels **toevoegt** en
het resultaat terugzet. Dan raakt de trainer het geheugen niet aan. Reken wel
op een mismatch in netwerkpotjes, want `GameData.ini` telt mee in de
INI-checksum.

## De trainer gebruiken

### Bouwen

```sh
sudo apt-get install mingw-w64
make trainer
```

Levert `build/zp-trainer.exe` (de GUI), `build/zp-freeze.dll` en
`build/zp-inject.exe` (de console-injector, voor als de GUI niet meewerkt).
Alles 32-bit, want dat is het spel ook, en statisch gelinkt, dus er hoeft
niets naast te staan.

### Draaien

1. Start het spel en **laad een skirmish** met een paar eigen units. In een
   menu bestaan de structuren niet en kan de DLL niets bepalen.
2. Zet `zp-trainer.exe` en `zp-freeze.dll` in dezelfde map.
3. Draai `zp-trainer.exe` **als administrator**.

De GUI herkent het draaiende spel zelf en zet de naam in de statusregel.
Op **Koppelen** gebeurt, in deze volgorde:

| | |
|---|---|
| **Thunk controleren** | De aanroepconventie wordt eerst tegen een nep-vtable geprobeerd. Klopt de stack niet, dan wordt er niets gehookt. |
| **Offsets bepalen** | Duurt een paar seconden. Zelfde afleiding als de probe. |
| **Hook plaatsen** | Eén pointer in de vtable, omkeerbaar. |

Daarna schakelt dezelfde knop tussen **AAN** en **UIT**, en doet de sneltoets
hetzelfde zonder alt-tabben. Die is instelbaar (standaard **F9**) omdat de
voor de hand liggende toetsen bezet zijn: F12 is Steam's screenshot en F10
opent het venstermenu van Windows.

Onder **Details** zitten het log en **Hook verwijderen**, dat de
oorspronkelijke vtable-pointer terugzet.

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

### Als de hook wel staat maar niets blokkeert

Het log zegt dit zelf. Een paar seconden nadat er voor het eerst schade langs
de hook komt, verschijnt een tabel:

```
[zp] wat de hook zag:
[zp]   schade-events            412
[zp]   doorgelaten type          18
[zp]   niet van jou             394
[zp]   GEBLOKKEERD                0
[zp]   eerste keten: self 0x... -> Object 0x... -> Team 0x...
```

Wat de regels betekenen:

- **niet van jou** bij alles — de keten loopt wel, maar komt op een andere
  speler uit dan jij. Er is dan alleen op vijandelijke eenheden geschoten, of
  de laatste stap van de keten klopt niet. De voorbeeldregel met `eigenaar` en
  `jij` laat het verschil zien.
- **een van de "ongeldig"-regels** — daar knapt de keten. Het adres in de
  regel `eerste keten` bij die stap zegt waarom: nul betekent dat er niets
  stond, en een adres boven `adresgrens van dit proces` betekent dat de
  plausibiliteitstoets hem afwees.
- **doorgelaten type** bij alles — dan wordt alle schade als besturing gezien
  en klopt de gemeten indeling van `DamageInfo` niet.
- **geen tabel** — er is nooit schade langs de hook gekomen. Dan staan de
  hooks op de verkeerde tabellen, of er is simpelweg niet op je geschoten.

### Als alles op "niet van jou" uitkomt

Dan loopt de keten wel, maar komt hij nooit bij jouw speler uit. Het log
splitst dat uit per speler:

```
[zp]   schade per speler:
[zp]     speler 2   367    0x18f6575c
[zp]     speler 4   0      0x190a57bc   <-- volgens ons ben jij dit
```

- **alle schade op een ander nummer, nul op dat van jou** -- er is alleen op
  vijandelijke eenheden geschoten, of onze notie van "jij" klopt niet.
- **schade verdeeld over meerdere nummers, maar nooit dat van jou** -- dan
  klopt onze notie van "jij" niet.

Achter *Details* staat **Beschermen**. Daar kun je dat overrulen:

- *jouw speler (automatisch)* -- de standaard: de lokale speler zoals de
  engine hem zelf kent.
- *alle spelers (alleen als test)* -- beschermt iedereen. Gaat de teller nu
  wel lopen, dan werkt de hook en zit de fout alleen in wie "jij" is. Je kunt
  er niet mee spelen: de vijand is dan ook onkwetsbaar.
- *alleen speler N* -- vast op een nummer uit de tabel hierboven.

De spelernummers zelf zijn niet gegokt. `PlayerList` maakt de zestien spelers
met `NEW Player(i)` en de constructor zet `m_playerIndex = i`, dus er is
precies één offset waar bij alle zestien spelers hun eigen positie staat.
Zestien keer achter elkaar kloppen is geen toeval.

### Als koppelen mislukt

De trainer weigert liever dan dat hij gokt. Mislukt de afleiding, dan wordt
er niets gehookt en zegt het log welke stap het niet haalde.

Klik dan op **Opnieuw proberen**. Dat is geen nieuwe injectie: de DLL zit al
in het proces en wacht op een verzoek. Opnieuw injecteren zou ook niets doen,
want `LoadLibrary` geeft voor een module die al geladen is de bestaande
terug, en `DllMain` draait niet nog een keer. Een eerdere versie liet je
daardoor met een dood venster achter en de enige uitweg was het spel
herstarten.

De gebruikelijke oorzaak is timing. De afleiding heeft nodig dat er een
kaart geladen is, dat jij er zelf eenheden op hebt staan, en dat er een
tegenstander is: de eigenaarsketen moet bij jou uitkomen **en** over meer dan
een speler spreiden. Een menu, een laadscherm of een intro is te vroeg.

**Generals Challenge** is geen skirmish maar een campagnekaart
(`m_gameMode == GAME_SINGLE_PLAYER` met `m_isChallengeCampaign`). De keten
werkt er hetzelfde, maar zulke kaarten hebben vaak extra spelers die niets
bezitten, zoals de plaatshouder `"ThePlayer"` die ontwerpers gebruiken om
relaties in scripts te zetten. Daarom wordt er nu uit 256 objecten
bemonsterd in plaats van 64: met een kleine steekproef kan het gebeuren dat
er van jouw eigen eenheden niets in zit.

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
  slot en verder nullen;
- **twee body-klassen** in plaats van één, met eigen vtables maar hetzelfde
  `attemptDamage` in slot 0 -- precies de situatie waarin een trainer die één
  tabel hookt de helft van je bezit mist;
- **de echo van de buurman**: een geheugenpool geeft blokken van vaste grootte
  uit, dus dezelfde vier hitpoint-floats zijn ook te lezen vanuit het volgende
  object, op de echte offset plus de stap. Die schaduw haalde in het testdoel
  meer variatie dan het echte veld (hij kijkt in objecten van gemengde
  klassen) en verdrong het. De afstand verraadt hem: hij ligt een heel object
  verderop, en daar kan geen veld van dít object meer staan.

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
src/freeze/dll.cpp     de vtable-hook, de sneltoets en de comfortinstellingen
src/freeze/resolve.*   dezelfde afleiding, maar binnen het spelproces
src/inject/main.cpp    console-injector
src/gui/main.cpp       de GUI
src/common/shared.h    gedeeld geheugen tussen GUI en DLL
tests/fake_game.cpp    synthetisch testdoel
tests/run_test.sh      testrunner
docs/research.md       bevindingen uit de EA-broncode, met verwijzingen
```
