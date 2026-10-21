"""Serial gateway: reads the JSON-line stream of a board and republishes it.

    dbrain gateway --port /dev/ttyUSB0 --baud 115200 --log run.jsonl --ws 8765

Lines that start with '{' are events; everything else is console text and
is echoed to stderr. With --ws a WebSocket server feeds host/dashboard/.
"""

import asyncio
import sys
import threading
import time
from typing import Iterator, Optional


def read_lines(port: str, baud: int = 115200) -> Iterator[str]:
    try:
        import serial
    except ImportError as e:
        raise SystemExit("pyserial is required: pip install pyserial") from e
    with serial.Serial(port, baud, timeout=1) as s:
        while True:
            raw = s.readline()
            if not raw:
                continue
            yield raw.decode("utf-8", "replace").rstrip("\r\n")


def send_command(port: str, baud: int, command: str) -> None:
    import serial
    with serial.Serial(port, baud, timeout=1) as s:
        s.write((command.strip() + "\n").encode())


class Hub:
    """Fans lines out to WebSocket clients from a reader thread."""

    def __init__(self):
        self.clients = set()
        self.loop: Optional[asyncio.AbstractEventLoop] = None

    async def handler(self, ws):
        self.clients.add(ws)
        try:
            async for _ in ws:
                pass
        finally:
            self.clients.discard(ws)

    def publish(self, line: str) -> None:
        if self.loop is None:
            return
        for ws in list(self.clients):
            asyncio.run_coroutine_threadsafe(ws.send(line), self.loop)

    async def serve(self, host: str, port: int):
        try:
            import websockets
        except ImportError as e:
            raise SystemExit("websockets is required: pip install websockets") from e
        self.loop = asyncio.get_running_loop()
        async with websockets.serve(self.handler, host, port):
            await asyncio.Future()


def run(port: str, baud: int, log: Optional[str], ws_port: Optional[int], quiet: bool = False) -> None:
    hub = Hub() if ws_port else None
    logf = open(log, "a") if log else None

    def reader():
        for line in read_lines(port, baud):
            if line.startswith("{"):
                if logf:
                    logf.write(line + "\n")
                    logf.flush()
                if hub:
                    hub.publish(line)
                if not quiet:
                    print(line)
            else:
                print(line, file=sys.stderr)

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    if hub:
        asyncio.run(hub.serve("127.0.0.1", ws_port))
    else:
        while t.is_alive():
            time.sleep(0.5)
