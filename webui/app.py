"""
ACU 웹 설정 인터페이스 (4단계).

acud(C 데몬)가 읽는 config.json을 직접 읽고 쓴다.
저장 후에는 acud.pid에 적힌 PID로 SIGHUP을 보내, acud가 재시작 없이
새 설정을 반영하도록 한다 (3단계에서 만든 무중단 리로드 기능을 그대로 사용).

설정 화면 진입 전에는 config.json의 admin_password(숫자 4자리)로 로그인해야 한다
(IDTi Header의 Password(4byte)/User Info의 Password(2byte BCD) 필드와 같은 맥락의
"단말기 비밀번호" 개념 - 호스트 PC 디바이스 관리 매니저 프로그램의 비밀번호 설정과 대응).

주의: 4자리 숫자 PIN은 그 자체로 (10000가지) 무차별 대입에 약하다.
실제 배포 전에는 반드시 실패 횟수 제한/계정 잠금, HTTPS, 사내망 한정 등을 추가해야 한다.
지금은 로그인 실패 시 1초 지연만 두어 아주 기본적인 완화만 적용했다.
"""

import functools
import ipaddress
import json
import os
import re
import signal
import subprocess
import time

from flask import Flask, flash, redirect, render_template, request, session, url_for

# 개발 중에는 acud/ 한 디렉터리에 config.json과 acud.pid가 같이 있지만,
# systemd로 띄우면 설정은 /etc/acud/, PID는 /run/acud/로 흩어진다.
# 그래서 각각 따로 지정할 수 있게 하고, ACU_ACUD_DIR는 기존 방식의 기본값으로 남긴다.
ACUD_DIR = os.environ.get(
    "ACU_ACUD_DIR", os.path.join(os.path.dirname(__file__), "..", "acud")
)
CONFIG_PATH = os.environ.get("ACU_CONFIG_PATH") or os.path.join(ACUD_DIR, "config.json")
PID_PATH = os.environ.get("ACU_PID_PATH") or os.path.join(ACUD_DIR, "acud.pid")

DEFAULT_CONFIG = {
    "db_path": "acud.db",
    "door_open_seconds": 3,
    "admin_password": "0000",
    "tcp_port": 9870,  # IDTi 프로토콜 V2 TCP 서버 포트 (5단계). 웹 화면 편집은 아직 미지원
}

DOOR_OPEN_SECONDS_MIN = 1
DOOR_OPEN_SECONDS_MAX = 99  # IDTi Device Output(Relay) ActiveTime 범위와 동일

PASSWORD_RE = re.compile(r"^\d{4}$")
FAILED_LOGIN_DELAY_SECONDS = 1  # 무차별 대입을 늦추기 위한 최소한의 지연

# 네트워크 설정은 root 권한이 필요하지만 webui를 root로 돌리지는 않는다.
# 검증과 자동 롤백을 책임지는 헬퍼 하나만 sudo로 부른다 (/etc/sudoers.d/acu-netcfg).
NETCFG_BIN = os.environ.get("ACU_NETCFG_BIN", "/usr/local/sbin/acu-netcfg")

# 적용 후 이 시간 안에 새 주소로 다시 접속해 "확인"을 누르지 않으면 자동으로 되돌아간다.
# 주소가 바뀌면 브라우저 세션(오리진)이 달라져 재로그인이 필요하므로 넉넉히 잡는다.
ROLLBACK_SECONDS = int(os.environ.get("ACU_NETCFG_ROLLBACK_SECONDS", "180"))
WEBUI_PORT = int(os.environ.get("ACU_WEBUI_PORT", "5000"))

app = Flask(__name__)
app.secret_key = os.environ.get("ACU_WEBUI_SECRET", "dev-only-change-me")


def load_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        cfg = {}
    for key, value in DEFAULT_CONFIG.items():
        cfg.setdefault(key, value)
    return cfg


def save_config(cfg):
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=4, ensure_ascii=False)
        f.write("\n")


