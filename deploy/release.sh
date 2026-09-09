#!/bin/sh
#
# 배포용 릴리스 번들을 만든다. **번들에는 소스가 들어가지 않는다.**
#
#   sh deploy/release.sh
#
# 제품 ACU에는 소스도 툴체인도 남으면 안 된다. 그래서 배포 단위를
# "미리 빌드된 바이너리 + 설치에 필요한 파일"로 좁힌다.
#
# 만들어지는 것:
#   dist/acud-<버전>-<아키텍처>.tar.gz
#
# 설치는 번들을 푼 뒤 그 안의 install.sh를 root로 실행하면 된다.
# install.sh는 바이너리 하나만 있으면 동작하도록 처음부터 그렇게 만들어 뒀다.
#
# **주의: 이 스크립트는 타깃과 같은 환경에서 돌려야 한다.**
# 지금은 보드(Debian 11 bullseye / arm64)에서 실행한다. 개발 PC(Ubuntu 26.04, glibc 2.43)에서
# 그냥 크로스 컴파일하면 glibc 2.31인 보드에서 실행되지 않는다.
# 재현 가능한 빌드는 bullseye arm64 컨테이너로 옮기는 것이 다음 단계다 (TODO 5.5-5).
#
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACUD_DIR="$ROOT/acud"
DEPLOY_DIR="$ROOT/deploy"
DIST_DIR="$ROOT/dist"

# 번들에 담을 파일들. 소스(.c/.h)는 의도적으로 빠져 있다
DEPLOY_FILES="
install.sh
acud.service
acu-netcfg
acu-netcfg-boot.service
acu-netcfg-apply.path
acu-netcfg-apply.service
sudoers-acu-netcfg
config.json
"

say() { echo "  $*"; }

# 빌드 환경이 타깃과 맞는지 알려 준다 (막지는 않는다 - 판단은 사람이 한다)
check_build_host() {
    arch=$(uname -m)
    os="unknown"
    [ -r /etc/os-release ] && os=$(. /etc/os-release && echo "$PRETTY_NAME")
    libc=$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')

    say "빌드 호스트: $os / $arch / glibc $libc"
    if [ "$arch" != "aarch64" ]; then
        echo "경고: 타깃은 arm64인데 여기는 $arch 다. 이 번들은 보드에서 실행되지 않는다." >&2
    fi
}

# 버전 문자열을 만든다.
# 보드에는 .git을 rsync하지 않으므로 git이 안 잡힌다. 그때는 개발 PC에서
# ACU_VERSION=$(git rev-parse --short HEAD) 로 넘겨 주면 커밋을 실을 수 있고,
# 그것도 없으면 날짜로 대신한다.
make_version() {
    if [ -n "${ACU_VERSION:-}" ]; then
        echo "$ACU_VERSION"
        return
    fi
    if git -C "$ROOT" rev-parse --short HEAD >/dev/null 2>&1; then
        v=$(git -C "$ROOT" rev-parse --short HEAD)
        git -C "$ROOT" diff --quiet 2>/dev/null || v="$v-dirty"
        echo "$v"
    else
        date +%Y%m%d
    fi
}

# 번들에 소스가 섞여 들어가지 않았는지 확인한다. 이 검사가 이 스크립트의 존재 이유다
assert_no_source() {
    dir="$1"
    found=$(find "$dir" -name '*.c' -o -name '*.h' -o -name '*.py' | head -5)
    if [ -n "$found" ]; then
        echo "번들에 소스가 섞였다:" >&2
        echo "$found" >&2
        exit 1
    fi
}

check_build_host

VERSION=$(make_version)
ARCH=$(uname -m)
NAME="acud-$VERSION-$ARCH"
STAGE="$DIST_DIR/$NAME"

say "버전: $VERSION"

# 1) 빌드
say "빌드 중..."
make -C "$ACUD_DIR" >/dev/null
[ -f "$ACUD_DIR/acud" ] || { echo "빌드 산출물이 없다" >&2; exit 1; }

# 2) 스테이징
rm -rf "$STAGE"
mkdir -p "$STAGE"

cp "$ACUD_DIR/acud" "$STAGE/acud"
before=$(wc -c < "$STAGE/acud")
strip "$STAGE/acud" 2>/dev/null || say "strip을 쓸 수 없다 - 심볼이 남는다"
after=$(wc -c < "$STAGE/acud")
say "바이너리: $before -> $after byte (strip)"
chmod 0755 "$STAGE/acud"

for f in $DEPLOY_FILES; do
    [ -f "$DEPLOY_DIR/$f" ] || { echo "빠진 파일: deploy/$f" >&2; exit 1; }
    cp "$DEPLOY_DIR/$f" "$STAGE/$f"
done
chmod 0755 "$STAGE/install.sh" "$STAGE/acu-netcfg"

# 3) 빌드 정보 (나중에 "이 보드에 뭐가 깔려 있지?"에 답하기 위한 것)
{
    echo "version=$VERSION"
    echo "arch=$ARCH"
    echo "built_at=$(date -Is)"
    echo "built_on=$(uname -sr)"
    [ -r /etc/os-release ] && echo "built_os=$(. /etc/os-release && echo "$PRETTY_NAME")"
    echo "glibc=$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')"
} > "$STAGE/VERSION"

assert_no_source "$STAGE"

# 4) 묶기
TARBALL="$DIST_DIR/$NAME.tar.gz"
rm -f "$TARBALL"
tar -czf "$TARBALL" -C "$DIST_DIR" "$NAME"
rm -rf "$STAGE"

say "완성: $TARBALL ($(wc -c < "$TARBALL") byte)"
echo
echo "설치 방법 (보드에서):"
echo "  tar xzf $NAME.tar.gz"
echo "  cd $NAME && sudo sh install.sh"
echo "  sudo systemctl enable --now acud"
