#!/bin/sh
# Every game mode played by bots, one whole match each, from the first spawn
# to the winner's summary (SP_FAST: the game's clock counts ticks and the loop
# never sleeps; SP_EXIT_AT_MATCH_END stops the server when the match is won).
# A run fails on a crash, a non-zero exit, an AddressSanitizer or
# UndefinedBehaviorSanitizer report, a match where nobody died (the bots must
# really have played) or a match that did not end within the game seconds
# given. A match takes 5 to 45 game minutes (1 to 66 real seconds in a release
# build, several times that under the sanitizers).
#
# usage: tools/smoke_modes.sh [server binary] [game seconds]
#   default: server/build-asan/<binary> (make server-asan), 4000 game seconds
#   (classic capture the flag gets half as long again)
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
name=$(sed -n 's/^set(GAME_SERVER_NAME "\(.*\)")/\1/p' "$root/common/game.cmake")
bin=${1:-$root/server/build-asan/$name}
# absolute: the runs start from server/
case "$bin" in /*) ;; *) bin="$(pwd)/$bin" ;; esac
seconds=${2:-4000}
out=$(mktemp -d "${TMPDIR:-/tmp}/smoke.XXXXXX")
port=34900
failed=0

# mode:map, one of each game mode but creative (building only: no match to win). No map is kept in the repository: the server
# downloads each one from ngspades/maps into a temporary folder (--maps-dir), so
# this needs the network (SP_MAP_DOWNLOAD=0 to use maps already in --maps-dir)
for match in ctf:Busan_Port ngctf:Busan_Port zombie:Normandie koth:Normandie tc:Normandie \
             arena:Titanic_Arena babel:babelstairs tdm:Normandie; do
    mode=${match%%:*}
    map=${match#*:}
    # classic capture the flag on Busan_Port (389 blocks between the bases) takes 4100
    # to 5400 game seconds with the bots that flank in a half moon, raise forts and
    # watch from towers (about 3500 before): it gets half as long again. The smoke
    # test only asks that a match ends
    limit=$seconds
    [ "$mode" = ctf ] && limit=$((seconds * 3 / 2))
    log="$out/$mode.log"
    mkdir -p "$out/data-$mode" "$out/maps"
    set +e
    # no smoke server may reach the public list or the network
    ( cd "$root/server" && SP_MASTER=0 SP_MAP_DOWNLOAD=${SP_MAP_DOWNLOAD:-1} SP_EVENTLOG=1 SP_FAST=1 SP_BOTS_PLAY=1 \
        SP_EXIT_AT_MATCH_END=1 SP_EXIT_AFTER="$limit" SP_RAND_SEED=3 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
        UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        "$bin" --port "$port" --data "$out/data-$mode" --maps-dir "$out/maps" --gamemode "$mode" --map "$map" --bots 16 ) > "$log" 2>&1
    code=$?
    set -e
    sanitizer=$(grep -ac 'AddressSanitizer\|runtime error:' "$log" || true)
    deaths=$(grep -ac 'EVT.* death ' "$log" || true)
    ended=$(grep -ac 'Match over: exiting' "$log" || true)
    summary=$(grep -a -o 'action=summary winner=[a-z0-9]* score=[0-9-]* seconds=[0-9]*' "$log" | head -1 | sed 's/action=summary //')
    status=ok
    if [ "$code" -ne 0 ] || [ "$sanitizer" -ne 0 ] || [ "$deaths" -eq 0 ]; then
        status=FAILED
        failed=1
    elif [ "$ended" -eq 0 ]; then
        # SMOKE_NO_END_OK: modes whose match may outlast the game seconds given
        # (babel on Linux: the bots' tower builders block each other's cells,
        # a known livelock). The run still has to be clean and to have played
        if [ "$mode" = babel ] && [ "${SMOKE_BABEL_NO_END_OK:-0}" = 1 ]; then
            status="ok (no winner: known babel livelock)"
        else
            status=FAILED
            failed=1
        fi
    fi
    printf '%-6s %-14s exit=%s sanitizer=%s deaths=%s ended=%s %s %s\n' "$mode" "$map" "$code" "$sanitizer" "$deaths" "$ended" "${summary:-no summary}" "$status"
    if [ "$status" = FAILED ]; then
        grep -a -m 20 -A 12 'AddressSanitizer\|runtime error:\|signal' "$log" || tail -n 30 "$log"
    fi
    port=$((port + 1))
done
echo "logs: $out"
exit "$failed"