def reload_daemon():
    """acud.pid의 PID로 SIGHUP을 보내 config.json을 다시 읽게 한다."""
    try:
        with open(PID_PATH, "r", encoding="utf-8") as f:
            pid = int(f.read().strip())
    except (FileNotFoundError, ValueError):
        return False, "PID 파일을 찾을 수 없습니다 (acud가 실행 중인지 확인하세요)"

    try:
        os.kill(pid, signal.SIGHUP)
    except ProcessLookupError:
        return False, f"PID {pid} 프로세스가 없습니다 (acud.pid가 오래된 값일 수 있음)"
    except PermissionError:
        return False, f"PID {pid}에 신호를 보낼 권한이 없습니다"

    return True, f"acud(PID {pid})에 설정 리로드 신호를 보냈습니다"


def netcfg(*args):
    """acu-netcfg 헬퍼를 sudo로 호출한다. (성공여부, 출력) 을 돌려준다."""
    try:
        proc = subprocess.run(
            ["sudo", "-n", NETCFG_BIN, *args],
            capture_output=True,
            text=True,
            timeout=60,
        )
    except FileNotFoundError:
        return False, "sudo 또는 acu-netcfg를 찾을 수 없습니다."
    except subprocess.TimeoutExpired:
        return False, "네트워크 설정 명령이 응답하지 않습니다."

    out = (proc.stdout + proc.stderr).strip()
    return proc.returncode == 0, out


def netcfg_show():
    """현재 네트워크 설정을 dict로 돌려준다. 실패하면 None."""
    ok, out = netcfg("show")
    if not ok:
        return None
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return None


def validate_network_form(ip_raw, prefix_raw, gateway_raw, dns_raw):
    """
    헬퍼도 같은 검증을 하지만, 화면에서 먼저 걸러야 사용자가 알아보기 쉬운 메시지를 받는다.
    (헬퍼 쪽 검증은 webui를 거치지 않고 호출되는 경우를 위한 것이라 지울 수 없다)
    """
    errors = []

    try:
        prefix = int(prefix_raw)
        if not (8 <= prefix <= 30):
            errors.append("넷마스크 비트(prefix)는 8~30 사이여야 합니다.")
            prefix = None
    except ValueError:
        errors.append("넷마스크 비트(prefix)는 숫자여야 합니다.")
        prefix = None

    ip_obj = gw_obj = None
    try:
        ip_obj = ipaddress.IPv4Address(ip_raw)
    except ValueError:
        errors.append(f"IP 주소 형식이 올바르지 않습니다: {ip_raw}")
    try:
        gw_obj = ipaddress.IPv4Address(gateway_raw)
    except ValueError:
        errors.append(f"게이트웨이 형식이 올바르지 않습니다: {gateway_raw}")

    dns_list = [d.strip() for d in dns_raw.split(",") if d.strip()]
    for d in dns_list:
        try:
            ipaddress.IPv4Address(d)
        except ValueError:
            errors.append(f"DNS 형식이 올바르지 않습니다: {d}")

    if ip_obj is not None and gw_obj is not None and prefix is not None:
        if ip_obj == gw_obj:
            errors.append("IP와 게이트웨이가 같습니다.")
        else:
            net = ipaddress.IPv4Network(f"{ip_obj}/{prefix}", strict=False)
            if gw_obj not in net:
                errors.append(
                    f"게이트웨이 {gw_obj} 가 {net} 대역 밖입니다. "
                    "이대로 적용하면 외부와 통신할 수 없습니다."
                )
            if ip_obj == net.network_address:
                errors.append("네트워크 주소는 장치 IP로 쓸 수 없습니다.")
            if ip_obj == net.broadcast_address:
                errors.append("브로드캐스트 주소는 장치 IP로 쓸 수 없습니다.")

    return errors, (str(ip_obj) if ip_obj else None), prefix, ",".join(dns_list)


def login_required(view):
    @functools.wraps(view)
    def wrapped(*args, **kwargs):
        if not session.get("authenticated"):
            return redirect(url_for("login"))
        return view(*args, **kwargs)

    return wrapped


@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        entered = request.form.get("password", "").strip()
        cfg = load_config()

        if entered and entered == cfg["admin_password"]:
            session.clear()
            session["authenticated"] = True
            return redirect(url_for("index"))

        time.sleep(FAILED_LOGIN_DELAY_SECONDS)
        flash("비밀번호가 올바르지 않습니다.", "error")
        return redirect(url_for("login"))

    if session.get("authenticated"):
        return redirect(url_for("index"))
    return render_template("login.html")


@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login"))


