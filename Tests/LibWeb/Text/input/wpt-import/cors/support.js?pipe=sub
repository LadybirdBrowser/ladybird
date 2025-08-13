function dirname(path) {
    return path.replace(/\/[^\/]*$/, '/')
}

/* This subdomain should point to this same location */
var SUBDOMAIN = 'www1'
var SUBDOMAIN2 = 'www2'
var PORT = {{ports[http][1]}}
//XXX HTTPS
var PORTS = {{ports[https][0]}}

/* Changes http://example.com/abc/def/cool.htm to http://www1.example.com/abc/def/ */
var CROSSDOMAIN     = dirname(location.href)
                        .replace('://', '://' + SUBDOMAIN + '.')
var REMOTE_HOST     = SUBDOMAIN + '.' + location.host
var REMOTE_PROTOCOL = location.protocol
var REMOTE_ORIGIN   = REMOTE_PROTOCOL + '//' + REMOTE_HOST
