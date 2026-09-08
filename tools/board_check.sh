#!/bin/sh
# CM3 보드 환경 점검 (읽기 전용 - 아무것도 바꾸지 않는다)
#
# 보드에 복사해서 실행하고 출력을 그대로 공유하면 된다.
#   scp tools/board_check.sh rock@<보드IP>:~/
#   ssh rock@<보드IP> 'sh board_check.sh'

echo "===== 1. OS / 커널 ====="
uname -a
echo "debian_version : $(cat /etc/debian_version 2>/dev/null)"
echo "arch           : $(dpkg --print-architecture 2>/dev/null)"
grep -E '^(PRETTY_NAME|VERSION)=' /etc/os-release 2>/dev/null

echo
echo "===== 2. 시각 (apt 실패의 흔한 원인) ====="
date
echo "UTC : $(date -u)"
if command -v timedatectl >/dev/null 2>&1; then timedatectl status 2>/dev/null | head -8; fi
ls /dev/rtc* 2>/dev/null || echo "RTC 장치 노드 없음"
echo "-- 시각이 실제와 크게 다르면 apt가 'Release file is not valid yet' 로 실패한다"

echo
echo "===== 3. 네트워크 ====="
ip -brief addr 2>/dev/null || ifconfig 2>/dev/null
echo "-- 기본 경로 --"
ip route 2>/dev/null | head -5
echo "-- DNS --"
cat /etc/resolv.conf 2>/dev/null | grep -v '^#' | head -5
echo "-- 도달성 (각 3초) --"
ping -c1 -W3 8.8.8.8            >/dev/null 2>&1 && echo "  ping 8.8.8.8        : OK" || echo "  ping 8.8.8.8        : 실패"
ping -c1 -W3 deb.debian.org     >/dev/null 2>&1 && echo "  ping deb.debian.org : OK (DNS 동작)" || echo "  ping deb.debian.org : 실패 (DNS 또는 외부망)"

echo
echo "===== 4. apt 저장소 설정 ====="
echo "-- /etc/apt/sources.list --"
grep -v -e '^#' -e '^$' /etc/apt/sources.list 2>/dev/null
echo "-- /etc/apt/sources.list.d/ --"
for f in /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
    [ -f "$f" ] || continue
    echo "[$f]"
    grep -v -e '^#' -e '^$' "$f"
done

echo
echo "===== 5. cdc_acm (6단계에서 RRU 연결에 필요) ====="
KREL=$(uname -r)
if [ -x /usr/sbin/modinfo ]; then
    /usr/sbin/modinfo cdc_acm 2>/dev/null | head -3 || echo "  modinfo: cdc_acm 정보 없음"
else
    echo "  /usr/sbin/modinfo 없음"
fi
echo "-- 모듈 파일 --"
find /lib/modules/"$KREL" -name 'cdc-acm.ko*' 2>/dev/null | head -3 || true
[ -z "$(find /lib/modules/"$KREL" -name 'cdc-acm.ko*' 2>/dev/null)" ] && echo "  모듈 파일 없음 (커널 내장일 수 있음)"
echo "-- 커널 설정 --"
if [ -f /boot/config-"$KREL" ]; then
    grep -E 'CONFIG_USB_ACM' /boot/config-"$KREL" || echo "  CONFIG_USB_ACM 항목 없음"
else
    echo "  /boot/config-$KREL 없음"
fi
echo "-- 현재 USB 장치 --"
lsusb 2>/dev/null | head -10 || echo "  lsusb 없음"

echo
echo "===== 6. 빌드 의존성 (이미 있으면 apt 없이도 진행 가능) ====="
for c in gcc make git sqlite3 python3 pip3; do
    if command -v $c >/dev/null 2>&1; then
        echo "  $c : $(command -v $c)"
    else
        echo "  $c : 없음"
    fi
done
[ -f /usr/include/sqlite3.h ] && echo "  sqlite3.h  : 있음" || echo "  sqlite3.h  : 없음 (libsqlite3-dev 필요)"
if [ -f /usr/include/cjson/cJSON.h ]; then echo "  cJSON.h    : 있음"
elif [ -f /usr/include/cJSON.h ]; then echo "  cJSON.h    : 있음 (/usr/include)"
else echo "  cJSON.h    : 없음 (libcjson-dev 필요)"; fi
ls /usr/lib/*/libsqlite3.so* /usr/lib/libsqlite3.so* 2>/dev/null | head -2
ls /usr/lib/*/libcjson.so* /usr/lib/libcjson.so* 2>/dev/null | head -2
python3 -c "import venv" 2>/dev/null && echo "  python3 venv : 있음" || echo "  python3 venv : 없음 (python3-venv 필요)"
python3 -V 2>/dev/null

echo
echo "===== 7. 포트 1004 사용 여부 ====="
(ss -ltnp 2>/dev/null || netstat -ltnp 2>/dev/null) | grep -E ':1004|:9870' || echo "  1004 / 9870 둘 다 비어 있음"

echo
echo "===== 점검 끝 ====="
