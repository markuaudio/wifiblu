"""Live COM6 monitor: print board logs and keep a timestamped session log."""
import argparse
from datetime import datetime
from pathlib import Path
import sys
import threading
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM6')
parser.add_argument('--output', default='docs/live-serial.log')
args = parser.parse_args()
Path(args.output).parent.mkdir(parents=True, exist_ok=True)
connection = serial.Serial(port=None, baudrate=115200, timeout=0.3)
connection.dtr = False
connection.rts = False
connection.port = args.port
print(f'LIVE ESP32 LOGS | {args.port} | 115200 baud | Ctrl+C to stop', flush=True)
print('Press PLAY / SET / VOL+ / VOL- on the board. Their events appear below.', flush=True)
print('Commands: a answer, h hangup, m redial, c connect, x disconnect, v bonds, f forget, i status, t ring test, l r z b tones, ] [ 0 DAC', flush=True)
print(f'Saving session to: {Path(args.output).resolve()}', flush=True)


def forward_keyboard():
    for line in sys.stdin:
        command = line.strip()
        if command and connection.is_open:
            connection.write((command[0] + '\n').encode('ascii', errors='ignore'))
            connection.flush()


try:
    connection.open()
    threading.Thread(target=forward_keyboard, daemon=True).start()
    with open(args.output, 'a', encoding='utf-8', buffering=1) as output:
        output.write(f'\n--- Monitor opened {datetime.now().isoformat(timespec="seconds")} ---\n')
        pending = b''
        while True:
            chunk = connection.read(connection.in_waiting or 1)
            if not chunk:
                continue
            pending += chunk
            while b'\n' in pending:
                line, pending = pending.split(b'\n', 1)
                line = line.decode('utf-8', errors='replace').strip('\r')
                if line:
                    print(line, flush=True)
                    output.write(line + '\n')
except KeyboardInterrupt:
    print('\nMonitor stopped; COM port released.', flush=True)
except serial.SerialException as error:
    print(f'Serial error: {error}', flush=True)
    raise SystemExit(1)
finally:
    connection.close()
