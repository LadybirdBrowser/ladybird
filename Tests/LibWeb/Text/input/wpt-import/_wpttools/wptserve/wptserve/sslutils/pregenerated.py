# mypy: allow-untyped-defs

class PregeneratedSSLEnvironment:
    """SSL environment to use with existing key/certificate files
    e.g. when running on a server with a public domain name
    """
    ssl_enabled = True

    def __init__(self, logger, host_key_path, host_cert_path,
                 ca_cert_path=None):
        self._ca_cert_path = ca_cert_path
        self._host_key_path = host_key_path
        self._host_cert_path = host_cert_path

    def __enter__(self):
        return self

    def __exit__(self, *args, **kwargs):
        pass

    def host_cert_path(self, hosts):
        """Return the key and certificate paths for the host"""
        return self._host_key_path, self._host_cert_path

    def ca_cert_path(self, hosts):
        """Return the certificate path of the CA that signed the
        host certificates, or None if that isn't known"""
        return self._ca_cert_path
