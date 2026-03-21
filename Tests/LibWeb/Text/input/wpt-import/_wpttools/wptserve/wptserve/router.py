# mypy: allow-untyped-defs

import itertools
import re
import sys

from .logger import get_logger

any_method = object()

class RouteTokenizer:
    def literal(self, scanner, token):
        return ("literal", token)

    def slash(self, scanner, token):
        return ("slash", None)

    def group(self, scanner, token):
        return ("group", token[1:-1])

    def star(self, scanner, token):
        return ("star", token[1:-3])

    def scan(self, input_str):
        scanner = re.Scanner([(r"/", self.slash),
                              (r"{\w*}", self.group),
                              (r"\*", self.star),
                              (r"(?:\\.|[^{\*/])*", self.literal),])
        return scanner.scan(input_str)

class RouteCompiler:
    def __init__(self):
        self.reset()

    def reset(self):
        self.star_seen = False

    def compile(self, tokens):
        self.reset()

        func_map = {"slash":self.process_slash,
                    "literal":self.process_literal,
                    "group":self.process_group,
                    "star":self.process_star}

        re_parts = ["^"]

        if not tokens or tokens[0][0] != "slash":
            tokens = itertools.chain([("slash", None)], tokens)

        for token in tokens:
            re_parts.append(func_map[token[0]](token))

        if self.star_seen:
            re_parts.append(")")
        re_parts.append("$")

        return re.compile("".join(re_parts))

    def process_literal(self, token):
        return re.escape(token[1])

    def process_slash(self, token):
        return "/"

    def process_group(self, token):
        if self.star_seen:
            raise ValueError("Group seen after star in regexp")
        return "(?P<%s>[^/]+)" % token[1]

    def process_star(self, token):
        if self.star_seen:
            raise ValueError("Star seen after star in regexp")
        self.star_seen = True
        return "(.*"

def compile_path_match(route_pattern):
    """tokens: / or literal or match or *"""

    tokenizer = RouteTokenizer()
    tokens, unmatched = tokenizer.scan(route_pattern)

    assert unmatched == "", unmatched

    compiler = RouteCompiler()

    return compiler.compile(tokens)

class Router:
    """Object for matching handler functions to requests.

    :param doc_root: Absolute path of the filesystem location from
                     which to serve tests
    :param routes: Initial routes to add; a list of three item tuples
                   (method, path_pattern, handler_function), defined
                   as for register()
    """

    def __init__(self, doc_root, routes):
        self.doc_root = doc_root
        self.routes = []
        self.logger = get_logger()

        # Add the doc_root to the Python path, so that any Python handler can
        # correctly locate helper scripts (see RFC_TO_BE_LINKED).
        #
        # TODO: In a perfect world, Router would not need to know about this
        # and the handler itself would take care of it. Currently, however, we
        # treat handlers like functions and so there's no easy way to do that.
        if self.doc_root not in sys.path:
            sys.path.insert(0, self.doc_root)

        for route in reversed(routes):
            self.register(*route)

    def register(self, methods, path, handler):
        r"""Register a handler for a set of paths.

        :param methods: Set of methods this should match. "*" is a
                        special value indicating that all methods should
                        be matched.

        :param path_pattern: Match pattern that will be used to determine if
                             a request path matches this route. Match patterns
                             consist of either literal text, match groups,
                             denoted {name}, which match any character except /,
                             and, at most one \*, which matches and character and
                             creates a match group to the end of the string.
                             If there is no leading "/" on the pattern, this is
                             automatically implied. For example::

                                 api/{resource}/*.json

                            Would match `/api/test/data.json` or
                            `/api/test/test2/data.json`, but not `/api/test/data.py`.

                            The match groups are made available in the request object
                            as a dictionary through the route_match property. For
                            example, given the route pattern above and the path
                            `/api/test/data.json`, the route_match property would
                            contain::

                                {"resource": "test", "*": "data.json"}

        :param handler: Function that will be called to process matching
                        requests. This must take two parameters, the request
                        object and the response object.

        """
        if isinstance(methods, (bytes, str)) or methods is any_method:
            methods = [methods]
        for method in methods:
            self.routes.append((method, compile_path_match(path), handler))
            self.logger.debug("Route pattern: %s" % self.routes[-1][1].pattern)

    def get_handler(self, request):
        """Get a handler for a request or None if there is no handler.

        :param request: Request to get a handler for.
        :rtype: Callable or None
        """
        for method, regexp, handler in reversed(self.routes):
            if (request.method == method or
                method in (any_method, "*") or
                (request.method == "HEAD" and method == "GET")):
                m = regexp.match(request.url_parts.path)
                if m:
                    if not hasattr(handler, "__class__"):
                        name = handler.__name__
                    else:
                        name = handler.__class__.__name__
                    self.logger.debug("Found handler %s" % name)

                    match_parts = m.groupdict().copy()
                    if len(match_parts) < len(m.groups()):
                        match_parts["*"] = m.groups()[-1]
                    request.route_match = match_parts

                    return handler
        return None
