#!/usr/bin/env python3
# PS3 UDP debug-log sink. The engine (--enable-ps3-udp-log) sends plain-text
# lines to <dev-ip>:18194. Appends everything received to the capture file.
import socket, sys, datetime

PORT = 18194
CAP = sys.argv[1] if len(sys.argv) > 1 else "udp_capture.log"

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", PORT))

with open(CAP, "a", buffering=1, encoding="utf-8", errors="replace") as f:
    f.write("\n===== listener started %s =====\n" % datetime.datetime.now().isoformat(timespec="seconds"))
    print("listening on udp/%d -> %s" % (PORT, CAP), flush=True)
    while True:
        try:
            data, addr = s.recvfrom(65535)
        except KeyboardInterrupt:
            break
        text = data.decode("utf-8", errors="replace")
        if not text.endswith("\n"):
            text += "\n"
        f.write(text)
        sys.stdout.write(text)
        sys.stdout.flush()
