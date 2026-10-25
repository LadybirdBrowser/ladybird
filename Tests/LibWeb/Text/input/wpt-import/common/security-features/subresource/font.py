import os, sys
from base64 import decodebytes

from wptserve.utils import isomorphic_decode
import importlib
subresource = importlib.import_module("common.security-features.subresource.subresource")


def generate_payload(request, server_data):
    data = (u'{"headers": %(headers)s}') % server_data
    if b"id" in request.GET:
        request.server.stash.put(request.GET[b"id"], data)
    # Simple base64 encoded .tff font
    return decodebytes(b"AAEAAAANAIAAAwBQRkZUTU6u6MkAAAXcAAAAHE9TLzJWYW"
                       b"QKAAABWAAAAFZjbWFwAA8D7wAAAcAAAAFCY3Z0IAAhAnkA"
                       b"AAMEAAAABGdhc3D//wADAAAF1AAAAAhnbHlmCC6aTwAAAx"
                       b"QAAACMaGVhZO8ooBcAAADcAAAANmhoZWEIkAV9AAABFAAA"
                       b"ACRobXR4EZQAhQAAAbAAAAAQbG9jYQBwAFQAAAMIAAAACm"
                       b"1heHAASQA9AAABOAAAACBuYW1lehAVOgAAA6AAAAIHcG9z"
                       b"dP+uADUAAAWoAAAAKgABAAAAAQAAMhPyuV8PPPUACwPoAA"
                       b"AAAMU4Lm0AAAAAxTgubQAh/5wFeAK8AAAACAACAAAAAAAA"
                       b"AAEAAAK8/5wAWgXcAAAAAAV4AAEAAAAAAAAAAAAAAAAAAA"
                       b"AEAAEAAAAEAAwAAwAAAAAAAgAAAAEAAQAAAEAALgAAAAAA"
                       b"AQXcAfQABQAAAooCvAAAAIwCigK8AAAB4AAxAQIAAAIABg"
                       b"kAAAAAAAAAAAABAAAAAAAAAAAAAAAAUGZFZABAAEEAQQMg"
                       b"/zgAWgK8AGQAAAABAAAAAAAABdwAIQAAAAAF3AAABdwAZA"
                       b"AAAAMAAAADAAAAHAABAAAAAAA8AAMAAQAAABwABAAgAAAA"
                       b"BAAEAAEAAABB//8AAABB////wgABAAAAAAAAAQYAAAEAAA"
                       b"AAAAAAAQIAAAACAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAA"
                       b"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAwAAAAAAAA"
                       b"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                       b"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                       b"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                       b"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                       b"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                       b"AAAAAAAAAAAAAAAAAAAhAnkAAAAqACoAKgBGAAAAAgAhAA"
                       b"ABKgKaAAMABwAusQEALzyyBwQA7TKxBgXcPLIDAgDtMgCx"
                       b"AwAvPLIFBADtMrIHBgH8PLIBAgDtMjMRIREnMxEjIQEJ6M"
                       b"fHApr9ZiECWAAAAwBk/5wFeAK8AAMABwALAAABNSEVATUh"
                       b"FQE1IRUB9AH0/UQDhPu0BRQB9MjI/tTIyP7UyMgAAAAAAA"
                       b"4ArgABAAAAAAAAACYATgABAAAAAAABAAUAgQABAAAAAAAC"
                       b"AAYAlQABAAAAAAADACEA4AABAAAAAAAEAAUBDgABAAAAAA"
                       b"AFABABNgABAAAAAAAGAAUBUwADAAEECQAAAEwAAAADAAEE"
                       b"CQABAAoAdQADAAEECQACAAwAhwADAAEECQADAEIAnAADAA"
                       b"EECQAEAAoBAgADAAEECQAFACABFAADAAEECQAGAAoBRwBD"
                       b"AG8AcAB5AHIAaQBnAGgAdAAgACgAYwApACAAMgAwADAAOA"
                       b"AgAE0AbwB6AGkAbABsAGEAIABDAG8AcgBwAG8AcgBhAHQA"
                       b"aQBvAG4AAENvcHlyaWdodCAoYykgMjAwOCBNb3ppbGxhIE"
                       b"NvcnBvcmF0aW9uAABNAGEAcgBrAEEAAE1hcmtBAABNAGUA"
                       b"ZABpAHUAbQAATWVkaXVtAABGAG8AbgB0AEYAbwByAGcAZQ"
                       b"AgADIALgAwACAAOgAgAE0AYQByAGsAQQAgADoAIAA1AC0A"
                       b"MQAxAC0AMgAwADAAOAAARm9udEZvcmdlIDIuMCA6IE1hcm"
                       b"tBIDogNS0xMS0yMDA4AABNAGEAcgBrAEEAAE1hcmtBAABW"
                       b"AGUAcgBzAGkAbwBuACAAMAAwADEALgAwADAAMAAgAABWZX"
                       b"JzaW9uIDAwMS4wMDAgAABNAGEAcgBrAEEAAE1hcmtBAAAA"
                       b"AgAAAAAAAP+DADIAAAABAAAAAAAAAAAAAAAAAAAAAAAEAA"
                       b"AAAQACACQAAAAAAAH//wACAAAAAQAAAADEPovuAAAAAMU4"
                       b"Lm0AAAAAxTgubQ==")

def generate_report_headers_payload(request, server_data):
    stashed_data = request.server.stash.take(request.GET[b"id"])
    return stashed_data

def main(request, response):
    handler = lambda data: generate_payload(request, data)
    content_type = b'application/x-font-truetype'

    if b"report-headers" in request.GET:
        handler = lambda data: generate_report_headers_payload(request, data)
        content_type = b'application/json'

    subresource.respond(request,
                        response,
                        payload_generator = handler,
                        content_type = content_type,
                        access_control_allow_origin = b"*")
