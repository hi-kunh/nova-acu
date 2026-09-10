#!/usr/bin/env python3
"""
netmodule UDP 탐색 클라이언트 (PC 역할).

기존 IntelliScan Device Manager가 하는 것과 같은 일을 한다:
  - 255.255.255.255:1460 으로 "FIND"(4byte)를 브로드캐스트
  - 자기 5001 포트에서 IMIN(50byte) 응답을 받아 해석

프레임 근거는 PC 소스 islnetmodule/clsnmSettingFrame.cs
(README "netmodule UDP 탐색/설정 프로토콜" 참고).

  python3 tools/nm_discover.py                # 2초 동안 탐색
  python3 tools/nm_discover.py --timeout 5
  python3 tools/nm_discover.py --raw          # 원본 바이트도 출력

설정 변경(SETT)은 대상 장치의 MAC이 필요하다. FIND로 먼저 찾은 뒤 보낸다:

  python3 tools/nm_discover.py --set 192.168.0.200/24 --gw 192.168.0.1
  python3 tools/nm_discover.py --set ... --password 1234
"""

import argparse
import socket
import struct
import sys
import time

PORT_BROADCAST = 1460  # PC -> 장치
PORT_LISTEN = 5001     # 장치 -> PC

FIND_ACK_LEN = 50
SET_ACK_LEN = 58

TCP_MODE = {0: "Client", 1: "Mixed", 2: "Server"}

# SerialBPS는 비선형 코드다 (clsnmParams.SerialBaudrate)
BPS = {
    160: 1200, 208: 2400, 232: 4800, 244: 9600, 250: 19200,
    253: 38400, 254: 57600, 255: 115200, 187: 230400,
}


def ip(b):
    return ".".join(str(x) for x in b)


def mac(b):
    return ":".join(f"{x:02X}" for x in b)


def parse_ack(data):
    """IMIN(50) 또는 SETC/FAIL(58)을 해석해 dict로 돌려준다."""
    if len(data) < FIND_ACK_LEN:
        return None

    o = 0

    def take(n):
        nonlocal o
        v = data[o:o + n]
        o += n
        return v

    out = {}
    out["command"] = take(4).decode("ascii", "replace")
    out["mac"] = mac(take(6))
    out["tcp_mode"] = take(1)[0]
    out["ip"] = ip(take(4))
    out["netmask"] = ip(take(4))
    out["gateway"] = ip(take(4))
    out["port"] = struct.unpack(">H", take(2))[0]
    out["peer_ip"] = ip(take(4))
    out["peer_port"] = struct.unpack(">H", take(2))[0]
    out["serial_bps"] = take(1)[0]
    out["databit"] = take(1)[0]
    out["parity"] = take(1)[0]
    out["stopbit"] = take(1)[0]
    out["flow"] = take(1)[0]
    out["dp_char"] = take(1)[0]
    out["dp_size"] = struct.unpack(">H", take(2))[0]
    out["dp_time"] = struct.unpack(">H", take(2))[0]
    out["inactivity"] = struct.unpack(">H", take(2))[0]
    out["debug"] = take(1)[0]
    fw = take(2)
    out["firmware"] = f"{fw[0]}.{fw[1]}"
    out["dhcp"] = take(1)[0]
    out["udp"] = take(1)[0]
    out["connect"] = take(1)[0]
    out["pw_set"] = take(1)[0]
    return out


# 설정 프레임 필드 오프셋 (discover.c의 표와 같아야 한다)
OFF_COMMAND, OFF_MAC, OFF_TCP_MODE = 0, 4, 10
OFF_IP, OFF_NETMASK, OFF_GATEWAY, OFF_PORT = 11, 15, 19, 23
OFF_PW_SET_FLAG, OFF_PW_COMPANY, OFF_PW_CUSTOM = 49, 50, 54

FRAME_LEN = 50
FRAME_PW_LEN = 58


def prefix_to_mask(prefix):
    """prefix 길이를 4byte 넷마스크로 바꾼다."""
    bits = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF
    return bytes((bits >> s) & 0xFF for s in (24, 16, 8, 0))


def build_sett(mac_text, ip_text, prefix, gw_text, port, password):
    """SETT 요청 58byte를 만든다. 대상 장치는 MAC으로 지목한다."""
    f = bytearray(FRAME_PW_LEN)
    f[OFF_COMMAND:OFF_COMMAND + 4] = b"SETT"
    f[OFF_MAC:OFF_MAC + 6] = bytes(int(x, 16) for x in mac_text.split(":"))
    f[OFF_TCP_MODE] = 2  # Server
    f[OFF_IP:OFF_IP + 4] = bytes(int(x) for x in ip_text.split("."))
    f[OFF_NETMASK:OFF_NETMASK + 4] = prefix_to_mask(prefix)
    f[OFF_GATEWAY:OFF_GATEWAY + 4] = bytes(int(x) for x in gw_text.split("."))
    f[OFF_PORT:OFF_PORT + 2] = struct.pack(">H", port)
    f[OFF_PW_SET_FLAG] = 1
    f[OFF_PW_COMPANY:OFF_PW_COMPANY + 4] = b"IDTi"
    f[OFF_PW_CUSTOM:OFF_PW_CUSTOM + 4] = password.encode("ascii")[:4].ljust(4, b"\0")
    return bytes(f)


