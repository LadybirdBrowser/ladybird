// META: title=FormData.append

    test(function() {
        assert_equals(create_formdata(['key', 'value1']).get('key'), "value1");
    }, 'testFormDataAppend1');
    test(function() {
        assert_equals(create_formdata(['key', 'value2'], ['key', 'value1']).get('key'), "value2");
    }, 'testFormDataAppend2');
    test(function() {
        assert_equals(create_formdata(['key', undefined]).get('key'), "undefined");
    }, 'testFormDataAppendUndefined1');
    test(function() {
        assert_equals(create_formdata(['key', undefined], ['key', 'value1']).get('key'), "undefined");
    }, 'testFormDataAppendUndefined2');
    test(function() {
        assert_equals(create_formdata(['key', null]).get('key'), "null");
    }, 'testFormDataAppendNull1');
    test(function() {
        assert_equals(create_formdata(['key', null], ['key', 'value1']).get('key'), "null");
    }, 'testFormDataAppendNull2');
    test(function() {
        var before = new Date(new Date().getTime() - 2000).getTime(); // two seconds ago, in case there's clock drift
        var fd = create_formdata(['key', new Blob(), 'blank.txt']).get('key');
        assert_equals(fd.name, "blank.txt");
        assert_equals(fd.type, "");
        assert_equals(fd.size, 0);
        assert_greater_than_equal(fd.lastModified, before);
        assert_less_than_equal(fd.lastModified, new Date().getTime());
    }, 'testFormDataAppendEmptyBlob');

    function create_formdata() {
        var fd = new FormData();
        for (var i = 0; i < arguments.length; i++) {
            fd.append.apply(fd, arguments[i]);
        };
        return fd;
    }
