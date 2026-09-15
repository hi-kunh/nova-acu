#!/bin/sh
# 시험 한 번에 돌리기 — 개발 PC·보드 겸용
#
#   sh tools/run_tests.sh                     # 개발 PC: acud 빌드 + C 시험 + 프로토콜 시험
#   sh tools/run_tests.sh --acud /usr/local/sbin/acud   # 보드: 설치된 acud로 프로토콜 시험만
#   sh tools/run_tests.sh --only setting_write          # 이름에 그 글자가 든 시험만
#   sh tools/run_tests.sh --keep                        # 끝나도 임시 폴더를 남긴다 (로그 보기)
#
# 프로토콜 시험은 **시험마다 acud를 새로 띄운다** (빈 DB · 자기 포트 · 자기 FIFO).
# 앞 시험이 남긴 사용자·이벤트·설정이 뒤 시험을 오염시키는 일이 9/15에 있었다.
#
# acud는 이 스크립트가 띄운 PID로만 끈다.
# ⚠ `pkill -f "acud -c ..."` 금지 — 명령줄에 그 글자가 든 **자기 셸까지** 죽는다 (9/15 사고)

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")

ACUD=""
ONLY=""
KEEP=0
BASE_PORT=19950

while [ $# -gt 0 ]; do
    case "$1" in
        --acud) ACUD="$2"; shift 2 ;;
        --only) ONLY="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --port) BASE_PORT="$2"; shift 2 ;;
        -h|--help) sed -n 2,13p "$0"; exit 0 ;;
        *) echo "모르는 옵션: $1" >&2; exit 2 ;;
    esac
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/acu-tests.XXXXXX")
RESULTS="$WORK/results"
: > "$RESULTS"
ACUD_PID=""

cleanup() {
    stop_acud
    if [ "$KEEP" -eq 1 ]; then
        echo "임시 폴더를 남김: $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

selected() {
    [ -z "$ONLY" ] && return 0
    case "$1" in *"$ONLY"*) return 0 ;; esac
    return 1
}

record() { # 이름 결과(PASS/FAIL/SKIP) 초
    printf '%s %s %s\n' "$1" "$2" "$3" >> "$RESULTS"
    printf '  -> %s (%ss)\n' "$2" "$3"
}

# ---------------------------------------------------------------- acud 띄우기·끄기

# start_acud <폴더> <포트>
start_acud() {
    dir="$1"; port="$2"
    mkdir -p "$dir"
    cat > "$dir/config.json" <<EOF
{
    "db_path": "$dir/acud.db",
    "door_open_seconds": 3,
    "admin_password": "0000",
    "pid_path": "$dir/acud.pid",
    "log_path": "$dir/acud.log",
    "netcfg_request_path": "$dir/netcfg-request",
    "tcp_port": $port,
    "time_sync_enabled": false
}
EOF
    ACU_MOCK_FIFO="$dir/mock.fifo" setsid "$ACUD" -c "$dir/config.json" \
        > "$dir/stdout.log" 2>&1 < /dev/null &
    ACUD_PID=$!

    # 포트가 열리고 FIFO가 생길 때까지 (최대 5초)
    i=0
    while [ $i -lt 50 ]; do
        if ! kill -0 "$ACUD_PID" 2>/dev/null; then
            echo "  acud가 바로 죽었다 — $dir/stdout.log:"
            tail -5 "$dir/stdout.log" "$dir/acud.log" 2>/dev/null
            ACUD_PID=""
            return 1
        fi
        if [ -p "$dir/mock.fifo" ] && python3 - "$port" <<'PY' 2>/dev/null
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.2)
s.close()
PY
        then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    echo "  acud가 5초 안에 포트 $port 를 열지 않았다"
    stop_acud
    return 1
}

