import os
from wptserve.utils import isomorphic_decode

def main(request, response):
  """Serves the contents in blue.png but with a Cache-Control header.

  Emits a Cache-Control header with max-age set to 1h to allow the browser
  cache the image. Used for testing behaviors involving caching logics.
  """
  image_path = os.path.join(os.path.dirname(isomorphic_decode(__file__)), u"blue.png")
  response.headers.set(b"Cache-Control", b"max-age=3600")
  response.headers.set(b"Content-Type", b"image/png")
  response.content = open(image_path, mode='rb').read()
