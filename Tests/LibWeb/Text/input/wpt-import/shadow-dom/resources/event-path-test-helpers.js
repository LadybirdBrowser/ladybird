
function dispatchEventWithEventLog(shadow, target, event) {
    var eventPath = [];
    var targets = [];
    var relatedTargets = [];
    var pathAtTargets = [];

    var attachedNodes = [];
    for (var nodeKey in shadow) {
        var startingNode = shadow[nodeKey];
        for (var node = startingNode; node; node = node.parentNode) {
            if (attachedNodes.indexOf(node) >= 0)
                continue;
            attachedNodes.push(node);
            node.addEventListener(event.type, (function (event) {
                eventPath.push(this.label);
                relatedTargets.push(event.relatedTarget ? event.relatedTarget.label : null);

                pathAtTargets.push(event.composedPath().map(function (node) { return node.label; }));
                targets.push(event.target);
            }).bind(node));
        }
    }

    target.dispatchEvent(event);

    return {event: event, targets: targets, eventPath: eventPath, relatedTargets: relatedTargets, pathAtTargets: pathAtTargets};
}

/*
-SR: ShadowRoot  -S: Slot
A ------------------------------- A-SR
+ B ------------ B-SR             + A1 --- A1-SR
  + C            + B1 --- B1-SR   + A2-S   + A1a
  + D --- D-SR     + B1a  + B1b --- B1b-SR
          + D1            + B1c-S   + B1b1
                                    + B1b2
*/
function createFixedTestTree(mode) {
    var namedNodes = {};

    function element(name) {
        var element = document.createElement(name.indexOf('-S') > 0 ? 'slot' : 'div');
        element.label = name;
        namedNodes[name] = element;
        for (var i = 1; i < arguments.length; i++) {
            var item = arguments[i];
            if (typeof(item) == 'function')
                item(element);
            else
                element.appendChild(item);
        }
        return element;
    }

    function shadow(name) {
        var children = [];
        for (var i = 1; i < arguments.length; i++)
            children.push(arguments[i]);
        return function (element) {
            var shadowRoot = element.attachShadow({mode: mode});
            shadowRoot.label = name;
            namedNodes[name] = shadowRoot;
            for (var child of children)
                shadowRoot.appendChild(child);
        }
    }

    var host = element('A',
        shadow('A-SR',
            element('A1',
                shadow('A1-SR',
                    element('A1a'))),
            element('A2-S')
        ),
        element('B',
            shadow('B-SR',
                element('B1',
                    shadow('B1-SR',
                        element('B1b',
                            shadow('B1b-SR',
                                element('B1b1'),
                                element('B1b2'))),
                        element('B1c-S')),
                    element('B1a'))),
            element('C'),
            element('D',
                shadow('D-SR',
                    element('D1')))));

    return namedNodes;
}