def show(info, addr, raw=None):
    print(f"[{info['command']}] {addr[0]}:{addr[1]} 에서 응답")
    print(f"  MAC         {info['mac']}")
    print(f"  IP          {info['ip']} / {info['netmask']}")
    print(f"  게이트웨이   {info['gateway']}")
    mode = TCP_MODE.get(info["tcp_mode"], f"?({info['tcp_mode']})")
    print(f"  TCP         {mode}, 포트 {info['port']}")
    if info["peer_ip"] != "0.0.0.0":
        print(f"  Peer        {info['peer_ip']}:{info['peer_port']}")
    print(f"  펌웨어       {info['firmware']}")
    bps = BPS.get(info["serial_bps"], f"코드 {info['serial_bps']}")
    print(f"  (시리얼)     {bps}bps {info['databit']}-{info['parity']}-{info['stopbit']}"
          "   <- 시리얼-이더넷 모듈 시절 잔재. 우리 장비에선 의미 없음")
    print(f"  유휴 타임아웃  {info['inactivity']}초" +
          ("  (0=끔)" if info["inactivity"] == 0 else ""))
    print(f"  DHCP={info['dhcp']} UDP={info['udp']} Connect={info['connect']} PwSet={info['pw_set']}")
    if raw is not None:
        print(f"  raw({len(raw)}) {raw.hex()}")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=float, default=2.0, help="응답 수집 시간(초). 기본 2")
    ap.add_argument("--target", default="255.255.255.255",
                    help="브로드캐스트 주소. 기본 255.255.255.255")
    ap.add_argument("--raw", action="store_true", help="원본 바이트도 출력")
    ap.add_argument("--set", metavar="IP/PREFIX",
                    help="설정 변경(SETT). 예: 192.168.0.200/24")
    ap.add_argument("--gw", help="--set 과 함께 쓸 게이트웨이")
    ap.add_argument("--port", type=int, default=1004, help="장치 TCP 수신 포트 (기본 1004)")
    ap.add_argument("--password", default="0000", help="단말기 비밀번호 4자리 (기본 0000)")
    ap.add_argument("--mac", help="대상 MAC. 생략하면 FIND로 찾은 장치를 쓴다")
    args = ap.parse_args()

    if args.set and not args.gw:
        ap.error("--set 을 쓰려면 --gw 도 필요하다")

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        rx.bind(("0.0.0.0", PORT_LISTEN))
    except OSError as e:
        print(f"UDP {PORT_LISTEN} 바인드 실패: {e}", file=sys.stderr)
        return 1

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    print(f"FIND 브로드캐스트 -> {args.target}:{PORT_BROADCAST} ({args.timeout}초 대기)")
    tx.sendto(b"FIND", (args.target, PORT_BROADCAST))

    rx.settimeout(0.3)
    deadline = time.time() + args.timeout
    # 브로드캐스트가 스위치/AP를 거치며 여러 번 도달할 수 있어 같은 장치가 여러 번 답한다.
    # MAC 기준으로 한 대만 보여 준다 (Device Manager도 장치 목록을 MAC으로 관리한다).
    devices = {}
    dupes = 0
    while time.time() < deadline:
        try:
            data, addr = rx.recvfrom(512)
        except socket.timeout:
            continue
        info = parse_ack(data)
        if info is None:
            print(f"짧은 응답 {len(data)}byte 무시 ({addr[0]})")
            continue
        if info["mac"] in devices:
            dupes += 1
            continue
        devices[info["mac"]] = True
        show(info, addr, data if args.raw else None)

    print(f"장치 {len(devices)}대 응답" + (f" (중복 응답 {dupes}건 무시)" if dupes else ""))

    if not args.set:
        return 0 if devices else 2

    # ---- 설정 변경 ----
    target = args.mac or (next(iter(devices)) if devices else None)
    if not target:
        print("설정을 보낼 장치를 찾지 못했다", file=sys.stderr)
        return 2

    ip_text, _, prefix_text = args.set.partition("/")
    prefix = int(prefix_text or 24)

    frame = build_sett(target, ip_text, prefix, args.gw, args.port, args.password)
    print(f"\nSETT -> {target} : {ip_text}/{prefix} gw={args.gw} ({len(frame)}byte)")
    tx.sendto(frame, (args.target, PORT_BROADCAST))

    deadline = time.time() + args.timeout
    while time.time() < deadline:
        try:
            data, addr = rx.recvfrom(512)
        except socket.timeout:
            continue
        info = parse_ack(data)
        if info is None or info["command"] not in ("SETC", "FAIL"):
            continue
        verdict = "수락(SETC)" if info["command"] == "SETC" else "거절(FAIL)"
        print(f"응답: {verdict} — {len(data)}byte")
        return 0 if info["command"] == "SETC" else 3

    print("SETT 응답 없음", file=sys.stderr)
    return 4


if __name__ == "__main__":
    sys.exit(main())
