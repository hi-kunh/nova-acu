#!/usr/bin/env python3
"""
장치 설정 쓰기(Cmd 5 / Sub 5)와 **알람 릴레이 규칙**을 확인한다.

  [1] 출력 3칸을 쓴다 -> Success + 설정 성공 이벤트 10220301 (모듈, 칸 1부터)
        모듈1 칸9  : Alarm + 화재로 동작(bit7)
        모듈1 칸10 : Alarm
        모듈2 칸11 : Door
  [2] 되읽는다 -> 쓴 byte 그대로
  [3] 입력 1칸을 쓴다 -> 10210301
  [4] 잘못된 자리에 쓴다 -> Fail, 이벤트 없음
  [5] 알람·화재 입력을 흉내 내 **어느 출력이 켜지는지** 로그로 본다
        알람 동작          -> 칸9·칸10 켬 (Door인 칸11은 건드리지 않음)
        알람 복구 + 화재    -> 칸9만 켬 (bit7), 칸10 끔
        화재 복구          -> 전부 끔

사용법: python3 tools/setting_write_test.py --port 19921 --fifo <mock.fifo> --log <acud.log>
"""
import argparse
import importlib.util
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("idti_client", os.path.join(HERE, "idti_client.py"))
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)

fails = []


def want(label, got, exp):
    ok = got == exp
    print(f"    {'✓' if ok else '✗'} {label}: {got}" + ("" if ok else f"  (기대 {exp})"))
    if not ok:
        fails.append(label)


def frame(cmd, sub, obj, module, bit, data=b""):
    """캡처와 같은 모양의 요청 — 모듈은 offset 10, 칸은 offset 11~14 비트맵"""
    req = bytearray(c.build_request(cmd, sub, obj, frame_index=1, data=data))
    req[7:11] = bytes([1, 1, 1, module])
    req[11:15] = (1 << bit).to_bytes(4, "big")
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


def output_block(active_type, by_fire=False, active_time=3):
    b = bytearray(14)
    b[0:4] = b"\x80\x00\x00\x00"
    b[6] = active_type
    b[7] = active_time
    b[10] = 0x80 if by_fire else 0x00
    return bytes(b)


