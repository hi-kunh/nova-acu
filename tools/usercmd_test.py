#!/usr/bin/env python3
"""
사용자 1명씩 명령(`1. IDTi Protocol Basic structure`) 수신 동작 확인.

  전송  Cmd 5 / Sub 3 / Obj 0x21   Data(112)  -> Result(1)
  삭제  Cmd 5 / Sub 4 / Obj 0x15   Data(12)   -> Result(1)
  받기  Cmd 6 / Sub 2 / Obj 0x21   Data(12)   -> UserData(112)

보는 것:
  [1] 등록 -> 카드로 판정되고, 되받으면 보낸 값이 그대로 나온다 (왕복)
  [2] 없는 사용자 받기 -> Fail
  [3] 같은 사용자 다시 등록 -> 덮어쓴다 (겹치지 않는다)
  [4] 삭제 -> 받기도 실패하고 그 사람 카드도 사라진다

사용법: python3 tools/usercmd_test.py --port 19900 [--users-db <경로>]
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


class Client:
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.timeout = timeout
        self.fi = 1

    def close(self):
        self.sock.close()

    def _send(self, cmd, sub, obj, data):
        self.fi += 1
        self.sock.sendall(c.build_request(cmd, sub, obj, frame_index=self.fi, data=data))
        return c.recv_packet(self.sock, self.timeout)

    def set_user(self, payload):
        return c.ACK_NAMES.get(c.userbin_ack(
            self._send(c.CMD_SND_DATA, c.SUBCMD_WRITE, c.OBJ_USER_DATA, payload)))

    def del_user(self, user_id):
        return c.ACK_NAMES.get(c.userbin_ack(
            self._send(c.CMD_SND_DATA, c.SUBCMD_DELETE, c.OBJ_USER_ALL, c.user_key(user_id))))

    def get_user(self, user_id):
        """돌려받은 UserData(112) 또는 None (Fail이면 None)"""
        resp = self._send(c.CMD_REQ_DATA, c.SUBCMD_READ, c.OBJ_USER_DATA, c.user_key(user_id))
        if resp is None:
            return None
        body = resp[c.HEADER_LEN:-c.TAIL_LEN]
        opt = int.from_bytes(resp[4:6], "big")
        if not (opt & c.FOPT_EXCLUDE_DEVICE_STATUS) and len(body) >= c.DEVICE_STATUS_V2_LEN:
            body = body[c.DEVICE_STATUS_V2_LEN:]
        return bytes(body) if len(body) >= c.USERDATA_LEN else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19900)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--users-db", help="인원을 직접 세려면 users.db 경로 (같은 기계일 때만)")
    args = ap.parse_args()

    def count(table="users"):
        if not args.users_db:
            return None
        return sqlite3.connect(args.users_db).execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]

    uid = (0x777).to_bytes(8, "big")
    cid = bytes.fromhex("00000000DEADBEEF")
    cl = Client(args.host, args.port, args.timeout)
    before = count()
    print(f"시작 인원: {before}")

    print("\n[1] 등록 -> 되받기 (왕복)")
    payload = c.make_user_data(uid, cid, level=9, timezone=0)
    want("전송", cl.set_user(payload), "Success")
    time.sleep(0.3)
    if before is not None:
        want("인원 +1", count(), before + 1)
    got = cl.get_user(uid)
    want("받기 성공", got is not None, True)
    if got:
        want("User ID 일치", got[0:8], uid)
        want("Level 일치", got[16], 9)
        want("Access Option(Enable) 일치", got[12], 0x80)
        want("이름 일치", got[32:40], b"NCU TEST")
        want("카드 일치 (뒤집힌 형태 그대로)", got[48:56], bytes(reversed(cid)))

    print("\n[2] 없는 사용자 받기")
    want("Fail (데이터 없음)", cl.get_user((0x999999).to_bytes(8, "big")) is None, True)

    print("\n[3] 같은 사용자 다시 등록 (덮어쓰기)")
    want("전송", cl.set_user(c.make_user_data(uid, cid, level=3)), "Success")
    time.sleep(0.3)
    if before is not None:
        want("인원 그대로 (겹치지 않음)", count(), before + 1)
    got = cl.get_user(uid)
    want("Level이 바뀜", got[16] if got else None, 3)

    print("\n[4] 삭제")
    want("삭제", cl.del_user(uid), "Success")
    time.sleep(0.3)
    want("받기 실패", cl.get_user(uid) is None, True)
    if before is not None:
        want("인원 원복", count(), before)
        want("카드도 사라짐", count("user_cards"), before)

    cl.close()
    print("\n" + ("전부 통과" if not fails else f"실패 {len(fails)}건: {fails}"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
