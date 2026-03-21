# mypy: allow-untyped-defs

import copy
import os
from collections import defaultdict
from typing import Any, Mapping

from . import sslutils
from .utils import get_port


_renamed_props = {
    "host": "browser_host",
    "bind_hostname": "bind_address",
    "external_host": "server_host",
    "host_ip": "server_host",
}


def _merge_dict(base_dict, override_dict):
    rv = base_dict.copy()
    for key, value in base_dict.items():
        if key in override_dict:
            if isinstance(value, dict):
                rv[key] = _merge_dict(value, override_dict[key])
            else:
                rv[key] = override_dict[key]
    return rv


class Config(Mapping[str, Any]):
    """wptserve configuration data

    Immutable configuration that's safe to be passed between processes.

    Inherits from Mapping for backwards compatibility with the old dict-based config

    :param data: - Extra configuration data
    """
    def __init__(self, data):
        for name in data.keys():
            if name.startswith("_"):
                raise ValueError("Invalid configuration key %s" % name)
        self.__dict__.update(data)

    def __str__(self):
        return str(self.__dict__)

    def __setattr__(self, key, value):
        raise ValueError("Config is immutable")

    def __setitem__(self, key, value):
        raise ValueError("Config is immutable")

    def __getitem__(self, key):
        try:
            return getattr(self, key)
        except AttributeError:
            raise ValueError

    def __contains__(self, key):
        return key in self.__dict__

    def __iter__(self):
        return (x for x in self.__dict__ if not x.startswith("_"))

    def __len__(self):
        return len([item for item in self])

    def as_dict(self):
        return json_types(self.__dict__, skip={"_logger"})


def json_types(obj, skip=None):
    if skip is None:
        skip = set()
    if isinstance(obj, dict):
        return {key: json_types(value) for key, value in obj.items() if key not in skip}
    if (isinstance(obj, str) or
        isinstance(obj, int) or
        isinstance(obj, float) or
        isinstance(obj, bool) or
        obj is None):
        return obj
    if isinstance(obj, list) or hasattr(obj, "__iter__"):
        return [json_types(value) for value in obj]
    raise ValueError