def events(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(c.build_request(c.CMD_REQ_DATA, c.SUBCMD_READ, c.OBJ_HISTORY, frame_index=1,
                              frame_option=c.FOPT_REQUEST_ACK | c.FOPT_BLOCKING | c.FOPT_TCP))
    r = c.recv_packet(s, 5)
    s.close()
    data = body(r) or b""
    out = []
    for i in range(len(data) // 36):
        ev = data[i * 36:(i + 1) * 36]
        out.append((int.from_bytes(ev[0:4], "big"), ev[6], ev[7]))
    return out


def log_since(path, mark):
    # mark는 byte 위치다 - 한글 로그라 문자 수로 자르면 어긋난다
    with open(path, "rb") as f:
        f.seek(mark)
        return f.read().decode("utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19921)
    ap.add_argument("--fifo", required=True)
    ap.add_argument("--log", required=True)
    args = ap.parse_args()
    P = args.port

    events(P)  # 쌓여 있던 것을 비운다

    print("\n[1] 출력 3칸 쓰기")
    plan = [(1, 8, output_block(2, by_fire=True)),   # 칸9
            (1, 9, output_block(2)),                 # 칸10
            (2, 10, output_block(1))]                # 칸11 Door
    for module, bit, blk in plan:
        r = exchange(P, frame(0x05, 0x05, 0x2D, module, bit, blk))
        want(f"모듈{module} 칸{bit + 1} 쓰기", body(r), b"\x01")
    time.sleep(0.3)
    want("설정 성공 이벤트", events(P),
         [(0x10220301, 1, 9), (0x10220301, 1, 10), (0x10220301, 2, 11)])

    print("\n[2] 되읽기 — 쓴 byte 그대로")
    for module, bit, blk in plan:
        r = exchange(P, frame(0x06, 0x02, 0x2D, module, bit))
        want(f"모듈{module} 칸{bit + 1}", body(r), blk)

    print("\n[3] 입력 1칸 쓰기 (모듈2 칸14 = 비트13, 화재 입력 자리)")
    inp = bytes.fromhex("80000000 00 00 03 00 00 00 0000 0000 0000 00 00 00 0000".replace(" ", ""))
    r = exchange(P, frame(0x05, 0x05, 0x2C, 2, 13, inp))
    want("쓰기", body(r), b"\x01")
    time.sleep(0.3)
    want("이벤트", events(P), [(0x10210301, 2, 14)])
    want("되읽기", body(exchange(P, frame(0x06, 0x02, 0x2C, 2, 13))), inp)

    print("\n[4] 잘못된 자리 — Fail, 이벤트 없음")
    want("출력에 입력 자리(비트2)", body(exchange(P, frame(0x05, 0x05, 0x2D, 1, 2, output_block(2)))), b"\x02")
    want("없는 모듈 3", body(exchange(P, frame(0x05, 0x05, 0x2D, 3, 8, output_block(2)))), b"\x02")
    want("데이터가 짧다", body(exchange(P, frame(0x05, 0x05, 0x2D, 1, 8, b"\x80"))), b"\x02")
    time.sleep(0.3)
    want("이벤트", events(P), [])

    print("\n[5] 알람 릴레이 규칙")
    def step(cmd, label, expect_on, expect_off, expect_untouched):
        mark = os.path.getsize(args.log)
        with open(args.fifo, "w") as f:
            f.write(cmd + "\n")
        time.sleep(0.6)
        text = log_since(args.log, mark)
        print(f"  {label}")
        for m, sl in expect_on:
            want(f"(모듈 {m}, 칸 {sl}) 켬", f"(모듈 {m}, 칸 {sl}) 켬" in text, True)
        for m, sl in expect_off:
            want(f"(모듈 {m}, 칸 {sl}) 끔", f"(모듈 {m}, 칸 {sl}) 끔" in text, True)
        for m, sl in expect_untouched:
            want(f"(모듈 {m}, 칸 {sl}) 안 건드림", f"(모듈 {m}, 칸 {sl})" in text, False)

    step("alarm on",  "알람 동작",       [(1, 9), (1, 10)], [],        [(2, 11)])
    step("fire on",   "화재까지 동작",   [(1, 9), (1, 10)], [],        [(2, 11)])
    step("alarm off", "알람 복구(화재 남음)", [(1, 9)],      [(1, 10)], [(2, 11)])
    step("fire off",  "화재 복구",       [],                [(1, 9), (1, 10)], [(2, 11)])

    print("\n[6] 카드리더 쓰기 — DM이 SSC-324(ACU04)에 보낸 프레임 그대로 (DM 회신 9/15 5절)")
    capture = bytes.fromhex(
        "02 00 4E 02 88 01 2C 01 01 01 01 00 00 00 01 01 01 01 01 01 00 01 00 01 00 00 00 00 00 05 05 EB"
        " 01 2F 01 FF 00 01 00 01 00 01 00 00"
        " 00 00 A8 44 80 F2 01 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
        " 08 03")
    events(P)  # 5절 알람·화재 이벤트를 비운다
    r = exchange(P, capture)
    want("응답 받음", r is not None, True)
    if r:
        # SSC-324 응답: 88 01 ... 05 05 CS 00 2F FF FF 00 01 00 01 00 01 00 01 + 상태 234 + 01 + 08 03
        want("FrameOption (요청 에코)", bytes(r[4:6]).hex(), "8801")
        want("Frame 인덱스", bytes(r[20:24]).hex(), "00010001")
        want("Cmd/Sub", bytes(r[29:31]).hex(), "0505")
        want("DataType/Object", bytes(r[32:34]).hex(), "002f")
        want("Item", bytes(r[34:36]).hex(), "ffff")
        want("블록 인덱스·크기", bytes(r[36:44]).hex(), "0001000100010001")
        want("길이 (헤더44 + 상태234 + 1 + 2)", len(r), 281)
        want("결과", body(r), b"\x01")
    want("설정 성공 이벤트 10230301 (1, 1)", events(P), [(0x10230301, 1, 1)])
    back = body(exchange(P, frame(0x06, 0x02, 0x2F, 1, 0)))
    want("되읽기 = 받은 32byte", back, capture[44:76])

    print("\n" + ("전부 통과" if not fails else f"실패 {len(fails)}건: {fails}"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
