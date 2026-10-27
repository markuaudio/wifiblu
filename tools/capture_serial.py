"""Capture a short, real serial log without holding the COM port open forever."""
import argparse
import time
from pathlib import Path
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM6')
parser.add_argument('--seconds', type=float, default=10)
parser.add_argument('--output', default='docs/serial-output.log')
parser.add_argument('--reset', action='store_true', help='Pulse RTS reset (requires auto-download jumpers)')
parser.add_argument('--command', choices=['l', 'r', 'z', 'b', 'd', '+', '-', '0'],
                    help='Speaker diagnostic or non-persistent power-profile command')
args = parser.parse_args()

connection = serial.Serial(port=None, baudrate=115200, timeout=0.5)
connection.dtr = False
connection.rts = False
connection.port = args.port
chunks = []
try:
    connection.open()
    if args.reset:
        connection.rts = True
        time.sleep(0.1)
        connection.rts = False
    if args.command:
        connection.write((args.command + '\n').encode('ascii'))
        connection.flush()
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        chunk = connection.read(connection.in_waiting or 1)
        if chunk:
            chunks.append(chunk)
finally:
    connection.close()

output = b''.join(chunks).decode('utf-8', errors='replace')
Path(args.output).write_text(output, encoding='utf-8')
print(output if output else 'No serial bytes received during capture.')
