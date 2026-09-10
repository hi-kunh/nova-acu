#!/usr/bin/env python3
"""
ACU 개통 테스트용 IDTi 프로토콜 V2 클라이언트.

상위 시스템(PC, IntelliScan NET Platinum)이 하는 일을 흉내낸다.
PC 쪽이 TCP 클라이언트로 장치에 접속해 오는 구조이므로, 이 스크립트도 접속하는 쪽이다.

사용 예:
    # 이벤트 로그 1회 조회
    python3 tools/idti_client.py --host 192.168.0.50

    # 2초마다 계속 조회 (실제 상위 시스템의 폴링 흉내)
    python3 tools/idti_client.py --host 192.168.0.50 --watch

    # 장치 상태 요청 (PC가 접속 후 보내는 첫 명령과 동일)
    python3 tools/idti_client.py --host 192.168.0.50 --request status

    # Device Status를 빼고 달라고 요청 (IsExcludeDeviceStatus 비트)
    python3 tools/idti_client.py --request status --exclude-status

    # 주고받은 바이트 그대로 보기
    python3 tools/idti_client.py --raw
"""

import argparse
import socket
import sys
import time

STX = 0x02
ETX = 0x03
HEADER_LEN = 44          # V2/V3 공통
TAIL_LEN = 2             # IsCheckPacket=0 일 때
PROTOCOL_V2 = 2
PACKET_CHECKSUM_FIXED = 0x08

CMD_SND_STATUS = 0x03
CMD_REQ_STATUS = 0x04
CMD_SND_DATA = 0x05
CMD_REQ_DATA = 0x06

SUBCMD_READ = 0x02

OBJ_HISTORY = 0x01
OBJ_FIRMWARE = 0x2A          # 42. PC가 접속 후 장치 상태를 물을 때 쓰는 오브젝트

DEVICE_STATUS_V2_LEN = 234
EVENT_INFO_LEN = 36
FIRMWARE_INFO_LEN = 268

# Frame Option 비트 (buf[4]<<8 | buf[5] 로 합친 값 기준)
FOPT_REQUEST_ACK = 0x8000
FOPT_EXCLUDE_DEVICE_STATUS = 0x0080
FOPT_CHECK_PACKET = 0x0010
FOPT_TCP = 0x0001

EVENT_NAMES = {
    0x01010102: "Access Authorized By Card (허용)",
    0x01020102: "Access Denied By Card (거부)",
    0x01020108: "Access Denied By Not Enabled (거부: 비활성 카드)",
    0x01020109: "Access Denied By Time (거부: 시간대)",
}

DOOR_STATUS_NAMES = {0x00: "None", 0x01: "Open(Not Closed)", 0x02: "Closed"}


def build_request(command, sub_command, obj,
                  frame_index=1, password=0,
                  start_item=0, end_item=0,
                  data=b"", frame_option=FOPT_REQUEST_ACK | FOPT_TCP):
    """IDTi V2 요청 패킷을 만든다 (Header 44 + Data + Tail 2)."""
    total_len = HEADER_LEN + len(data) + TAIL_LEN
    p = bytearray(HEADER_LEN)

    p[0] = STX
    p[1] = (total_len >> 8) & 0xFF
    p[2] = total_len & 0xFF
    p[3] = PROTOCOL_V2
    p[4] = (frame_option >> 8) & 0xFF
    p[5] = frame_option & 0xFF
    p[6] = HEADER_LEN
    # Destination Address (8byte): 장치 쪽. 1:1 연결이라 의미가 크지 않아 1로 채운다
    p[7:15] = bytes([1, 1, 1, 1, 1, 0, 0, 0])
    # Source Address (5byte): PC 쪽. 장치는 응답의 Destination에 이 값을 그대로 되돌려준다
    p[15:20] = bytes([1, 1, 1, 1, 1])
    p[20:24] = frame_index.to_bytes(4, "big")
    p[24:28] = password.to_bytes(4, "big")
    p[28] = 0x00             # Command Option
    p[29] = command
    p[30] = sub_command
    checksum = 0
    for b in p[0:31]:        # STX ~ SubCommand 의 XOR
        checksum ^= b
    p[31] = checksum
    p[32] = 0x00             # Object Type
    p[33] = obj
    p[34] = start_item
    p[35] = end_item
    p[36:38] = (0).to_bytes(2, "big")   # Data Block Current
    p[38:40] = (0).to_bytes(2, "big")   # Data Block End
    p[40:42] = (0).to_bytes(2, "big")   # Data Block Total
    p[42:44] = (0).to_bytes(2, "big")   # Data Block One Length

    return bytes(p) + data + bytes([PACKET_CHECKSUM_FIXED, ETX])


