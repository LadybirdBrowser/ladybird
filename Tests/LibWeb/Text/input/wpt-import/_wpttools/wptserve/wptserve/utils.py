import socket
from typing import AnyStr, Dict, List, TypeVar

from .logger import get_logger

KT = TypeVar('KT')
VT = TypeVar('VT')


def isomorphic_decode(s: AnyStr) -> str:
    """Decodes a binary string into a text string using iso-8859-1.

    Returns `str`. The function is a no-op if the argument already has a text
    type. iso-8859-1 is chosen because it is an 8-bit encoding whose code
    points range from 0x0 to 0xFF and the values are the same as the binary
    representations, so any binary string can be decoded into and encoded from
    iso-8859-1 without any errors or data loss. Python 3 also uses iso-8859-1
    (or latin-1) extensively in http:
    https://github.com/python/cpython/blob/273fc220b25933e443c82af6888eb1871d032fb8/Lib/http/client.py#L213
    """
    if isinstance(s, str):
        return s

    if isinstance(s, bytes):
        return s.decode("iso-8859-1")

    raise TypeError("Unexpected value (expecting string-like): %r" % s)


def isomorphic_encode(s: AnyStr) -> bytes:
    """Encodes a text-type string into binary data using iso-8859-1.

    Returns `bytes`. The function is a no-op if the argument already has a
    binary type. This is the counterpart of isomorphic_decode.
    """
    if isinstance(s, bytes):
        return s

    if isinstance(s, str):
        return s.encode("iso-8859-1")

    raise TypeError("Unexpected value (expecting string-like): %r" % s)


def invert_dict(dict: Dict[KT, List[VT]]) -> Dict[VT, KT]:
    rv = {}
    for key, values in dict.items():
        for value in values:
            if value in rv:
                raise ValueError
            rv[value] = key
    return rv


class HTTPException(Exception):
    def __init__(self, code: int, message: str = ""):
        self.code = code
        self.message = message


def _open_socket(host: str, port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if port != 0:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    sock.listen(5)
    return sock


def is_bad_port(port: int) -> bool:
    """
    Bad port as per https://fetch.spec.whatwg.org/#port-blocking
    """
    return port in [
        0,
        1,     # tcpmux
        7,     # echo
        9,     # discard
        11,    # systat
        13,    # daytime
        15,    # netstat
        17,    # qotd
        19,    # chargen
        20,    # ftp-data
        21,    # ftp
        22,    # ssh
        23,    # telnet
        25,    # smtp
        37,    # time
        42,    # name
        43,    # nicname
        53,    # domain
        69,    # tftp
        77,    # priv-rjs
        79,    # finger
        87,    # ttylink
        95,    # supdup
        101,   # hostriame
        102,   # iso-tsap
        103,   # gppitnp
        104,   # acr-nema
        109,   # pop2
        110,   # pop3
        111,   # sunrpc
        113,   # auth
        115,   # sftp
        117,   # uucp-path
        119,   # nntp
        123,   # ntp
        135,   # loc-srv / epmap
        137,   # netbios-ns
        139,   # netbios-ssn
        143,   # imap2
        161,   # snmp
        179,   # bgp
        389,   # ldap
        427,   # afp (alternate)
        465,   # smtp (alternate)
        512,   # print / exec
        513,   # login
        514,   # shell
        515,   # printer
        526,   # tempo
        530,   # courier
        531,   # chat
        532,   # netnews
        540,   # uucp
        548,   # afp
        554,   # rtsp
        556,   # remotefs
        563,   # nntp+ssl
        587,   # smtp (outgoing)
        601,   # syslog-conn
        636,   # ldap+ssl
        989,   # ftps-data
        999,   # ftps
        993,   # ldap+ssl
        995,   # pop3+ssl
        1719,  # h323gatestat
        1720,  # h323hostcall
        1723,  # pptp
        2049,  # nfs
        3659,  # apple-sasl
        4045,  # lockd
        4190,  # sieve
        5060,  # sip
        5061,  # sips
        6000,  # x11
        6566,  # sane-port
        6665,  # irc (alternate)
        6666,  # irc (alternate)
        6667,  # irc (default)
        6668,  # irc (alternate)
        6669,  # irc (alternate)
        6679,  # osaut
        6697,  # irc+tls
        10080,  # amanda
    ]


def get_port(host: str = '') -> int:
    host = host or '127.0.0.1'
    port = 0
    while True:
        free_socket = _open_socket(host, 0)
        port = free_socket.getsockname()[1]
        free_socket.close()
        if not is_bad_port(port):
            break
    return port

def http2_compatible() -> bool:
    # The HTTP/2 server requires OpenSSL 1.0.2+.
    #
    # For systems using other SSL libraries (e.g. LibreSSL), we assume they
    # have the necessary support.
    import ssl
    if not ssl.OPENSSL_VERSION.startswith("OpenSSL"):
        logger = get_logger()
        logger.warning(
            'Skipping HTTP/2 compatibility check as system is not using '
            'OpenSSL (found: %s)' % ssl.OPENSSL_VERSION)
        return True

    # Note that OpenSSL's versioning scheme differs between 1.1.1 and
    # earlier and 3.0.0. ssl.OPENSSL_VERSION_INFO returns a
    #     (major, minor, 0, patch, 0)
    # tuple with OpenSSL 3.0.0 and later, and a
    #     (major, minor, fix, patch, status)
    # tuple for older releases.
    # Semantically, "patch" in 3.0.0+ is similar to "fix" in previous versions.
    #
    # What we do in the check below is allow OpenSSL 3.x.y+, 1.1.x+ and 1.0.2+.
    ssl_v = ssl.OPENSSL_VERSION_INFO
    return (ssl_v[0] > 1 or
            (ssl_v[0] == 1 and
             (ssl_v[1] == 1 or
              (ssl_v[1] == 0 and ssl_v[2] >= 2))))


def get_error_cause(exc: BaseException) -> BaseException:
    """Get the parent cause/context from an exception"""
    if exc.__cause__ is not None:
        return exc.__cause__
    if exc.__context__ is not None:
        return exc.__context__
    return exc
