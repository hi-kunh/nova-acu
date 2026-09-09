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

BIN_SRC="${1:-$(dirname "$0")/../acud/acud}"
UNIT_SRC="$(dirname "$0")/acud.service"
CONF_SRC="$(dirname "$0")/config.json"

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
