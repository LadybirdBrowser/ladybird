#!/usr/bin/env python3
import socket
import json

# Clean content - should not trigger any alerts
CLEAN_TEXT = "Hello, this is a clean text file with no malicious content."

# Connect to Sentinel
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect('/tmp/sentinel.sock')

# Send scan_content request
request = {
    "action": "scan_content",
    "request_id": "test_clean_001",
    "content": CLEAN_TEXT
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
