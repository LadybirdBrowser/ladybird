function create_window_in_test(t, srcdoc) {
  let p = new Promise((resolve) => {
    let f = document.createElement('iframe');
    f.srcdoc = srcdoc ? srcdoc : '';
    f.onload = (event) => {
      let w = f.contentWindow;
      t.add_cleanup(() => f.remove());
      resolve(w);
    };
    document.body.appendChild(f);
  });
  return p;
}

function create_window_in_test_async(test, mime, doc) {
    return new Promise((resolve) => {
        let iframe = document.createElement('iframe');
        blob = new Blob([doc], {type: mime});
        iframe.src = URL.createObjectURL(blob);
        iframe.onload = (event) => {
            let contentWindow = iframe.contentWindow;
            test.add_cleanup(() => iframe.remove());
            resolve(contentWindow);
        };
        document.body.appendChild(iframe);
    });
}

function test_with_window(f, name, srcdoc) {
  promise_test((t) => {
    return create_window_in_test(t, srcdoc)
    .then((w) => {
      f(w, w.document);
    });
  }, name);
}

function define_custom_element_in_window(window, name, observedAttributes) {
    let log = [];

    class CustomElement extends window.HTMLElement {
        constructor() {
            super();
            log.push(create_constructor_log(this));
        }
        attributeChangedCallback(...args) {
            log.push(create_attribute_changed_callback_log(this, ...args));
        }
        connectedCallback() { log.push(create_connected_callback_log(this)); }
        disconnectedCallback() { log.push(create_disconnected_callback_log(this)); }
        adoptedCallback(oldDocument, newDocument) { log.push({type: 'adopted', element: this, oldDocument: oldDocument, newDocument: newDocument}); }
    }
    CustomElement.observedAttributes = observedAttributes;

    window.customElements.define(name, CustomElement);

    return {
        name: name,
        class: CustomElement,
        takeLog: function () {
            let currentLog = log; log = [];
            currentLog.types = () => currentLog.map((entry) => entry.type);
            currentLog.last = () => currentLog[currentLog.length - 1];
            return currentLog;
        }
    };
}

function create_constructor_log(element) {
    return {type: 'constructed', element: element};
}

function assert_constructor_log_entry(log, element) {
    assert_equals(log.type, 'constructed');
    assert_equals(log.element, element);
}

function create_connected_callback_log(element) {
    return {type: 'connected', element: element};
}

function assert_connected_log_entry(log, element) {
    assert_equals(log.type, 'connected');
    assert_equals(log.element, element);
}

function create_disconnected_callback_log(element) {
    return {type: 'disconnected', element: element};
}

function assert_disconnected_log_entry(log, element) {
    assert_equals(log.type, 'disconnected');
    assert_equals(log.element, element);
}

function assert_adopted_log_entry(log, element) {
    assert_equals(log.type, 'adopted');
    assert_equals(log.element, element);
}

function create_adopted_callback_log(element) {
    return {type: 'adopted', element: element};
}

function create_attribute_changed_callback_log(element, name, oldValue, newValue, namespace) {
    return {
        type: 'attributeChanged',
        element: element,
        name: name,
        namespace: namespace,
        oldValue: oldValue,
        newValue: newValue,
        actualValue: element.getAttributeNS(namespace, name)
    };
}

function assert_attribute_log_entry(log, expected) {
    assert_equals(log.type, 'attributeChanged');
    assert_equals(log.name, expected.name);
    assert_equals(log.oldValue, expected.oldValue);
    assert_equals(log.newValue, expected.newValue);
    assert_equals(log.actualValue, expected.newValue);
    assert_equals(log.namespace, expected.namespace);
}


