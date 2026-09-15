#!/usr/bin/env python3
"""
금요일 몫 설정 6종 — 조회·쓰기·되읽기를 확인한다.

주소는 **9/15 실제 DM 트래픽**(Platinum「장치 정보 업데이트」)에서 읽은 그대로 쓴다:
  0x2B 컨트롤러 장치   모듈 0 / 비트맵 0
  0x2F 카드리더        모듈 1·2 × bit 0·1
  0x51 알람            모듈 0 / 비트맵 0
  0x52 알람벨 스케줄   모듈 0 / 비트맵 0
  0x53 운영모드 스케줄 모듈 0 (컨트롤러) + 모듈 1·2 × bit 0·1 (리더별)
  0x54 도어모드 스케줄 모듈 1·2 × bit 8~11

크기는 SDK 필드 길이 합 (0x29·0x2C·0x2D는 캡처와 일치했다).

사용법: python3 tools/setting_all_test.py --port 19931
"""
import argparse
import importlib.util
import os
import socket
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("idti_client", os.path.join(HERE, "idti_client.py"))
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)

fails = []


def want(label, got, exp):
    ok = got == exp
    print(f"    {'✓' if ok else '✗'} {label}: {got!r}" + ("" if ok else f"  (기대 {exp!r})"))
    if not ok:
        fails.append(label)


def frame(cmd, sub, obj, module, bitmap, data=b""):
    req = bytearray(c.build_request(cmd, sub, obj, frame_index=1, data=data))
    req[7:11] = bytes([1, 1, 1, module])
    req[11:15] = bitmap.to_bytes(4, "big")
    cs = 0
    for b in req[0:31]:
        cs ^= b
    req[31] = cs
    return bytes(req)


def exchange(port, req):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(req)
    r = c.recv_packet(s, 5)
    s.close()
    return r


def body(r):
    return bytes(r[44 + 234:-2]) if r else None


def events(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(c.build_request(c.CMD_REQ_DATA, c.SUBCMD_READ, c.OBJ_HISTORY, frame_index=1,
                              frame_option=c.FOPT_REQUEST_ACK | c.FOPT_BLOCKING | c.FOPT_TCP))
    r = c.recv_packet(s, 5)
    s.close()
    d = body(r) or b""
    return [(hex(int.from_bytes(d[i:i+4], "big")), d[i+6], d[i+7]) for i in range(0, len(d) - len(d) % 36, 36)]


def pattern(obj, n, salt=0):
    """오브젝트마다 알아볼 수 있는 byte 열 (되읽기가 섞이지 않았는지 보려고)"""
    return bytes(((obj * 7 + i * 13 + salt) & 0xFF) for i in range(n))


# (오브젝트, 이름, 크기, [(모듈, 비트맵) 주소들], 설정 성공 이벤트)
CASES = [
    (0x2B, "컨트롤러 장치",   24, [(0, 0)],                                None),
    (0x2F, "카드리더",        32, [(1, 1 << 0), (1, 1 << 1), (2, 1 << 0), (2, 1 << 1)], 0x10230301),
    (0x51, "알람",            21, [(0, 0)],                                None),
    (0x52, "알람벨 스케줄",   16, [(0, 0)],                                None),
    (0x53, "운영모드 스케줄",  7, [(0, 0), (1, 1 << 0), (2, 1 << 1)],       None),
    (0x54, "도어모드 스케줄",  7, [(1, 1 << 8), (1, 1 << 11), (2, 1 << 9)], None),
]

# 단위가 틀린 주소 — Fail이어야 한다
WRONG = [
    (0x2F, "카드리더에 입력 자리(bit2)", 1, 1 << 2),
    (0x54, "도어모드에 리더 자리(bit0)", 1, 1 << 0),
    (0x2F, "카드리더에 없는 모듈 3",     3, 1 << 0),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19931)
    P = ap.parse_args().port

    events(P)

    for obj, name, n, addrs, ev in CASES:
        print(f"\n[0x{obj:02X}] {name} — {n}byte, 주소 {len(addrs)}곳")
        m, bm = addrs[0]
        want("쓰기 전 조회 = Fail", body(exchange(P, frame(0x06, 0x02, obj, m, bm))), b"\x02")

        for k, (m, bm) in enumerate(addrs):
            data = pattern(obj, n, salt=k)
            want(f"쓰기 모듈{m} 비트맵{bm:08x}", body(exchange(P, frame(0x05, 0x05, obj, m, bm, data))), b"\x01")

        for k, (m, bm) in enumerate(addrs):
            r = exchange(P, frame(0x06, 0x02, obj, m, bm))
            want(f"되읽기 모듈{m} 비트맵{bm:08x}", body(r), pattern(obj, n, salt=k))
            if r:
                want("  OneDataBlockSize", int.from_bytes(r[42:44], "big"), n)

        got = events(P)
        if ev:
            expect = [(hex(ev), m, (bm.bit_length()) if m else 0) for m, bm in addrs]
            want("설정 성공 이벤트 (모듈, 칸 1부터)", got, expect)
        else:
            want("설정 성공 이벤트 없음 (코드표에 없다)", got, [])

    print("\n[주소 단위가 틀린 쓰기]")
    for obj, label, m, bm in WRONG:
        want(label, body(exchange(P, frame(0x05, 0x05, obj, m, bm, pattern(obj, 32)))), b"\x02")
    want("짧은 데이터(알람 3byte)", body(exchange(P, frame(0x05, 0x05, 0x51, 0, 0, b"\x01\x02\x03"))), b"\x02")
    want("이벤트 없음", events(P), [])

    print("\n" + ("전부 통과" if not fails else f"실패 {len(fails)}건: {fails}"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
