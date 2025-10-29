#!/usr/bin/env python3
import socket
import json

# EICAR test file content
EICAR = 'X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*'

# Connect to Sentinel
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect('/tmp/sentinel.sock')

# Send scan_content request
request = {
    "action": "scan_content",
    "request_id": "test_eicar_001",
    "content": EICAR
}

message = json.dumps(request) + '\n'
print(f"Sending request: {message}")
sock.sendall(message.encode())

# Receive response
response_data = sock.recv(4096)
print(f"Received response: {response_data.decode()}")

response = json.loads(response_data.decode())
print(f"\nParsed response:")
print(json.dumps(response, indent=2))

sock.close()