stop_acud() {
    [ -z "$ACUD_PID" ] && return 0
    kill "$ACUD_PID" 2>/dev/null
    i=0
    while kill -0 "$ACUD_PID" 2>/dev/null && [ $i -lt 30 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    kill -9 "$ACUD_PID" 2>/dev/null
    wait "$ACUD_PID" 2>/dev/null
    ACUD_PID=""
}

# ---------------------------------------------------------------- 시험 하나 돌리기

# run_protocol <이름> <명령...>   — 명령 안의 @DIR@ @PORT@ 를 바꿔 넣는다
N=0
run_protocol() {
    name="$1"; shift
    selected "$name" || return 0
    N=$((N + 1))
    dir="$WORK/$name"
    port=$((BASE_PORT + N))
    echo "== $name (포트 $port)"
    t0=$(date +%s)
    if ! start_acud "$dir" "$port"; then
        record "$name" FAIL $(( $(date +%s) - t0 ))
        return 0
    fi

    set --  "$@"
    cmd=""
    for a in "$@"; do
        a=$(printf '%s' "$a" | sed "s|@DIR@|$dir|g; s|@PORT@|$port|g")
        cmd="$cmd '$a'"
    done
    if eval "python3 $cmd" > "$dir/test.out" 2>&1; then
        result=PASS
    else
        result=FAIL
        echo "  --- 시험 출력 (끝 20줄) ---"
        tail -20 "$dir/test.out" | sed 's/^/  | /'
        echo "  --- acud.log (끝 10줄) ---"
        tail -10 "$dir/acud.log" 2>/dev/null | sed 's/^/  | /'
    fi
    stop_acud
    record "$name" "$result" $(( $(date +%s) - t0 ))
}

# run_c <이름> <make 대상> <실행 파일> [인자...]
run_c() {
    name="$1"; target="$2"; bin="$3"; shift 3
    selected "$name" || return 0
    echo "== $name"
    t0=$(date +%s)
    if ! make -s -C "$ROOT/acud" "$target" > "$WORK/$name.build" 2>&1; then
        tail -20 "$WORK/$name.build" | sed 's/^/  | /'
        record "$name" FAIL $(( $(date +%s) - t0 ))
        return 0
    fi
    mkdir -p "$WORK/$name"
    if (cd "$WORK/$name" && "$ROOT/acud/$bin" "$@") > "$WORK/$name.out" 2>&1; then
        record "$name" PASS $(( $(date +%s) - t0 ))
    else
        tail -20 "$WORK/$name.out" | sed 's/^/  | /'
        record "$name" FAIL $(( $(date +%s) - t0 ))
    fi
}

# ---------------------------------------------------------------- 준비

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3가 없다" >&2
    exit 2
fi

BUILD_C=0
if [ -z "$ACUD" ]; then
    echo "== acud 빌드"
    if ! make -s -C "$ROOT/acud" acud; then
        echo "acud 빌드 실패" >&2
        exit 1
    fi
    ACUD="$ROOT/acud/acud"
    BUILD_C=1
fi
echo "acud: $ACUD"
echo "임시 폴더: $WORK"
echo

# ---------------------------------------------------------------- C 시험 (개발 PC만 — 보드에는 컴파일러가 없다)

if [ "$BUILD_C" -eq 1 ] && command -v gcc >/dev/null 2>&1; then
    run_c users_test       userstest users_test
    run_c events_wrap_test wraptest  events_wrap_test
    run_c rru_proto_check  rrucheck  rru_proto_check
fi

# ---------------------------------------------------------------- 프로토콜 시험

T="$HERE"
run_protocol userbin_test       "$T/userbin_test.py"       --port @PORT@ --users-db @DIR@/users.db
run_protocol usercmd_test       "$T/usercmd_test.py"       --port @PORT@ --users-db @DIR@/users.db
run_protocol setting_read_test  "$T/setting_read_test.py"  --port @PORT@ --db @DIR@/acud.db
run_protocol setting_write_test "$T/setting_write_test.py" --port @PORT@ --fifo @DIR@/mock.fifo --log @DIR@/acud.log
run_protocol setting_all_test   "$T/setting_all_test.py"   --port @PORT@
run_protocol event_index_test   "$T/event_index_test.py"   --port @PORT@ --fifo @DIR@/mock.fifo --events-db @DIR@/events.db

# ---------------------------------------------------------------- 요약

echo
echo "================ 요약 ================"
pass=0; fail=0
while read -r name result secs; do
    printf '  %-20s %s  %ss\n' "$name" "$result" "$secs"
    case "$result" in
        PASS) pass=$((pass + 1)) ;;
        FAIL) fail=$((fail + 1)) ;;
    esac
done < "$RESULTS"
echo "  ------------------------------------"
echo "  통과 $pass / 실패 $fail"

if [ $((pass + fail)) -eq 0 ]; then
    echo "  돌린 시험이 없다 (--only 확인)"
    exit 2
fi
[ "$fail" -eq 0 ]
