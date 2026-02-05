(function(root) {
'use strict';

var index = 0;
var suite = root.generalParallelTest = {
    // prepare individual test
    setup: function(data, options) {
        suite._setupDom(data, options);
        suite._setupEvents(data, options);
    },
    // clone fixture and prepare data containers
    _setupDom: function(data, options) {
        // clone fixture into off-viewport test-canvas
        data.fixture = document.getElementById('fixture').cloneNode(true);
        data.fixture.id = 'test-' + (index++);
        (document.getElementById('offscreen') || document.body).appendChild(data.fixture);

        // data container for #fixture > .container > .transition
        data.transition = {
            node: data.fixture.querySelector('.transition'),
            values: [],
            events: [],
            computedStyle: function(property) {
                return computedStyle(data.transition.node, property);
            }
        };

        // data container for #fixture > .container
        data.container = {
            node: data.transition.node.parentNode,
            values: [],
            events: [],
            computedStyle: function(property) {
                return computedStyle(data.container.node, property);
            }
        };

        // data container for #fixture > .container > .transition[:before | :after]
        if (data.pseudo) {
            data.pseudo = {
                name: data.pseudo,
                values: [],
                computedStyle: function(property) {
                    return computedStyle(data.transition.node, property, ':' + data.pseudo.name);
                }
            };
        }
    },
    // bind TransitionEnd event listeners
    _setupEvents: function(data, options) {
        ['transition', 'container'].forEach(function(elem) {
            var handler = function(event) {
                event.stopPropagation();
                var name = event.propertyName;
                var time = Math.round(event.elapsedTime * 1000) / 1000;
                var pseudo = event.pseudoElement ? (':' + event.pseudoElement) : '';
                data[elem].events.push(name + pseudo + ":" + time + "s");
            };
            data[elem].node.addEventListener('transitionend', handler, false);
            data[elem]._events = {'transitionend': handler};
        });
    },
    // cleanup after individual test
    teardown: function(data, options) {
        // data.fixture.remove();
        if (data.fixture.parentNode) {
            data.fixture.parentNode.removeChild(data.fixture);
        }
    },
    // invoked prior to running a slice of tests
    sliceStart: function(options, tests) {
        // inject styles into document
        setStyle(options.styles);
    },
    transitionsStarted: function(options, tests) {
        // kick off value collection loop
        generalParallelTest.startValueCollection(options);
    },
    // invoked after running a slice of tests
    sliceDone: function(options, tests) {
        // stop value collection loop
        generalParallelTest.stopValueCollection(options);
        // reset styles cache
        options.styles = {};
    },
    // called once all tests are done
    done: function(options) {
        // reset document styles
        setStyle();
        reflow();
    },
    // add styles of individual test to slice cache
    addStyles: function(data, options, styles) {
        if (!options.styles) {
            options.styles = {};
        }

        Object.keys(styles).forEach(function(key) {
            var selector = '#' + data.fixture.id
                // fixture must become #fixture.fixture rather than a child selector
                + (key.substring(0, 8) === '.fixture' ? '' : ' ')
                + key;

            options.styles[selector] = styles[key];
        });
    },
    // set style and compute values for container and transition
    getStyle: function(data) {
        reflow();
        // grab current styles: "initial state"
        suite._getStyleFor(data, 'from');
        // apply target state
        suite._addClass(data, 'to', true);
        // grab current styles: "target state"
        suite._getStyleFor(data, 'to');
        // remove target state
        suite._removeClass(data, 'to', true);

        // clean up the mess created for value collection
        data.container._values = [];
        data.transition._values = [];
        if (data.pseudo) {
            data.pseudo._values = [];
        }
    },
    // grab current styles and store in respective element's data container
    _getStyleFor: function(data, key) {
        data.container[key] = data.container.computedStyle(data.property);
        data.transition[key] = data.transition.computedStyle(data.property);
        if (data.pseudo) {
            data.pseudo[key] = data.pseudo.computedStyle(data.property);
        }
    },
    // add class to test's elements and possibly reflow
    _addClass: function(data, className, forceReflow) {
        data.container.node.classList.add(className);
        data.transition.node.classList.add(className);
        if (forceReflow) {
            reflow();
        }
    },
    // remove class from test's elements and possibly reflow
    _removeClass: function(data, className, forceReflow) {
        data.container.node.classList.remove(className);
        data.transition.node.classList.remove(className);
        if (forceReflow) {
            reflow();
        }
    },
    // add transition and to classes to container and transition
    startTransition: function(data) {
        // add transition-defining class
        suite._addClass(data, 'how', true);
        // add target state (without reflowing)
        suite._addClass(data, 'to', false);
    },
    // requestAnimationFrame runLoop to collect computed values
    startValueCollection: function(options) {
        const promises = [];
        const animations = document.getAnimations();
        animations.forEach(a => {
            promises.push(new Promise(resolve => {
                const listener = (event) => {
                    let found = false;
                    if (event.pseudoElement != '') {
                        if (event.pseudoElement == a.effect.pseudoElement) {
                            found = true;
                        }
                    } else if (!a.effect.pseudoElement) {
                        found = true;
                    }
                    if (found) {
                        a.effect.target.removeEventListener(
                            'transitionend', listener);
                        resolve();
                    }
                };
                a.effect.target.addEventListener('transitionend', listener);
            }));
        });
        Promise.all(promises).then(() => {
            options.allTransitionsCompleted();
        });

        // Sample at these values expressed as relative progress.
        // The last sample being the end of the transition ensure that the
        // completion events are received in short order.
        const sample_at = [0.2, 0.4, 0.6, 0.8, 1.0];
        sample_at.forEach(at => {
            const time = options.duration * at;
            animations.forEach(a => { a.currentTime = time; });
            // Collect current style for test's elements.
            options.tests.forEach(function(data) {
                if (!data.property) {
                    return;
                }

                ['transition', 'container', 'pseudo'].forEach(function(elem) {
                    var pseudo = null;
                    if (!data[elem] || (elem === 'pseudo' && !data.pseudo)) {
                        return;
                    }

                    var current = data[elem].computedStyle(data.property);
                    var values = data[elem].values;
                    var length = values.length;
                    if (!length || values[length - 1] !== current) {
                        values.push(current);
                    }
                });
            });
        });
    },
    // stop requestAnimationFrame runLoop collecting computed values
    stopValueCollection: function(options) {
        options._collectValues = false;
    },

    // generate test.step function asserting collected events match expected
    assertExpectedEventsFunc: function(data, elem, expected) {
        return function() {
            var _result = data[elem].events.sort().join(" ");
            var _expected = typeof expected === 'string' ? expected : expected.sort().join(" ");
            assert_equals(_result, _expected, "Expected TransitionEnd events triggered on ." + elem);
        };
    },
    // generate test.step function asserting collected values are neither initial nor target
    assertIntermediateValuesFunc: function(data, elem) {
        return function() {
            // the first value (index: 0) is always going to be the initial value
            // the last value is always going to be the target value
            var values = data[elem].values;
            if (data.flags.discrete) {
                // a discrete value will just switch from one state to another without having passed intermediate states.
                assert_equals(values[0], data[elem].from, "must be initial value while transitioning on ." + elem);
                assert_equals(values[1], data[elem].to, "must be target value after transitioning on ." + elem);
                assert_equals(values.length, 2, "discrete property only has 2 values ." + elem);
            } else {
                assert_not_equals(values[1], data[elem].from, "may not be initial value while transitioning on ." + elem);
                assert_not_equals(values[1], data[elem].to, "may not be target value while transitioning on ." + elem);
            }

            // TODO: first value must be initial, last value must be target
        };
    }
};

})(window);
