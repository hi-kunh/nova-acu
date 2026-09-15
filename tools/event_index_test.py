#!/usr/bin/env python3
"""
이벤트 보관 명령 3종 + 읽기 위치 자동 복구를 확인한다.

  [1] 카드 3장 -> EventCountCheck = 3 -> 폴링으로 받기 -> 0
  [2] EventIndexChange 타입 3 (지정 시각부터) -> 다시 3건이 올라온다
  [3] EventIndexChange 타입 4 / 모르는 타입 -> Fail (거짓 성공 금지)
  [4] EventReset -> 0 (지우지 않음 - 타입 3으로 되살아난다)
  [5] ⚠ 현장 SSC-324 고장 재현: 읽기 위치를 쓰기 위치보다 앞으로 밀어 둔다
      -> 새 카드가 **그래도 올라와야 한다** (acud가 스스로 되돌린다)

사용법: python3 tools/event_index_test.py --port 19922 --fifo <mock.fifo> --events-db <events.db>
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
    print(f"    {'✓' if ok else '✗'} {label}: {got}" + ("" if ok else f"  (기대 {exp})"))
    if not ok:
        fails.append(label)


def ask(port, cmd, sub, obj, data=b""):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(c.build_request(cmd, sub, obj, frame_index=1, data=data))
    r = c.recv_packet(s, 5)
    s.close()
    return bytes(r[44 + 234:-2]) if r else None


def count(port):
    b = ask(port, 0x06, 0x02, 0x05)
    return int.from_bytes(b[0:4], "big") if b and len(b) >= 36 else None


def poll(port):
    b = ask(port, 0x06, 0x02, 0x01) or b""
    return len(b) // 36


def index_change(port, typ, start=b"\x00" * 6, end=b"\x00" * 6):
    data = bytearray(36)
    data[1] = typ
    data[6:12] = start
    data[12:18] = end
    return ask(port, 0x03, 0x05, 0x06, bytes(data))


def bcd(n):
    return bytes([(n // 10) << 4 | (n % 10)])


def tap(fifo, n):
    with open(fifo, "w") as f:
        for _ in range(n):
            f.write("04A1B2C3D4E5F600\n")
    time.sleep(0.6)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19922)
    ap.add_argument("--fifo", required=True)
    ap.add_argument("--events-db", required=True)
    a = ap.parse_args()
    P = a.port

    while poll(P):
        pass

    # 앞선 시험이 남긴 이벤트가 있을 수 있다 - 지금 보관 건수를 기준으로 삼는다
    db = sqlite3.connect(a.events_db)
    base_total = db.execute("SELECT COUNT(*) FROM events").fetchone()[0]
    # 되돌림 기준 시각은 **이 시험이 시작한 때**로 잡는다 (한 시간 전으로 잡으면 앞 시험 이벤트까지 섞인다).
    # 시각은 초 단위라, 앞 시험의 마지막 이벤트와 같은 초에 걸리지 않게 1초를 넘긴 뒤 **지금**을 기준으로 삼는다
    time.sleep(1.1)
    t_start = time.localtime(time.time())

    print("\n[1] 개수 조회")
    tap(a.fifo, 3)
    want("카드 3장 뒤 개수", count(P), 3)
    want("폴링으로 받은 건수", poll(P), 3)
    want("받은 뒤 개수", count(P), 0)

    print("\n[2] 타입 3 — 이 시험이 시작한 시각부터 다시")
    t = t_start
    start = b"".join(bcd(x) for x in (t.tm_year % 100, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec))
    want("응답", index_change(P, 3, start), b"\x01")
    want("되돌린 뒤 개수", count(P), 3)
    want("다시 받은 건수", poll(P), 3)

    print("\n[3] 구현 안 한 타입은 Fail")
    want("타입 4", index_change(P, 4, start, start), b"\x02")
    want("타입 1", index_change(P, 1), b"\x02")
    want("타입 3 + 틀린 시각", index_change(P, 3, b"\x99\x99\x99\x99\x99\x99"), b"\x02")

    print("\n[4] 리셋 — 보낸 것으로 표시, 지우지 않는다")
    tap(a.fifo, 2)
    want("리셋 전 개수", count(P), 2)
    want("리셋 응답", ask(P, 0x03, 0x06, 0x01), b"\x01")
    want("리셋 뒤 개수", count(P), 0)
    want("지워지지 않았다(보관 건수)", db.execute("SELECT COUNT(*) FROM events").fetchone()[0], base_total + 5)
    want("타입 3으로 되살아난다", (index_change(P, 3, start), count(P)), (b"\x01", 5))
    while poll(P):
        pass

    print("\n[5] 현장 고장 재현 — 읽기 위치가 쓰기 위치보다 앞선다")
    mx = db.execute("SELECT MAX(seq) FROM events").fetchone()[0]
    db.execute("UPDATE event_state SET value=? WHERE key='sent_seq'", (mx + 100,))
    db.commit()
    print(f"    · 쓰기 위치 {mx}, 읽기 위치를 {mx + 100}으로 밀어 둠")
    tap(a.fifo, 1)
    want("새 카드가 그래도 올라온다", poll(P), 1)
    # 응답은 전송 위치를 기록하기 **전에** 나간다(잃는 것보다 겹치는 편이 낫다는 설계).
    # 느린 eMMC에서는 기록이 몇 ms 뒤라, 바로 읽으면 한 칸 뒤처진 값이 보인다
    time.sleep(0.5)
    sent = db.execute("SELECT value FROM event_state WHERE key='sent_seq'").fetchone()[0]
    mx2 = db.execute("SELECT MAX(seq) FROM events").fetchone()[0]
    want("읽기 위치 == 쓰기 위치", sent, mx2)

    print("\n" + ("전부 통과" if not fails else f"실패 {len(fails)}건: {fails}"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
