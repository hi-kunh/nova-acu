#!/usr/bin/env python3
"""
사용자 바이너리 전송(`5. IDTi Protocol UserBinaryTransmit.doc`) 수신 동작 확인.

돌고 있는 acud에 붙어 어려운 경우만 골라 본다:

  [1] 정상 전송            명단이 교체되고 받은 카드로 판정된다
  [2] 검사값 깨진 레코드    그 한 명만 버리고 나머지는 등록 (이벤트 0x101D0101)
  [3] 전송 중 연결 끊김     기존 명단이 그대로 남는다 (반쪽 명단이 생기지 않는다)
  [4] 같은 번호 재전송      Success로 답하고 두 번 넣지 않는다
                            (PC는 Fail을 받으면 직전 조각을 다시 보낸다 - 규약)
  [5] 조각 번호 건너뜀      Fail로 답하고 받던 것을 버린다

사용법:
    python3 tools/userbin_test.py --port 19900 --users-db <경로>

`--users-db`는 인원을 직접 세기 위한 것이라 **acud와 같은 기계에서 돌릴 때만** 쓴다.
없으면 인원 확인 없이 프로토콜 응답만 본다.
"""
import argparse
import importlib.util
import os
import socket
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("idti_client", os.path.join(HERE, "idti_client.py"))
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)

fails = []


def want(label, got, exp):
    ok = got == exp
    print(f"  {'✓' if ok else '✗'} {label}: {got}" + ("" if ok else f"  (기대 {exp})"))
    if not ok:
        fails.append(label)


class Sender:
    def __init__(self, host, port, timeout):
        self.host, self.port, self.timeout = host, port, timeout
        self.sock = None

    def connect(self):
        self.close()
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def start(self, size, count):
        data = size.to_bytes(4, "big") + count.to_bytes(4, "big") + bytes([0x01, 0x00])
        self.sock.sendall(c.build_request(c.CMD_SND_DATA, c.SUBCMD_WRITE,
                                          c.OBJ_USERBIN_START, frame_index=1, data=data))
        return c.ACK_NAMES.get(c.userbin_ack(c.recv_packet(self.sock, self.timeout)))

    def cont(self, index, payload):
        data = index.to_bytes(2, "big") + payload
        self.sock.sendall(c.build_request(c.CMD_SND_DATA, c.SUBCMD_WRITE,
                                          c.OBJ_USERBIN_CONTINUE, frame_index=2, data=data))
        return c.ACK_NAMES.get(c.userbin_ack(c.recv_packet(self.sock, self.timeout)))


def records(n, base=0):
    """사용자 n명분 _SSCUserInfo(128byte)"""
    out = b""
    for i in range(n):
        out += c.make_user_record(base + i,
                                  (base + i + 1).to_bytes(8, "big"),
                                  (0xAAAA0000 + base + i).to_bytes(8, "big"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19900)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--users-db", help="인원을 직접 세려면 users.db 경로 (같은 기계일 때만)")
    args = ap.parse_args()

    def count():
        if not args.users_db:
            return None
        return sqlite3.connect(args.users_db).execute("SELECT COUNT(*) FROM users").fetchone()[0]

    s = Sender(args.host, args.port, args.timeout)
    base_count = count()
    print(f"시작 인원: {base_count}")

    print("\n[1] 정상 전송 (20명)")
    s.connect()
    blob = records(20, base=100)
    want("Start", s.start(len(blob), 20), "Success")
    want("조각 0", s.cont(0, blob[:1280]), "Success")
    want("조각 1", s.cont(1, blob[1280:]), "Success")
    time.sleep(0.6)
    if base_count is not None:
        want("교체 인원", count(), 20)
    s.close()
    time.sleep(0.3)

    print("\n[2] 검사값이 깨진 레코드 1개")
    s.connect()
    blob = bytearray(records(10, base=200))
    blob[127] ^= 0xFF
    want("Start", s.start(len(blob), 10), "Success")
    want("조각 0", s.cont(0, bytes(blob)), "Success")
    time.sleep(0.6)
    if base_count is not None:
        want("등록 인원 (1명 버림)", count(), 9)
    s.close()
    time.sleep(0.3)

    print("\n[3] 전송 중 연결 끊김")
    s.connect()
    blob = records(40, base=300)
    want("Start", s.start(len(blob), 40), "Success")
    want("조각 0 (절반만 보낸다)", s.cont(0, blob[:2560]), "Success")
    s.close()
    time.sleep(0.8)
    if base_count is not None:
        want("기존 명단 유지", count(), 9)

    print("\n[4] 같은 번호 재전송 (Fail 뒤 PC의 재전송 흉내)")
    s.connect()
    blob = records(20, base=500)
    want("Start", s.start(len(blob), 20), "Success")
    want("조각 0", s.cont(0, blob[:1280]), "Success")
    want("조각 0 재전송", s.cont(0, blob[:1280]), "Success")
    want("조각 1", s.cont(1, blob[1280:]), "Success")
    time.sleep(0.6)
    if base_count is not None:
        want("겹치지 않고 20명", count(), 20)
    s.close()
    time.sleep(0.3)

    print("\n[5] 조각 번호 건너뜀")
    s.connect()
    blob = records(30, base=900)
    want("Start", s.start(len(blob), 30), "Success")
    want("조각 0", s.cont(0, blob[:1280]), "Success")
    want("조각 5 (건너뜀)", s.cont(5, blob[1280:]), "Fail")
    s.close()
    time.sleep(0.8)
    if base_count is not None:
        want("앞의 20명 유지", count(), 20)

    print("\n" + ("전부 통과" if not fails else f"실패 {len(fails)}건: {fails}"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