class ConfigBuilder:
    """Builder object for setting the wptserve config.

    Configuration can be passed in as a dictionary to the constructor, or
    set via attributes after construction. Configuration options must match
    the keys on the _default class property.

    The generated configuration is obtained by using the builder
    object as a context manager; this returns a Config object
    containing immutable configuration that may be shared between
    threads and processes. In general the configuration is only valid
    for the context used to obtain it.

    with ConfigBuilder() as config:
        # Use the configuration
        print config.browser_host

    The properties on the final configuration include those explicitly
    supplied and computed properties. The computed properties are
    defined by the computed_properties attribute on the class. This
    is a list of property names, each corresponding to a _get_<name>
    method on the class. These methods are called in the order defined
    in computed_properties and are passed a single argument, a
    dictionary containing the current set of properties. Thus computed
    properties later in the list may depend on the value of earlier
    ones.


    :param logger: - A logger object. This is used for logging during
                     the creation of the configuration, but isn't
                     part of the configuration
    :param subdomains: - A set of valid subdomains to include in the
                         configuration.
    :param not_subdomains: - A set of invalid subdomains to include in
                             the configuration.
    :param config_cls: - A class to use for the configuration. Defaults
                         to default_config_cls
    """

    _default = {
        "browser_host": "localhost",
        "alternate_hosts": {},
        "doc_root": os.path.dirname("__file__"),
        "server_host": None,
        "ports": {"http": [8000]},
        "check_subdomains": True,
        "bind_address": True,
        "ssl": {
            "type": "none",
            "encrypt_after_connect": False,
            "none": {},
            "openssl": {
                "openssl_binary": "openssl",
                "base_path": "_certs",
                "password": "web-platform-tests",
                "force_regenerate": False,
                "duration": 30,
                "base_conf_path": None
            },
            "pregenerated": {
                "host_key_path": None,
                "host_cert_path": None,
            },
        },
        "aliases": [],
        "logging": {
            "level": "debug",
            "suppress_handler_traceback": False,
        }
    }
    default_config_cls = Config

    # Configuration properties that are computed. Each corresponds to a method
    # _get_foo, which is called with the current data dictionary. The properties
    # are computed in the order specified in the list.
    computed_properties = ["logging",
                           "paths",
                           "server_host",
                           "ports",
                           "domains",
                           "not_domains",
                           "all_domains",
                           "domains_set",
                           "not_domains_set",
                           "all_domains_set",
                           "ssl_config"]

    def __init__(self,
                 logger,
                 subdomains=set(),
                 not_subdomains=set(),
                 config_cls=None,
                 **kwargs):

        self._logger = logger
        self._data = self._default.copy()
        self._ssl_env = None

        self._config_cls = config_cls or self.default_config_cls

        for k, v in self._default.items():
            self._data[k] = kwargs.pop(k, v)

        self._data["subdomains"] = subdomains
        self._data["not_subdomains"] = not_subdomains

        for k, new_k in _renamed_props.items():
            if k in kwargs:
                logger.warning(
                    "%s in config is deprecated; use %s instead" % (
                        k,
                        new_k
                    )
                )
                self._data[new_k] = kwargs.pop(k)

        if kwargs:
            raise TypeError("__init__() got unexpected keyword arguments %r" % (tuple(kwargs),))

    def __setattr__(self, key, value):
        if not key[0] == "_":
            self._data[key] = value
        else:
            self.__dict__[key] = value

    def __getattr__(self, key):
        try:
            return self._data[key]
        except KeyError as e:
            raise AttributeError from e

    def update(self, override):
        """Load an overrides dict to override config values"""
        override = override.copy()

        for k in self._default:
            if k in override:
                self._set_override(k, override.pop(k))

        for k, new_k in _renamed_props.items():
            if k in override:
                self._logger.warning(
                    "%s in config is deprecated; use %s instead" % (
                        k,
                        new_k
                    )
                )
                self._set_override(new_k, override.pop(k))

        if override:
            k = next(iter(override))
            raise KeyError("unknown config override '%s'" % k)

    def _set_override(self, k, v):
        old_v = self._data[k]
        if isinstance(old_v, dict):
            self._data[k] = _merge_dict(old_v, v)
        else:
            self._data[k] = v

    def __enter__(self):
        if self._ssl_env is not None:
            raise ValueError("Tried to re-enter configuration")
        data = self._data.copy()
        prefix = "_get_"
        for key in self.computed_properties:
            data[key] = getattr(self, prefix + key)(data)
        return self._config_cls(data)

    def __exit__(self, *args):
        self._ssl_env.__exit__(*args)
        self._ssl_env = None

    def _get_logging(self, data):
        logging = data["logging"]
        logging["level"] = logging["level"].upper()
        return logging

    def _get_paths(self, data):
        return {"doc_root": data["doc_root"]}

    def _get_server_host(self, data):
        return data["server_host"] if data.get("server_host") is not None else data["browser_host"]

    def _get_ports(self, data):
        new_ports = defaultdict(list)
        for scheme, ports in data["ports"].items():
            if scheme in ["wss", "https"] and not sslutils.get_cls(data["ssl"]["type"]).ssl_enabled:
                continue
            for i, port in enumerate(ports):
                real_port = get_port("") if port == "auto" else port
                new_ports[scheme].append(real_port)
        return new_ports

    def _get_domains(self, data):
        hosts = data["alternate_hosts"].copy()
        assert "" not in hosts
        hosts[""] = data["browser_host"]

        rv = {}
        for name, host in hosts.items():
            rv[name] = {subdomain: (subdomain.encode("idna").decode("ascii") + "." + host)
                        for subdomain in data["subdomains"]}
            rv[name][""] = host
        return rv

    def _get_not_domains(self, data):
        hosts = data["alternate_hosts"].copy()
        assert "" not in hosts
        hosts[""] = data["browser_host"]

        rv = {}
        for name, host in hosts.items():
            rv[name] = {subdomain: (subdomain.encode("idna").decode("ascii") + "." + host)
                        for subdomain in data["not_subdomains"]}
        return rv

    def _get_all_domains(self, data):
        rv = copy.deepcopy(data["domains"])
        nd = data["not_domains"]
        for host in rv:
            rv[host].update(nd[host])
        return rv

    def _get_domains_set(self, data):
        return {domain
                for per_host_domains in data["domains"].values()
                for domain in per_host_domains.values()}

    def _get_not_domains_set(self, data):
        return {domain
                for per_host_domains in data["not_domains"].values()
                for domain in per_host_domains.values()}

    def _get_all_domains_set(self, data):
        return data["domains_set"] | data["not_domains_set"]

    def _get_ssl_config(self, data):
        ssl_type = data["ssl"]["type"]
        ssl_cls = sslutils.get_cls(ssl_type)
        kwargs = data["ssl"].get(ssl_type, {})
        self._ssl_env = ssl_cls(self._logger, **kwargs)
        self._ssl_env.__enter__()
        if self._ssl_env.ssl_enabled:
            key_path, cert_path = self._ssl_env.host_cert_path(data["domains_set"])
            ca_cert_path = self._ssl_env.ca_cert_path(data["domains_set"])
            return {"key_path": key_path,
                    "ca_cert_path": ca_cert_path,
                    "cert_path": cert_path,
                    "encrypt_after_connect": data["ssl"].get("encrypt_after_connect", False)}
