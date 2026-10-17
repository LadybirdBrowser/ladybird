body = b'''
'use strict';

onconnect = (event) => {
  const port = event.ports[0];

  port.onmessage = (event) => {
    fetch(event.data, { mode: 'no-cors' })
      .then(
        () => port.postMessage('success'),
        () => port.postMessage('failure')
      );
  };

  port.postMessage('ready');
};'''

def main(request, response):
    headers = [(b'Content-Type', b'text/javascript')]

    for value in request.GET.get_list(b'value'):
        headers.append((b'Cross-Origin-Embedder-Policy', value))

    return (200, headers, body)
