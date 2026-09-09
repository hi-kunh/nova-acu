#!/bin/sh
#
# acud를 systemd 서비스로 설치한다. 보드에서 root로 실행한다.
#
#   sudo sh deploy/install.sh [acud_바이너리_경로]
#
# 바이너리 경로를 주지 않으면 acud/acud를 쓴다.
# 소스는 필요하지 않다 - 미리 빌드된 바이너리 하나만 있으면 된다.
# (5.5-5의 "바이너리 전용 배포"로 넘어가도 이 스크립트를 그대로 쓸 수 있게 한 것)
#
set -eu

HERE="$(dirname "$0")"

#
# acud 바이너리를 찾는다. 두 가지 배치를 모두 지원해야 한다:
#   1) 릴리스 번들  - 바이너리가 install.sh 옆에 있다 (제품 배포 경로. 소스가 없다)
#   2) 저장소       - 바이너리가 ../acud/acud 에 있다 (개발 중 경로)
# 인자로 직접 주면 그것이 우선한다.
#
find_binary() {
    if [ $# -ge 1 ] && [ -n "$1" ]; then
        echo "$1"
    elif [ -f "$HERE/acud" ]; then
        echo "$HERE/acud"
    else
        echo "$HERE/../acud/acud"
    fi
}

BIN_SRC="$(find_binary "${1:-}")"
UNIT_SRC="$HERE/acud.service"
CONF_SRC="$HERE/config.json"

BIN_DST=/usr/local/sbin/acud
CONF_DIR=/etc/acud
CONF_DST="$CONF_DIR/config.json"
UNIT_DST=/etc/systemd/system/acud.service

if [ "$(id -u)" -ne 0 ]; then
    echo "root로 실행해야 한다: sudo sh $0" >&2
    exit 1
fi

if [ ! -f "$BIN_SRC" ]; then
    echo "acud 바이너리를 찾을 수 없다: $BIN_SRC" >&2
    exit 1
fi

# 1) 전용 시스템 계정 (로그인 불가, 홈 없음)
if ! getent passwd acud >/dev/null; then
    useradd --system --no-create-home --shell /usr/sbin/nologin acud
    echo "시스템 계정 acud 생성"
else
    echo "시스템 계정 acud 이미 있음"
fi

# 2) 바이너리
install -m 0755 "$BIN_SRC" "$BIN_DST"
echo "설치: $BIN_DST"

# 3) 설정 - 이미 있으면 덮어쓰지 않는다 (운영 중 값을 날리면 안 된다)
mkdir -p "$CONF_DIR"
if [ -f "$CONF_DST" ]; then
    echo "설정 유지: $CONF_DST (이미 존재)"
else
    install -m 0640 -o root -g acud "$CONF_SRC" "$CONF_DST"
    echo "설치: $CONF_DST"
fi

# 4) 유닛
install -m 0644 "$UNIT_SRC" "$UNIT_DST"
systemctl daemon-reload
echo "설치: $UNIT_DST"

# 4-1) 네트워크 설정 헬퍼 + 부팅 시 롤백 + sudoers
#      화면/키보드가 없는 장치라 IP를 webui로 바꿔야 하는데, 잘못 넣으면 복구할 수단이 없다.
#      헬퍼가 검증/자동 롤백을 책임지고, webui는 root가 되지 않는다.
NETCFG_SRC="$HERE/acu-netcfg"
if [ -f "$NETCFG_SRC" ]; then
    install -m 0755 "$NETCFG_SRC" /usr/local/sbin/acu-netcfg
    install -m 0644 "$HERE/acu-netcfg-boot.service" \
            /etc/systemd/system/acu-netcfg-boot.service
    # UDP 탐색(SETT)이 남긴 요청을 root로 집어 가 적용하는 감시자
    install -m 0644 "$HERE/acu-netcfg-apply.path" \
            /etc/systemd/system/acu-netcfg-apply.path
    install -m 0644 "$HERE/acu-netcfg-apply.service" \
            /etc/systemd/system/acu-netcfg-apply.service
    # sudoers는 문법 오류가 나면 sudo 자체가 막히므로 반드시 검사 후 설치한다
    TMP_SUDO=$(mktemp)
    cp "$HERE/sudoers-acu-netcfg" "$TMP_SUDO"
    if visudo -cf "$TMP_SUDO" >/dev/null 2>&1; then
        install -m 0440 -o root -g root "$TMP_SUDO" /etc/sudoers.d/acu-netcfg
        echo "설치: /usr/local/sbin/acu-netcfg, /etc/sudoers.d/acu-netcfg"
    else
        echo "경고: sudoers 문법 검사 실패 - 설치하지 않았다" >&2
    fi
    rm -f "$TMP_SUDO"
    systemctl daemon-reload
    systemctl enable acu-netcfg-boot.service >/dev/null 2>&1 || true
    systemctl enable --now acu-netcfg-apply.path >/dev/null 2>&1 || true

    if ! command -v arping >/dev/null 2>&1; then
        echo "참고: arping이 없다. IP 충돌 검사를 위해 'apt install iputils-arping' 권장" >&2
    fi
fi

# 5) mock 단계 한정 - 개발 계정이 카드 주입 FIFO에 쓸 수 있게 acud 그룹에 넣는다.
#    6단계에서 실제 HAL로 바뀌면 필요 없어진다. (재로그인해야 그룹이 적용된다)
DEV_USER="${SUDO_USER:-}"
if [ -n "$DEV_USER" ] && [ "$DEV_USER" != root ]; then
    if id -nG "$DEV_USER" | tr ' ' '\n' | grep -qx acud; then
        echo "$DEV_USER 는 이미 acud 그룹"
    else
        usermod -aG acud "$DEV_USER"
        echo "$DEV_USER 를 acud 그룹에 추가 (mock FIFO 주입용. 재로그인 필요)"
    fi
fi

cat <<'MSG'

설치 완료. 다음 단계:

  sudo systemctl enable --now acud     # 부팅 자동 시작 + 지금 시작
  systemctl status acud
  journalctl -u acud -f                # 로그
  sudo systemctl reload acud           # SIGHUP (설정 리로드)

설정은 /etc/acud/config.json 을 고친 뒤 reload 한다.
MSG
