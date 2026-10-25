import os

from wptserve.utils import isomorphic_decode

def get_template(template_basename):
  script_directory = os.path.dirname(os.path.abspath(isomorphic_decode(__file__)))
  template_directory = os.path.abspath(
      os.path.join(script_directory, u"template"))
  template_filename = os.path.join(template_directory, template_basename)

  with open(template_filename, "r") as f:
    return f.read()


def __noop(request, response):
  return u""


def respond(request,
            response,
            status_code=200,
            content_type=b"text/html",
            payload_generator=__noop,
            cache_control=b"no-cache; must-revalidate",
            access_control_allow_origin=b"*",
            maybe_additional_headers=None):
  response.add_required_headers = False
  response.writer.write_status(status_code)

  if access_control_allow_origin != None:
    response.writer.write_header(b"access-control-allow-origin",
                                 access_control_allow_origin)
  response.writer.write_header(b"content-type", content_type)
  response.writer.write_header(b"cache-control", cache_control)

  additional_headers = maybe_additional_headers or {}
  for header, value in additional_headers.items():
    response.writer.write_header(header, value)

  response.writer.end_headers()

  payload = payload_generator()
  response.writer.write(payload)
