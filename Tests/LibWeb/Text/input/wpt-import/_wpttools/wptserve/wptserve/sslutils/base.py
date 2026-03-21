# mypy: allow-untyped-defs

class NoSSLEnvironment:
    ssl_enabled = False

    def __init__(self, *args, **kwargs):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *args, **kwargs):
        pass

    def host_cert_path(self, hosts):
        return None, None

    def ca_cert_path(self, hosts):
        return None