# Device Status의 모듈 배열 (근거: PC 소스 isldev/clsDevStatus.cs)
#   [8..9]  IsExistModule  빅엔디안 16bit, 모듈 N = bit N
#   [10..]  모듈 14개 x 16byte = ModuleIOType(1) + InstallType(1) + IOStatus(14)
#   IOStatus 각 바이트 = 상위 니블 IOType + 하위 니블 IOStatus
MODULE_COUNT = 14
MODULE_ENTRY_LEN = 16
MODULE_ARRAY_OFFSET = 10

IO_TYPE_NAME = {0: "-", 1: "지문리더", 2: "카드리더", 3: "입력", 4: "출력"}
IO_STATUS_NAME = {0: "N/A", 1: "Active", 2: "Inactive", 3: "LineOpen", 4: "LineShort"}
INSTALL_NAME = {0: "None", 1: "Internal", 2: "External"}


def describe_modules(ds):
    """Device Status에서 모듈 구성을 사람이 읽을 수 있게 풀어 준다."""
    lines = []
    existed = int.from_bytes(ds[8:10], "big")
    total = {}

    for i in range(MODULE_COUNT):
        if not (existed >> i) & 1:
            continue
        base = MODULE_ARRAY_OFFSET + i * MODULE_ENTRY_LEN
        mtype = ds[base]
        install = ds[base + 1]
        io = ds[base + 2:base + MODULE_ENTRY_LEN]

        # DM 문서(devmoduleNiocategory)와 같은 형태: 슬롯 14칸의 종류 니블을 이어 붙인 문자열
        category = "".join(str((b >> 4) & 0x0F) for b in io)
        for b in io:
            t = (b >> 4) & 0x0F
            if t != 0:
                total[t] = total.get(t, 0) + 1
        lines.append(f"    모듈{i + 1}: type={mtype} {INSTALL_NAME.get(install, install)} "
                     f"iocategory={category}")

    if total:
        summary = ", ".join(f"{IO_TYPE_NAME.get(t, t)} {n}" for t, n in sorted(total.items()))
        lines.append(f"    합계: {summary}")
    else:
        lines.append("    모듈 없음 (DM이 장치 트리를 만들지 못한다)")
    return lines


def bcd(byte):
    return (byte >> 4) * 10 + (byte & 0x0F)


def parse_response(pkt):
    """응답 패킷을 사람이 읽을 수 있게 풀어 준다."""
    out = []
    if len(pkt) < HEADER_LEN:
        return [f"응답이 너무 짧음 ({len(pkt)}byte)"]

    packet_length = int.from_bytes(pkt[1:3], "big")
    version = pkt[3]
    command = pkt[29]
    sub_command = pkt[30]
    obj = pkt[33]
    cur = int.from_bytes(pkt[36:38], "big")
    end = int.from_bytes(pkt[38:40], "big")
    total = int.from_bytes(pkt[40:42], "big")
    one_len = int.from_bytes(pkt[42:44], "big")

    out.append(f"수신 {len(pkt)}byte (Packet Length 필드={packet_length}, V{version})")
    out.append(f"  Command=0x{command:02x} Sub=0x{sub_command:02x} Object=0x{obj:02x}")
    out.append(f"  DataBlock cur={cur} end={end} total={total} oneLen={one_len}")

    if pkt[0] != STX:
        out.append("  ** STX가 아님")
    if pkt[-1] != ETX:
        out.append("  ** ETX로 끝나지 않음")

    frame_option = int.from_bytes(pkt[4:6], "big")
    out.append(f"  FrameOption=0x{frame_option:04x}"
               f"{' (ExcludeDeviceStatus)' if frame_option & FOPT_EXCLUDE_DEVICE_STATUS else ''}")

    body = pkt[HEADER_LEN:len(pkt) - TAIL_LEN]
    if frame_option & FOPT_EXCLUDE_DEVICE_STATUS:
        out.append("  [Device Status] 응답에서 제외됨 (요청대로)")
    elif len(body) >= DEVICE_STATUS_V2_LEN:
        ds = body[:DEVICE_STATUS_V2_LEN]
        dev_type = int.from_bytes(ds[0:2], "big")
        out.append(f"  [Device Status] DeviceType=0x{dev_type:04x} "
                   f"시각=20{bcd(ds[2]):02d}-{bcd(ds[3]):02d}-{bcd(ds[4]):02d} "
                   f"{bcd(ds[5]):02d}:{bcd(ds[6]):02d}:{bcd(ds[7]):02d} "
                   f"ExistedModule=0x{int.from_bytes(ds[8:10], 'big'):04x}")
        out.extend(describe_modules(ds))
        body = body[DEVICE_STATUS_V2_LEN:]
    else:
        out.append(f"  [Device Status] 없음 (body {len(body)}byte)")

    if obj == OBJ_FIRMWARE:
        if len(body) >= FIRMWARE_INFO_LEN:
            fw = body[:FIRMWARE_INFO_LEN]
            version = "".join(f"{b:02d}" for b in fw[2:6])
            out.append(f"  [Firmware] Category={fw[0]} DeviceType=0x{fw[1]:02x} Version={version}")
            out.append(f"             빌드일시=20{bcd(fw[6]):02d}-{bcd(fw[7]):02d}-{bcd(fw[8]):02d} "
                       f"{bcd(fw[9]):02d}:{bcd(fw[10]):02d}:{bcd(fw[11]):02d}")
        else:
            out.append(f"  [Firmware] 데이터 부족 ({len(body)}byte, 268 필요)")
        return out

    if len(body) >= EVENT_INFO_LEN:
        ev = body[:EVENT_INFO_LEN]
        code = int.from_bytes(ev[0:4], "big")
        name = EVENT_NAMES.get(code, "알 수 없는 Event Code")
        out.append(f"  [Event] 0x{code:08x} {name}")
        out.append(f"          OpMode=0x{ev[4]:02x} Module={ev[6]} Reader={ev[7]} "
                   f"DoorStatus={DOOR_STATUS_NAMES.get(ev[8], ev[8])} Func=0x{ev[9]:02x}")
        out.append(f"          시각=20{bcd(ev[10]):02d}-{bcd(ev[11]):02d}-{bcd(ev[12]):02d} "
                   f"{bcd(ev[13]):02d}:{bcd(ev[14]):02d}:{bcd(ev[15]):02d}")
        out.append(f"          AccessID={ev[16:24].hex().upper()}")
    else:
        out.append("  [Event] 없음 (대기 중인 이벤트가 없음)")

    return out


