# mypy: allow-untyped-defs

import base64
import json
import os
import threading
import queue
import uuid

from multiprocessing.managers import BaseManager, BaseProxy
# We also depend on some undocumented parts of multiprocessing.managers which
# don't have any type annotations.
from multiprocessing.managers import AcquirerProxy, DictProxy, public_methods  # type: ignore
from typing import Dict

from .utils import isomorphic_encode


class StashManager(BaseManager):
    shared_data: Dict[str, object] = {}
    lock = threading.Lock()


def _get_shared():
    return StashManager.shared_data


def _get_lock():
    return StashManager.lock

StashManager.register("get_dict",
                      callable=_get_shared,
                      proxytype=DictProxy)
StashManager.register('Lock',
                      callable=_get_lock,
                      proxytype=AcquirerProxy)


# We have to create an explicit class here because the built-in
# AutoProxy has a bug with nested managers, and the MakeProxy
# method doesn't work with spawn-based multiprocessing, since the
# generated class can't be pickled for use in child processes.
class QueueProxy(BaseProxy):
    _exposed_ = public_methods(queue.Queue)


for method in QueueProxy._exposed_:

    def impl_fn(method):
        def _impl(self, *args, **kwargs):
            return self._callmethod(method, args, kwargs)
        _impl.__name__ = method
        return _impl

    setattr(QueueProxy, method, impl_fn(method))  # type: ignore


StashManager.register("Queue",
                      callable=queue.Queue,
                      proxytype=QueueProxy)


class StashServer:
    def __init__(self, address=None, authkey=None, mp_context=None):
        self.address = address
        self.authkey = authkey
        self.manager = None
        self.mp_context = mp_context

    def __enter__(self):
        self.manager, self.address, self.authkey = start_server(self.address,
                                                                self.authkey,
                                                                self.mp_context)
        store_env_config(self.address, self.authkey)

    def __exit__(self, *args, **kwargs):
        if self.manager is not None:
            self.manager.shutdown()


def load_env_config():
    address, authkey = json.loads(os.environ["WPT_STASH_CONFIG"])
    if isinstance(address, list):
        address = tuple(address)
    else:
        address = str(address)
    authkey = base64.b64decode(authkey)
    return address, authkey


def store_env_config(address, authkey):
    authkey = base64.b64encode(authkey)
    os.environ["WPT_STASH_CONFIG"] = json.dumps((address, authkey.decode("ascii")))


def start_server(address=None, authkey=None, mp_context=None):
    if isinstance(authkey, str):
        authkey = authkey.encode("ascii")
    kwargs = {}
    if mp_context is not None:
        kwargs["ctx"] = mp_context
    manager = StashManager(address, authkey, **kwargs)
    manager.start()

    address = manager._address
    if isinstance(address, bytes):
        address = address.decode("ascii")
    return (manager, address, manager._authkey)


class LockWrapper:
    def __init__(self, lock):
        self.lock = lock

    def acquire(self):
        self.lock.acquire()

    def release(self):
        self.lock.release()

    def __enter__(self):
        self.acquire()

    def __exit__(self, *args, **kwargs):
        self.release()


#TODO: Consider expiring values after some fixed time for long-running
#servers

class Stash:
    """Key-value store for persisting data across HTTP/S and WS/S requests.

    This data store is specifically designed for persisting data across server
    requests. The synchronization is achieved by using the BaseManager from
    the multiprocessing module so different processes can acccess the same data.

    Stash can be used interchangeably between HTTP, HTTPS, WS and WSS servers.
    A thing to note about WS/S servers is that they require additional steps in
    the handlers for accessing the same underlying shared data in the Stash.
    This can usually be achieved by using load_env_config(). When using Stash
    interchangeably between HTTP/S and WS/S request, the path part of the key
    should be expliclitly specified if accessing the same key/value subset.

    The store has several unusual properties. Keys are of the form (path,
    uuid), where path is, by default, the path in the HTTP request and
    uuid is a unique id. In addition, the store is write-once, read-once,
    i.e. the value associated with a particular key cannot be changed once
    written and the read operation (called "take") is destructive. Taken together,
    these properties make it difficult for data to accidentally leak
    between different resources or different requests for the same
    resource.
    """

    _proxy = None
    lock = None
    manager = None
    _initializing = threading.Lock()

    def __init__(self, default_path, address=None, authkey=None):
        self.default_path = default_path
        self._get_proxy(address, authkey)
        self.data = Stash._proxy

    def _get_proxy(self, address=None, authkey=None):
        if address is None and authkey is None:
            Stash._proxy = {}
            Stash.lock = threading.Lock()

        # Initializing the proxy involves connecting to the remote process and
        # retrieving two proxied objects. This process is not inherently
        # atomic, so a lock must be used to make it so. Atomicity ensures that
        # only one thread attempts to initialize the connection and that any
        # threads running in parallel correctly wait for initialization to be
        # fully complete.
        with Stash._initializing:
            if Stash.lock:
                return

            Stash.manager = StashManager(address, authkey)
            Stash.manager.connect()
            Stash._proxy = self.manager.get_dict()
            Stash.lock = LockWrapper(self.manager.Lock())

    def get_queue(self):
        return self.manager.Queue()

    def _wrap_key(self, key, path):
        if path is None:
            path = self.default_path
        # This key format is required to support using the path. Since the data
        # passed into the stash can be a DictProxy which wouldn't detect
        # changes when writing to a subdict.
        if isinstance(key, bytes):
            # UUIDs are within the ASCII charset.
            key = key.decode('ascii')
        try:
            my_uuid = uuid.UUID(key).bytes
        except ValueError as e:
            raise ValueError(f"""Invalid UUID "{key}" used as stash key""") from e
        return (isomorphic_encode(path), my_uuid)

    def put(self, key, value, path=None):
        """Place a value in the shared stash.

        :param key: A UUID to use as the data's key.
        :param value: The data to store. This can be any python object.
        :param path: The path that has access to read the data (by default
                     the current request path)"""
        if value is None:
            raise ValueError("SharedStash value may not be set to None")
        internal_key = self._wrap_key(key, path)
        if internal_key in self.data:
            raise StashError("Tried to overwrite existing shared stash value "
                             "for key %s (old value was %s, new value is %s)" %
                             (internal_key, self.data[internal_key], value))
        else:
            self.data[internal_key] = value

    def take(self, key, path=None):
        """Remove a value from the shared stash and return it.

        :param key: A UUID to use as the data's key.
        :param path: The path that has access to read the data (by default
                     the current request path)"""
        internal_key = self._wrap_key(key, path)
        value = self.data.get(internal_key, None)
        if value is not None:
            try:
                self.data.pop(internal_key)
            except KeyError:
                # Silently continue when pop error occurs.
                pass

        return value


class StashError(Exception):
    pass
