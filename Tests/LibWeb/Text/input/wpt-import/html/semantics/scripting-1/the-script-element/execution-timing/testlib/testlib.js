/*
* Utility functions for script scheduler test
*/
(function(){ /* namespace hiding local variables like arOrderOfAllEvents from global scope */
    window.testlib = {};
    window.eventOrder = [];
    var arNumberOfScriptsParsedPerEvent=[];
    window.log = function (str){
        eventOrder.push(str);
        arNumberOfScriptsParsedPerEvent.push(document.getElementsByTagName('script').length);
    }

    window.testlib.addScript = function(source, attributes, parent, firstInParent,funcPrepare) {
        try{
            parent = parent||document.body;
            var script = document.createElement('script');
            if(funcPrepare) {
                funcPrepare(script);
            }
            if(source)script.appendChild( document.createTextNode(source) );
            for( var name in attributes){
                if(/^on/i.test(name)) {
                    script[name] = attributes[name];
                } else {
                    script.setAttribute(name, attributes[name]);
                }
            }
            if (firstInParent && parent.firstChild) {
                parent.insertBefore(script, parent.firstChild);
            } else {
                parent.appendChild(script);
            }
        } catch(e) {
            log('ERROR when adding script to DOM!');
            alert(e);
        }
        return script;
    }

    window.testlib.urlParam = function(relativeURL) {
        return location.href.replace( /\d*\.html$/, relativeURL);
    }
})();