@app.route("/", methods=["GET", "POST"])
@login_required
def index():
    if request.method == "POST":
        cfg = load_config()

        db_path = request.form.get("db_path", "").strip()
        door_open_seconds_raw = request.form.get("door_open_seconds", "").strip()
        new_password = request.form.get("new_password", "").strip()
        new_password_confirm = request.form.get("new_password_confirm", "").strip()

        errors = []
        if not db_path:
            errors.append("DB 경로는 비어 있을 수 없습니다.")

        door_open_seconds = None
        try:
            door_open_seconds = int(door_open_seconds_raw)
            if not (DOOR_OPEN_SECONDS_MIN <= door_open_seconds <= DOOR_OPEN_SECONDS_MAX):
                errors.append(
                    f"도어 릴레이 동작 시간은 {DOOR_OPEN_SECONDS_MIN}~{DOOR_OPEN_SECONDS_MAX}초 "
                    "사이여야 합니다 (IDTi Relay ActiveTime 규격)."
                )
        except ValueError:
            errors.append("도어 릴레이 동작 시간은 숫자여야 합니다.")

        admin_password = cfg["admin_password"]
        password_changed = False
        if new_password or new_password_confirm:
            if not PASSWORD_RE.match(new_password):
                errors.append("새 비밀번호는 숫자 4자리여야 합니다.")
            elif new_password != new_password_confirm:
                errors.append("새 비밀번호 확인이 일치하지 않습니다.")
            else:
                admin_password = new_password
                password_changed = True

        if errors:
            for e in errors:
                flash(e, "error")
        else:
            cfg["db_path"] = db_path
            cfg["door_open_seconds"] = door_open_seconds
            cfg["admin_password"] = admin_password
            save_config(cfg)

            ok, msg = reload_daemon()
            flash(msg, "success" if ok else "warning")
            if password_changed:
                flash("단말기 비밀번호가 변경되었습니다.", "success")

        return redirect(url_for("index"))

    cfg = load_config()
    return render_template("index.html", cfg=cfg)


@app.route("/network", methods=["GET", "POST"])
@login_required
def network():
    if request.method == "POST":
        ip_raw = request.form.get("ip", "").strip()
        prefix_raw = request.form.get("prefix", "").strip()
        gateway_raw = request.form.get("gateway", "").strip()
        dns_raw = request.form.get("dns", "").strip()

        errors, ip_ok, prefix, dns_norm = validate_network_form(
            ip_raw, prefix_raw, gateway_raw, dns_raw
        )
        if errors:
            for e in errors:
                flash(e, "error")
            return redirect(url_for("network"))

        # 헬퍼가 ARP 중복 주소 감지(arping -D)로 충돌을 먼저 확인하고,
        # 적용과 동시에 자동 롤백을 예약한다.
        ok, out = netcfg(
            "apply", ip_ok, str(prefix), gateway_raw, dns_norm, str(ROLLBACK_SECONDS)
        )
        if not ok:
            flash(out or "네트워크 설정 적용에 실패했습니다.", "error")
            return redirect(url_for("network"))

        # 여기서부터 이 브라우저는 옛 주소로 보고 있다. 새 주소로 옮겨가야 한다.
        return render_template(
            "network_applied.html",
            new_ip=ip_ok,
            port=WEBUI_PORT,
            seconds=ROLLBACK_SECONDS,
        )

    info = netcfg_show()
    if info is None:
        flash(
            "네트워크 설정을 읽지 못했습니다. acu-netcfg 헬퍼가 설치돼 있고 "
            "sudo 권한(/etc/sudoers.d/acu-netcfg)이 있는지 확인하세요.",
            "warning",
        )
    return render_template("network.html", info=info, seconds=ROLLBACK_SECONDS)


@app.route("/network/confirm", methods=["POST"])
@login_required
def network_confirm():
    ok, out = netcfg("confirm")
    flash(out or ("확정했습니다." if ok else "확정에 실패했습니다."), "success" if ok else "error")
    return redirect(url_for("network"))


@app.route("/network/rollback", methods=["POST"])
@login_required
def network_rollback():
    ok, out = netcfg("rollback")
    flash(out or ("되돌렸습니다." if ok else "되돌리지 못했습니다."), "success" if ok else "error")
    return redirect(url_for("network"))


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
