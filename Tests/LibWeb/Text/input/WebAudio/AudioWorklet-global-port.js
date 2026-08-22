globalThis.port.onmessage = event => globalThis.port.postMessage(`${event.data} through global scope`);
