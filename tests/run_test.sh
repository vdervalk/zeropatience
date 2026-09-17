#!/usr/bin/env bash
# Draait de probe tegen het synthetische testdoel en controleert of de
# afgeleide offsets overeenkomen met wat fake_game daadwerkelijk heeft gebouwd.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
WORK="${TMPDIR:-/tmp}/zp-test.$$"
mkdir -p "$WORK"

export WINEPREFIX="${WINEPREFIX:-$WORK/prefix}"
export WINEDEBUG="${WINEDEBUG:--all}"

# Op sommige distributies staat alleen /usr/lib/wine/wine64 geinstalleerd,
# zonder de wrapper in /usr/bin.
WINE="${WINE:-}"
if [[ -z "$WINE" ]]; then
    for c in wine wine64 /usr/lib/wine/wine64 /usr/lib/wine/wine; do
        if command -v "$c" >/dev/null 2>&1; then WINE="$c"; break; fi
        if [[ -x "$c" ]]; then WINE="$c"; break; fi
    done
fi
if [[ -z "$WINE" ]]; then
    echo "OVERGESLAGEN: geen wine gevonden, kan het testdoel niet draaien"
    exit 77
fi
WINESERVER="${WINESERVER:-$(command -v wineserver || echo /usr/lib/wine/wineserver)}"
echo "wine: $WINE"

cleanup() {
    [[ -n "${GAME_PID:-}" ]] && kill "$GAME_PID" 2>/dev/null
    "$WINESERVER" -k 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "== zelftest (32-bit logica, synthetische adresruimte) =="
if ! "$WINE" "$BUILD/zp-probe64.exe" --selftest 2>/dev/null; then
    echo "FOUT: de zelftest is mislukt"
    exit 1
fi
echo

echo "== testdoel starten =="
"$WINE" "$BUILD/fake_game64.exe" > "$WORK/game.out" 2>"$WORK/game.err" &
GAME_PID=$!

for _ in $(seq 1 60); do
    grep -q '^PID=' "$WORK/game.out" 2>/dev/null && break
    sleep 1
done

if ! grep -q '^PID=' "$WORK/game.out"; then
    echo "FOUT: testdoel is niet gestart"
    sed -n '1,20p' "$WORK/game.err"
    exit 1
fi

TARGET_PID="$(sed -n 's/^PID=//p' "$WORK/game.out")"
echo "testdoel draait als pid $TARGET_PID"
echo
cat "$WORK/game.out"
echo

echo "== probe draaien =="
"$WINE" "$BUILD/zp-probe64.exe" --pid "$TARGET_PID" \
     --out "$WORK/report.txt" --max-snapshot 512 \
     > "$WORK/probe.out" 2>"$WORK/probe.err"
RC=$?
if [[ ! -s "$WORK/report.txt" ]]; then
    echo "FOUT: probe leverde geen rapport (exit $RC)"
    sed -n '1,40p' "$WORK/probe.out"
    sed -n '1,20p' "$WORK/probe.err"
    exit 1
fi

REPORT="$WORK/report.txt"
cp "$REPORT" "$BUILD/test-report.txt"
echo "rapport: $BUILD/test-report.txt"
echo

# --- controles ---------------------------------------------------------
# De resolutie-sectie is de eigenlijke uitkomst van de probe. Daar wordt
# exact op gecontroleerd; elders in het rapport staan ook kandidaten, en een
# losse grep zou daar ten onrechte op aanslaan.
RESOLUTION="$WORK/resolution.txt"
sed -n '/^  RESOLUTIE$/,/^  GEZONDHEIDSVELDEN/p' "$REPORT" > "$RESOLUTION"

# De dubbelgelinkte objectlijst in het testdoel is de valkuil die de echte
# meting liet mislukken: toen kwamen ActiveBody en Object op dezelfde vtable
# uit. Die twee moeten nu verschillen.
AB="$(sed -n 's/^ActiveBody-vtable : \(0x[0-9a-f]*\).*/\1/p' "$RESOLUTION")"
OB="$(sed -n 's/^Object-vtable     : \(0x[0-9a-f]*\).*/\1/p' "$RESOLUTION")"
if [[ -n "$AB" && -n "$OB" && "$AB" != "$OB" ]]; then
    echo "VERSCHILLEND $AB $OB" > "$WORK/distinct.txt"
else
    echo "GELIJK $AB $OB" > "$WORK/distinct.txt"
fi

FAILED=0
check() {  # check <omschrijving> <bestand> <grep-patroon>
    if grep -qE "$3" "$2"; then
        echo "  OK    $1"
    else
        echo "  FOUT  $1"
        echo "        verwacht patroon: $3"
        FAILED=1
    fi
}

echo "== controles =="
check "ThePlayerList gevonden"              "$REPORT"     'm_local @ 0x'
check "vier spelers geteld"                 "$REPORT"     'spelers=4'
check "lokale speler is index 1"            "$REPORT"     'lokale index=1'
check "body-module vastgesteld"             "$RESOLUTION" 'ActiveBody-vtable : 0x'
check "Object is niet dezelfde vtable"      "$WORK/distinct.txt" 'VERSCHILLEND'
check "this -> Object is -0x70"             "$RESOLUTION" 'this -> Object +\-0x70$'
check "this -> health is +0x30"             "$RESOLUTION" 'this -> m_currentHealth +\+0x30$'
check "Object::m_body is +0x40"             "$RESOLUTION" 'Object::m_body +\+0x40$'
check "vormcontrole: Object negatief"       "$RESOLUTION" 'Object is negatief +ja'
check "vormcontrole: hitpoints positief"    "$RESOLUTION" 'hitpoints is positief +ja'
check "poolnaam ActiveBody gevonden"        "$REPORT"     'string op: 0x'

# De poolnaam moet dezelfde vtable aanwijzen als de resolutie koos. Dat is de
# enige controle die niet op statistiek steunt.
sed -n '/-- "ActiveBody" --/,/-- "ObjectPool" --/p' "$REPORT" > "$WORK/anchor.txt"
if [[ -n "$AB" ]] && grep -q "$AB" "$WORK/anchor.txt"; then
    echo "NAAM_KLOPT" > "$WORK/named.txt"
else
    echo "NAAM_WIJKT_AF $AB" > "$WORK/named.txt"
fi
check "poolnaam wijst dezelfde vtable aan"  "$WORK/named.txt" 'NAAM_KLOPT'
check "eigenaarsketen m_team +0x150"        "$REPORT"     '^\+0x150 '
check "eigenaarsketen volledig +0x150/+0x20/+0x48" "$REPORT" '^\+0x150 +\+0x20 +\+0x48 '

echo
if [[ $FAILED -eq 0 ]]; then
    echo "ALLE CONTROLES GESLAAGD"
else
    echo "ER ZIJN CONTROLES MISLUKT -- zie $BUILD/test-report.txt"
fi
exit $FAILED