def recv_packet(sock, timeout):
    """헤더의 Packet Length만큼 다 받을 때까지 읽는다. 타임아웃이면 None."""
    sock.settimeout(timeout)
    buf = b""
    try:
        while len(buf) < HEADER_LEN:
            chunk = sock.recv(4096)
            if not chunk:
                return None
            buf += chunk
        want = int.from_bytes(buf[1:3], "big")
        while len(buf) < want:
            chunk = sock.recv(4096)
            if not chunk:
                return None
            buf += chunk
        return buf
    except socket.timeout:
        return buf if buf else None


def main():
    ap = argparse.ArgumentParser(description="ACU IDTi V2 테스트 클라이언트")
    ap.add_argument("--host", default="127.0.0.1", help="ACU 주소 (기본 127.0.0.1)")
    ap.add_argument("--port", type=int, default=9870, help="ACU 포트 (기본 9870)")
    ap.add_argument("--request", choices=["history", "status"], default="history",
                    help="history=이벤트 로그 조회(기본), "
                         "status=장치 상태 요청(RequestStatus/Read/Firmware — PC가 접속 후 보내는 첫 명령)")
    ap.add_argument("--exclude-status", action="store_true",
                    help="IsExcludeDeviceStatus 비트를 켜서 Device Status 없는 응답을 요청")
    ap.add_argument("--watch", action="store_true", help="끊지 않고 계속 폴링")
    ap.add_argument("--interval", type=float, default=2.0, help="--watch 폴링 주기(초, 기본 2)")
    ap.add_argument("--count", type=int, default=1, help="요청 횟수 (--watch면 무시)")
    ap.add_argument("--timeout", type=float, default=3.0, help="응답 대기 시간(초, 기본 3)")
    ap.add_argument("--raw", action="store_true", help="주고받은 바이트를 hex로 함께 출력")
    args = ap.parse_args()

    if args.request == "history":
        command, sub, obj = CMD_REQ_DATA, SUBCMD_READ, OBJ_HISTORY
    else:
        # PC(DM)가 접속 후 장치를 확인할 때 보내는 명령과 동일
        command, sub, obj = CMD_REQ_STATUS, SUBCMD_READ, OBJ_FIRMWARE

    frame_option = FOPT_REQUEST_ACK | FOPT_TCP
    if args.exclude_status:
        frame_option |= FOPT_EXCLUDE_DEVICE_STATUS

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as e:
        print(f"접속 실패 {args.host}:{args.port} - {e}")
        return 1
    print(f"접속됨 {args.host}:{args.port}")

    frame_index = 1
    sent = 0
    try:
        while args.watch or sent < args.count:
            req = build_request(command, sub, obj, frame_index=frame_index,
                                frame_option=frame_option)
            if args.raw:
                print(f"송신 {len(req)}byte: {req.hex()}")
            sock.sendall(req)
            sent += 1
            frame_index += 1

            resp = recv_packet(sock, args.timeout)
            if resp is None:
                print("응답 없음 (타임아웃 또는 연결 끊김)")
                if not args.watch:
                    break
            else:
                if args.raw:
                    print(f"수신 원본: {resp.hex()}")
                for line in parse_response(resp):
                    print(line)

            if args.watch or sent < args.count:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n중단")
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
