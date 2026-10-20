# D: PC-port

En native Windows-version av PS1-spelet *D* (Warp/Acclaim, 1995, tre
skivor), byggd från den dekompilerade källkoden i projektet. Ingen
emulator behövs.

**Status:** se `PLAN.md`. Launchern hittar och kontrollerar de tre skivorna
(`.bin/.cue` eller `.chd`). Spelet körs native: filmerna (MDEC och XA-ljud
strömmas från skivan i konsolens takt), titelskärmen och musiken, slottets
platser, inventariet och ljudeffekterna. Skivbytet sker automatiskt med en
virtuell lucka. Bilden kan visas skarp, mjuk, som CRT eller som mjuk TV.
Filmernas färger kan uppsamplas mjukt, och logotyperna kan hoppas över.
Inställningen "Softer bass" (Sound, på som standard) tar bort den djupaste
basen ur ljudeffekterna: skrolljudet i inventariet är en djup duns som kan
få högtalare att skallra. På konsolen hördes den genom TV:ns lilla
högtalare. Filmerna och musiken ändras inte.
Tangentbord eller XInput-handkontroll, och F1 (eller Select+Start) öppnar
inställningarna mitt i spelet. Start pausar när som helst i spelet, även
mitt i en film (utom i introt, på titelskärmen och i den första filmen, där
Start har en funktion i spelet); P, Pause eller R3 (höger spak intryckt)
pausar alltid. Bilden står kvar, och filmen och ljudet fortsätter exakt där
de stannade.

Pausskärmen är en meny: fortsätt, **spara läge** (save state), **ladda
läge**, inställningar och tillbaka till launchern. Det finns åtta platser
med en liten bild, skiva, speltid och datum. "LOAD GAME" i launchern visar
dem: Enter/A startar spelet direkt från ett läge (rätt skiva läggs i), och
Delete-knappen, Del eller X raderar ett (efter en fråga). Spara går när
man ser sig omkring, i filmerna och i inventariet, men inte mitt i ett
pussel eller på titelskärmen. Lägena ligger i `saves/` bredvid `d.exe` (knappt
1 MB styck). De följer med när porten byggs om; bara om spelets kod
(`game.dll`) eller uppbyggnaden av det sparade ändras går de inte att
ladda, och listan säger då varför.

## Du behöver dina egna skivor
Porten innehåller ingen grafik, inget ljud, inga filmer och inga andra
filer från spelet, och ingen BIOS. Du väljer din egen avbild av *D (USA)*
i launchern: "CHOOSE DISCS" (markera en, två eller alla tre filerna med
Ctrl/Skift) eller dra in filerna i fönstret. Väljer du bara en skiva hittas
de andra två automatiskt om de ligger bredvid med samma namn
och ett annat skivnummer, t.ex. `D (USA) (Disc 1).chd` → `(Disc 2)`,
`(Disc 3)`. Det räcker med skiva 1 för att börja: när spelet ber om nästa
skiva och den inte är vald frågar spelfönstret efter filen.

Avbilderna måste ha hela sektorer (`.bin/.cue` eller `.chd` från en sådan),
eftersom filmerna och musiken ligger i XA-sektorer. En `.iso` räcker inte.
Första gången en skiva väljs jämförs dataspårets SHA1 med den kända
avbilden (några sekunder per skiva, i bakgrunden: launchern visar en
snurrande indikator med skivans namn och hur långt den kommit, och PLAY
väntar tills den är klar); sedan räcker filens storlek och tid.

Inställningarna sparas i `settings.ini` bredvid programmet.

## Bygga
Krav: Windows 10/11 och Python 3. Spelkoden kommer från projektet
(`../src`, `../include`), PS1-typerna från PSn00bSDK:s headers i
`../tools/psn00bsdk`, och listan över spelets oinitierade variabler från
`../asm/data/bss.bss.s` (kör `build.py split` i projektet om `asm/` saknas).

```bash
build.bat
```

Första gången laddas kompilatorn (llvm-mingw) och ninja ner till `tools/`
(SHA256-kontrollerade, ca 190 MB). Resultatet blir `build/d.exe`.
`build.bat --debug` ger ett felsökningsbygge. Från projektets rot gör
`runpc.bat` samma sak och startar sedan programmet (`--no-run`: bara bygg,
`--debug`: felsökningsbygge).

## Teknik i korthet
- Två filer: `d.exe` (launchern, fönstret, PS1-biblioteken under
  `src/psx/`) och `game.dll`, spelets dekompilerade kod och data på fast
  adress (0x60000000), som anropar biblioteken i `d.exe` (listan skrivs av
  `tools/exe_exports.py` när det byggs). Sparlägena innehåller adresser
  till spelets kod, så de gäller så länge `game.dll` är densamma: porten
  kan byggas om utan att lägena slutar fungera.
