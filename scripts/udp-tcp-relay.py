#!/usr/bin/env python3
"""UDP → 长度前缀 TCP 中继（给"下行 UDP 必须穿过 SSH 隧道"这种拓扑用）。

为什么这个脚本值得存在：SSH 只能转发 TCP，而 cue 的下行是 UDP。要在两台机器之间
做下行验证，就必须有一个 UDP↔TCP 的转接。这个转接怎么排队，直接决定接收端看起来
是否流畅——第一版把它写成"阻塞 TCP + 16 MB 接收缓冲"，结果是：

  · 3 Mbps 的流配 16 MB 接收缓冲 = 42 秒的潜在队列。SSH 通道稍一慢，内核把这几十秒
    的包全存下来，等 TCP 通了再一次性灌过去；
  · 接收端（cue-probe）看到的是"第 21 秒 8 帧、第 22 秒 40 帧"这种节奏，
    帧间隔最大 1.6 秒，11% 的帧因为"迟到"被抖动缓冲丢掉；
  · 而 cued 侧的上行统计一直是稳定的 30 帧/秒——问题完全出在这个转接的排队上。

所以这里的规矩是：**实时媒体在拥塞时应该丢弃，不应该排队**。
  · UDP 读取永不阻塞（非阻塞 + 循环读到 EAGAIN），不会被 TCP 反压拖住；
  · TCP 写入用 select 等可写，应用层积压超过上限就丢新包（丢旧包会破坏顺序，
    而 probe 有 NACK/FEC 能补，丢最新的代价最小）；
  · TCP_NODELAY 关掉 Nagle，否则小包会被攒到 40 ms 才发。

用法：udp-tcp-relay.py <udp_port> <tcp_host> <tcp_port> [--max-backlog-kb N]
统计每 5 秒打一行：转发的包数、丢掉的包数、应用层积压峰值。
"""
import argparse
import errno
import select
import socket
import struct
import sys
import time

MAX_DATAGRAM = 65535


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("udp_port", type=int)
    ap.add_argument("tcp_host")
    ap.add_argument("tcp_port", type=int)
    ap.add_argument("--max-backlog-kb", type=int, default=96,
                    help="应用层允许积压的字节上限（默认 96 KB）。"
                         "3 Mbps 下 96 KB ≈ 0.26 s，超过就丢新包。")
    ap.add_argument("--rcvbuf-kb", type=int, default=512,
                    help="内核 UDP 接收缓冲（默认 512 KB）。故意不设大："
                         "设成 16 MB 等于给链路加了几十秒的排队延迟。")
    args = ap.parse_args()

    max_backlog = max(1, args.max_backlog_kb) * 1024

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, max(1, args.rcvbuf_kb) * 1024)
    udp.bind(("127.0.0.1", args.udp_port))
    udp.setblocking(False)
    print("udp listen %d (rcvbuf=%d B) -> tcp %s:%d (max_backlog=%d B)"
          % (args.udp_port, udp.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF),
             args.tcp_host, args.tcp_port, max_backlog), flush=True)

    tcp = None
    backlog = bytearray()      # 待发送的 [4B 长度][载荷]
    sent = 0
    dropped = 0
    peak_backlog = 0
    last_report = time.monotonic()

    def connect():
        nonlocal tcp
        try:
            s = socket.create_connection((args.tcp_host, args.tcp_port), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256 * 1024)
            s.setblocking(False)
            print("tcp connected", flush=True)
            return s
        except OSError as e:
            print("tcp connect fail: %s" % e, flush=True)
            return None

    # 关键：**先连上 TCP，再开始收 UDP**。
    # 边连边收的话，连接就绪前收到的数据报只能丢掉——而丢掉的很可能正是开头的
    # CodecConfig（SPS/PPS）。实测后果不是"掉几帧"，是接收端永久解不出来：
    # probe 每一帧都报 send_packet failed: -1094995529 (AVERROR_INVALIDDATA)，
    # 画面停在最后一帧，而 frames/decoded 之外的所有指标看起来都正常。
    while tcp is None:
        tcp = connect()
        if tcp is None:
            time.sleep(0.5)

    while True:
        readfds = [udp]
        writefds = []
        if tcp is not None:
            writefds.append(tcp)
        else:
            # 连接掉了（对端重启/隧道抖动）：重连期间**不读 UDP**，让内核缓冲先兜住，
            # 而不是把数据读出来再丢掉——丢掉 CodecConfig 会让接收端永久解不出画面。
            readfds = []
            tcp = connect()
            if tcp is None:
                time.sleep(0.5)

        try:
            r, w, _ = select.select(readfds, writefds, [], 0.2)
        except OSError as e:
            if e.errno == errno.EINTR:
                continue
            raise

        # ---- 1) 尽力读空 UDP：读到 EAGAIN 为止，绝不被 TCP 反压卡住 ----
        if udp in r:
            for _ in range(256):  # 一次性最多处理这么多，避免饿死写入
                try:
                    data = udp.recv(MAX_DATAGRAM)
                except BlockingIOError:
                    break
                except OSError:
                    break
                if len(backlog) + 4 + len(data) > max_backlog:
                    # 积压已超上限：丢**新**包。丢旧包会打乱顺序，而 probe 有 NACK/FEC
                    # 可以补丢掉的包；把最新的帧挤出去反而更糟。
                    dropped += 1
                    continue
                backlog += struct.pack(">I", len(data))
                backlog += data

        # ---- 2) TCP 可写就把积压尽量推出去 ----
        if tcp is not None and (w or backlog):
            while backlog:
                try:
                    n = tcp.send(backlog)
                except BlockingIOError:
                    break
                except OSError as e:
                    print("tcp error: %s (sent %d, dropped %d)" % (e, sent, dropped), flush=True)
                    try:
                        tcp.close()
                    except OSError:
                        pass
                    tcp = None
                    backlog.clear()
                    break
                if n <= 0:
                    break
                del backlog[:n]
                sent += 1
            if len(backlog) > peak_backlog:
                peak_backlog = len(backlog)

        now = time.monotonic()
        if now - last_report >= 5.0:
            last_report = now
            print("relayed %d chunks, dropped %d, backlog %d B (peak %d B)"
                  % (sent, dropped, len(backlog), peak_backlog), flush=True)
            peak_backlog = len(backlog)


if __name__ == "__main__":
    sys.exit(main())
