#!/usr/bin/env python3
import sys, time, datetime, serial

# Command: python3 sync_time_to_server.py <PORT of ESP32>
# On macOS:  /usr/bin/python3 sync_time_to_server.py /dev/tty.usbserial-xxxx
# on Windows: python sync_time_to_server.py COMxxxx

# Hint: If serial monitor is started port is blocked and cannot sync time

if len(sys.argv) < 2:
    print("Usage: python3 sync_time_to_server.py <PORT>")
    sys.exit(1)

port = sys.argv[1] 

ser = serial.Serial(port, 115200, timeout=2)
time.sleep(1.0) 

def send_hosttime():
    now = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
    line = f"HOSTTIME {now}\n".encode()
    ser.write(line)
    print("Sent:", line.decode().strip())

start = time.time()
buf = b""
while time.time() - start < 5.0:
    chunk = ser.read(256)
    if chunk:
        buf += chunk
        text = buf.decode(errors="ignore")
        if "TIME?" in text:
            send_hosttime()
            break

if "TIME?" not in buf.decode(errors="ignore"): 
    send_hosttime()

# Wait for server module response
time.sleep(0.5)
resp = ser.read(512)
if resp:
    try:
        print(resp.decode(errors="ignore"))
    except:
        pass
ser.close()