- 32-bitars Windows-program (`--large-address-aware`): PS1-minnet ligger på
  sina ursprungliga adresser (0x80000000), och spelets oinitierade
  variabler ligger där de låg på konsolen (absoluta symboler som
  `configure.py` genererar), så spelkoden körs som den är. Den enda
  ändringen i `../src` är `PC_SAFE_POINT()` överst i huvudloopen (ett tomt
  makro på PS1, se nedan).
- Direct3D 11 för bilden, XInput för handkontroller, XAudio2 för ljud.
  Allt ingår i Windows.
- Sonys PS1-bibliotek som D anropar (82 funktioner: grafik, CD och
  strömning, MDEC, ljud, handkontroll, räknare) skrivs om i C under
  `src/psx/`. Grunden (fönster, GPU, SPU, launcher, skivläsare) kommer från
  PC-porten av American Pool.
- Filmerna: CD:n läser i verklig tid (150 sektorer/s), `stream.c` fogar
  ihop STR-bildrutorna och skickar XA-ljudet till `xa.c`; skivläsaren läser
  de närmaste två sekunderna i förväg på en egen tråd, och ljudet spelar
  vidare medan avbilden läses (en kall läsning ur en CHD kan ta tiondels
  sekunder); `mdec.c` avkodar
  bildrutorna (VLC, IDCT, YCbCr→RGB) och spelets egen filmspelare
  (`../src/movie.c`) lägger dem i VRAM kolumn för kolumn.
- Spara läge (`src/psx/savestate.c`): överst i spelets huvudloop finns hela
  spelets tillstånd i minnet (inget på stacken), så där tas läget:
  PS1-minnet, spelets egna variabler och biblioteksersättningarnas
  tillstånd (VRAM, ljud-RAM och röster, XA-kön, CD:n och strömningen, MDEC,
  räknare, handkontroll, debugtext), varje del bakom en märkning, packat
  med miniz. Att ladda startar om spelet och lägger tillbaka läget vid
  första varvet. Pausmenyn låter spelet gå en bildruta till (tyst, under
  pausskärmen) fram till den punkten. Läget innehåller adresser till
  spelets kod (anrop tillbaka), därför en kontrollsumma av `game.dll` i
  filhuvudet, och storleken på varje del: ett läge från ett bygge där
  någon del ändrats avvisas med en förklaring i listan.

## Tester
Inga av testerna använder det globala tangentbordet, så datorn går att
använda medan de körs. Skivorna tas från `build/settings.ini` (välj dem i
launchern en gång).

- `python tests/play.py`: skriptade körningar (intro, slottet, kompakten,
  skivbytet) med bildmontage i `build/test/play/`; `python tests/play.py
  puzzles` startar alla pussel och systemskärmar (på rätt skiva).
- `python tests/compare_duckstation.py [skiva1.cue]`: samma skärmar i
  DuckStation och i porten, bilder och differenser i `build/test/compare/`
  (matsalen: medel 0,9 av 255).
- `python tests/compare_sound.py [skiva1.cue]`: ljudet i DuckStation och i
  porten genom samma förlopp: titelmusiken (XA) och matsalens bakgrundsljud
  (SPU) jämförs i nivå, spektrum och vågform. DuckStations ljudinspelning
  avkodas med `tests/mf_to_wav.c` (Windows Media Foundation).
- `python tests/savestate.py`: sparar ett läge (matsalen, en film,
  slottet på skiva 2) och spelar vidare, laddar det i en ny körning med
  samma knappar efteråt och jämför bilderna (`build/test/savestate/`;
  matsalen och skiva 2: identiska). I ett pussel ska sparandet vägras.
- `tests/test_mdec.c`: avkodar en STR-bildruta fristående med `mdec.c`.

## Felsökning
Miljövariabler: `D_AUTOSTART=1` (startar spelet direkt),
`D_INPUT=<bildruta>[-<sista>]:<knapp>,…` (trycker knappar),
`D_VRAM_DUMP=<bildruta>,…` (`vram_<n>.bmp` och `shot_<n>.bmp`),
`D_WAV_DUMP=<fil>` (spelar in det som hörs), `D_MUTE=1`,
`D_LEAVE=<bildruta>`, `D_PAUSE=<bildruta>:<ms>` (pausar som P),
`D_SAVE_STATE=<bildruta>:<plats>` (sparar ett läge utan pausskärm),
`D_LOAD_STATE=<plats>` (med `D_AUTOSTART`: startar från ett läge),
`D_SAVE_DIR=<mapp>` (lägen någon annanstans än `saves/`), `D_EVENT=<vblank>:<händelse>[@<plats>]` (startar ett
pussel eller en systemskärm, t.ex. `75` = skivbyte) och `D_NATIVE=1`
(originalupplösning, inga förbättringar), `D_SOUND_LOG=1` (loggar varje
`SsVoKeyOn` med programmets toner). Loggen
skrivs till `d.log` bredvid exe-filen; funktioner som inte är skrivna loggar
sitt första anrop där.

Se `PLAN.md` för hela planen och `THIRD_PARTY.md` för licenser.
