#!/bin/sh
#
# 개발 PC에서 릴리스 번들을 만든다. 빌드는 **bullseye arm64 컨테이너 안에서** 돈다.
#
#   sh deploy/build-container.sh
#
# 보드에 접속하지 않고도 배포용 번들을 만들 수 있게 하는 것이 목적이다.
# 컨테이너가 타깃과 같은 배포판/아키텍처라 glibc 불일치 문제가 생기지 않는다
# (자세한 이유는 deploy/Containerfile.build 주석 참고).
#
# 필요한 것: podman, qemu-user, qemu-user-binfmt
#   sudo apt install podman qemu-user qemu-user-binfmt
#
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="acud-build:bullseye-arm64"
PLATFORM="linux/arm64"

die() { echo "$1" >&2; exit 1; }

command -v podman >/dev/null 2>&1 || \
    die "podman이 없다: sudo apt install podman qemu-user qemu-user-binfmt"

# arm64 바이너리를 이 PC에서 실행하려면 커널에 binfmt 핸들러가 등록돼 있어야 한다
if [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
    die "arm64 binfmt 핸들러가 없다: sudo apt install qemu-user qemu-user-binfmt
    (설치 후에도 없으면 sudo systemctl restart systemd-binfmt)"
fi

# 이미지가 없을 때만 만든다. 두 번째부터는 이 단계를 건너뛴다
if ! podman image exists "$IMAGE" 2>/dev/null; then
    echo "== 빌드 이미지를 만든다 (처음 한 번만, 에뮬레이션이라 몇 분 걸린다) =="
    podman build --platform "$PLATFORM" -t "$IMAGE" -f "$ROOT/deploy/Containerfile.build" "$ROOT"
fi

# 번들 버전에는 개발 PC의 커밋을 싣는다
VERSION="${ACU_VERSION:-}"
if [ -z "$VERSION" ] && git -C "$ROOT" rev-parse --short HEAD >/dev/null 2>&1; then
    VERSION=$(git -C "$ROOT" rev-parse --short HEAD)
    git -C "$ROOT" diff --quiet 2>/dev/null || VERSION="$VERSION-dirty"
fi

echo "== 컨테이너 안에서 릴리스 번들 생성 =="
podman run --rm --platform "$PLATFORM" \
    -v "$ROOT:/src" \
    -e "ACU_VERSION=$VERSION" \
    "$IMAGE" \
    sh -c 'cd /src && make -C acud clean >/dev/null 2>&1; sh deploy/release.sh'

echo
echo "== 결과 =="
ls -la "$ROOT/dist/"