function define_new_custom_element(observedAttributes) {
    let log = [];
    let name = 'custom-element-' + define_new_custom_element._element_number++;

    class CustomElement extends HTMLElement {
        constructor() {
            super();
            log.push({type: 'constructed', element: this});
        }
        attributeChangedCallback(...args) {
            log.push(create_attribute_changed_callback_log(this, ...args));
        }
        connectedCallback() { log.push({type: 'connected', element: this}); }
        disconnectedCallback() { log.push({type: 'disconnected', element: this}); }
        adoptedCallback(oldDocument, newDocument) { log.push({type: 'adopted', element: this, oldDocument: oldDocument, newDocument: newDocument}); }
    }
    CustomElement.observedAttributes = observedAttributes;

    customElements.define(name, CustomElement);

    return {
        name: name,
        class: CustomElement,
        takeLog: function () {
            let currentLog = log; log = [];
            currentLog.types = () => currentLog.map((entry) => entry.type);
            currentLog.last = () => currentLog[currentLog.length - 1];
            return currentLog;
        }
    };
}
define_new_custom_element._element_number = 1;

function define_build_in_custom_element(observedAttributes, extendedElement, extendsOption) {
    let log = [];
    let name = 'custom-element-' + define_build_in_custom_element._element_number++;

    class CustomElement extends extendedElement {
        constructor() {
            super();
            log.push({type: 'constructed', element: this});
        }
        attributeChangedCallback(...args) {
            log.push(create_attribute_changed_callback_log(this, ...args));
        }
        connectedCallback() { log.push({type: 'connected', element: this}); }
        disconnectedCallback() { log.push({type: 'disconnected', element: this}); }
        adoptedCallback(oldDocument, newDocument) { log.push({type: 'adopted', element: this, oldDocument: oldDocument, newDocument: newDocument}); }
    }
    CustomElement.observedAttributes = observedAttributes;
    customElements.define(name, CustomElement, { extends: extendsOption});

    return {
        name: name,
        class: CustomElement,
        takeLog: function () {
            let currentLog = log; log = [];
            currentLog.types = () => currentLog.map((entry) => entry.type);
            currentLog.last = () => currentLog[currentLog.length - 1];
            return currentLog;
        }
    };
}
define_build_in_custom_element._element_number = 1;

function document_types() {
    return [
        {
            name: 'the document',
            create: function () { return Promise.resolve(document); },
            isOwner: true,
            hasBrowsingContext: true,
        },
        {
            name: 'the document of the template elements',
            create: function () {
                return new Promise(function (resolve) {
                    var template = document.createElementNS('http://www.w3.org/1999/xhtml', 'template');
                    var doc = template.content.ownerDocument;
                    if (!doc.documentElement)
                        doc.appendChild(doc.createElement('html'));
                    resolve(doc);
                });
            },
            hasBrowsingContext: false,
        },
        {
            name: 'a new document',
            create: function () {
                return new Promise(function (resolve) {
                    var doc = new Document();
                    doc.appendChild(doc.createElement('html'));
                    resolve(doc);
                });
            },
            hasBrowsingContext: false,
        },
        {
            name: 'a cloned document',
            create: function () {
                return new Promise(function (resolve) {
                    var doc = document.cloneNode(false);
                    doc.appendChild(doc.createElement('html'));
                    resolve(doc);
                });
            },
            hasBrowsingContext: false,
        },
        {
            name: 'a document created by createHTMLDocument',
            create: function () {
                return Promise.resolve(document.implementation.createHTMLDocument());
            },
            hasBrowsingContext: false,
        },
        {
            name: 'an HTML document created by createDocument',
            create: function () {
                return Promise.resolve(document.implementation.createDocument('http://www.w3.org/1999/xhtml', 'html', null));
            },
            hasBrowsingContext: false,
        },
        {
            name: 'the document of an iframe',
            create: function () {
                return new Promise(function (resolve, reject) {
                    var iframe = document.createElement('iframe');
                    iframe.onload = function () { resolve(iframe.contentDocument); }
                    iframe.onerror = function () { reject('Failed to load an empty iframe'); }
                    document.body.appendChild(iframe);
                });
            },
            hasBrowsingContext: true,
        },
        {
            name: 'an HTML document fetched by XHR',
            create: function () {
                return new Promise(function (resolve, reject) {
                    var xhr = new XMLHttpRequest();
                    xhr.open('GET', 'resources/empty-html-document.html');
                    xhr.overrideMimeType('text/xml');
                    xhr.onload = function () { resolve(xhr.responseXML); }
                    xhr.onerror = function () { reject('Failed to fetch the document'); }
                    xhr.send();
                });
            },
            hasBrowsingContext: false,
        }
    ];
}
