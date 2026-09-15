#!/usr/bin/env python3
"""
장치 설정 조회 3종을 **SSC-324 캡처와 byte 단위로** 비교한다.

캡처 출처: dm/DM_회신_Object번호확정_원시바이트_20260914.md 3절 (DM이 실물 ACU04와 주고받은 바이트)

  0x29 컨트롤러 기본설정   13byte
  0x2C 입력                 21byte   (요청 비트맵 bit2)
  0x2D 출력                 14byte   (요청 비트맵 bit8)

방법:
  1. 캡처의 **요청 프레임을 한 byte도 바꾸지 않고** acud에 보낸다
  2. 입력·출력은 캡처의 응답 데이터를 미리 acud.db에 넣어 둔다 (같은 설정을 가진 장비로 만든다)
  3. 응답의 헤더 필드와 데이터를 캡처와 맞춰 본다

**일치해야 하는 것** (DM 회신 4-3에서 NCU가 채워야 한다고 한 것 + 데이터):
  FrameLength · Command/Sub/Object · DataType · StartItem/EndItem · 블록 인덱스 3개 ·
  OneDataBlockSize · 데이터 블록
**참고로만 보는 것** (장비마다 다르거나 의미를 모르는 칸):
  FrameOption · 주소 · Password(캡처는 5B 30 01 54) · 장치상태(시각이 들어 있다)

사용법: python3 tools/setting_read_test.py --port 19920 --db <acud.db 경로>
"""
import argparse
import importlib.util
import os
import socket
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("idti_client", os.path.join(HERE, "idti_client.py"))
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)


def hx(s):
    return bytes.fromhex(s.replace("\n", " "))


# ---- DM 회신 3절 캡처 ----
CAPTURES = {
    0x29: {
        "name": "컨트롤러 기본설정",
        "req": hx("02 00 2E 02 88 01 2C 01 01 01 00 00 00 00 00 01 01 01 01 01 00 01 00 01"
                  "00 00 00 00 00 06 02 8F 01 29 01 FF 00 01 00 01 00 01 00 00 08 03"),
        "resp_len": 293,
        "data": hx("03 21 01 05 FF 00 00 00 00 00 00 00 00"),
        "store": None,   # 기본값으로 답해야 한다
    },
    0x2C: {
        "name": "입력",
        "req": hx("02 00 2E 02 88 01 2C 01 01 01 01 00 00 00 04 01 01 01 01 01 00 01 00 01"
                  "00 00 00 00 00 06 02 8A 01 2C 01 FF 00 01 00 01 00 01 00 00 08 03"),
        "resp_len": 301,
        "data": hx("80 00 00 00 01 01 01 00 01 00 00 00 00 00 00 00 00 00 00 00 00"),
        "store": (1, 2),  # 모듈 1, 칸 2 (비트맵 bit2)
    },
    0x2D: {
        "name": "출력",
        "req": hx("02 00 2E 02 88 01 2C 01 01 01 01 00 00 01 00 01 01 01 01 01 00 01 00 01"
                  "00 00 00 00 00 06 02 8F 01 2D 01 FF 00 01 00 01 00 01 00 00 08 03"),
        "resp_len": 294,
        "data": hx("80 00 00 00 00 00 01 03 00 00 00 00 00 00"),
        "store": (1, 8),  # 모듈 1, 칸 8 (비트맵 bit8)
    },
}

fails = []


def want(label, got, exp):
    ok = got == exp
    shown = got.hex(" ") if isinstance(got, (bytes, bytearray)) else got
    print(f"    {'✓' if ok else '✗'} {label:<22} {shown}" + ("" if ok else f"   (캡처 {exp.hex(' ') if isinstance(exp, (bytes, bytearray)) else exp})"))
    if not ok:
        fails.append(label)


def exchange(port, frame):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(frame)
    resp = c.recv_packet(s, 5)
    s.close()
    return resp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19920)
    ap.add_argument("--db", required=True, help="acud가 쓰는 acud.db 경로 (설정을 미리 넣는다)")
    args = ap.parse_args()

    db = sqlite3.connect(args.db)
    for obj, cap in CAPTURES.items():
        if cap["store"]:
            module, slot = cap["store"]
            db.execute("INSERT OR REPLACE INTO device_settings(object,module,slot,data) VALUES(?,?,?,?)",
                       (obj, module, slot, cap["data"]))
    db.commit()

    for obj, cap in CAPTURES.items():
        print(f"\n[0x{obj:02X}] {cap['name']}  — 캡처 요청을 그대로 보낸다")
        r = exchange(args.port, cap["req"])
        if r is None:
            print("    ✗ 응답 없음")
            fails.append(cap["name"])
            continue

        want("FrameLength", len(r), cap["resp_len"])
        want("Command / Sub", (r[29], r[30]), (0x06, 0x02))
        want("Object", r[33], obj)
        want("DataType", r[32], 0x00)
        want("StartItem / EndItem", (r[34], r[35]), (0xFF, 0xFF))
        want("블록 Start/End/Count", (r[36:38], r[38:40], r[40:42]),
             (b"\x00\x01", b"\x00\x01", b"\x00\x01"))
        want("OneDataBlockSize", int.from_bytes(r[42:44], "big"), len(cap["data"]))
        data = bytes(r[44 + 234:-2])
        want("데이터", data, cap["data"])
        print(f"    · 참고 FrameOption {r[4:6].hex(' ')} / 주소 {r[7:15].hex(' ')} / 송신 {r[15:20].hex(' ')}"
              f" / Password {r[24:28].hex(' ')}")

    # ---- 저장된 것이 없는 칸은 Fail ----
    print("\n[0x2C] 입력 — 저장된 것이 없는 칸 (모듈 1, 칸 3 = bit3)")
    req = bytearray(CAPTURES[0x2C]["req"])
    req[11:15] = (1 << 3).to_bytes(4, "big")
    cs = 0
    for b in req[0:31]:
        cs ^= b
    req[31] = cs
    r = exchange(args.port, bytes(req))
    body = bytes(r[44 + 234:-2]) if r else b""
    want("Fail 한 byte", body, b"\x02")

    print("\n[0x2D] 출력 — 입력 자리를 물으면 (칸 2 = bit2)")
    req = bytearray(CAPTURES[0x2D]["req"])
    req[11:15] = (1 << 2).to_bytes(4, "big")
    cs = 0
    for b in req[0:31]:
        cs ^= b
    req[31] = cs
    r = exchange(args.port, bytes(req))
    body = bytes(r[44 + 234:-2]) if r else b""
    want("Fail 한 byte", body, b"\x02")

    print("\n" + ("전부 통과" if not fails else f"실패 {len(fails)}건: {fails}"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
