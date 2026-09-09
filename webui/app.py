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
import json
import os
import re
import signal
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


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
