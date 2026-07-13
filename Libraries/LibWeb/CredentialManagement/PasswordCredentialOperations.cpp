/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/PasswordCredentialOperations.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#abstract-opdef-create-a-passwordcredential-from-an-htmlformelement
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> create_password_credential(JS::Realm& realm, GC::Ref<HTML::HTMLFormElement> form, URL::Origin origin)
{
    // 1. Let data be a new PasswordCredentialData dictionary.
    Bindings::PasswordCredentialData data;

    // 2. Set data’s origin member’s value to origin’s value.

    // 3. Let formData be the result of executing the FormData constructor on form.
    auto form_data = TRY(XHR::FormData::construct_impl(realm, form));

    // 4. Let elements be a list of all the submittable elements whose form owner is form, in tree order.
    auto elements = form->get_submittable_elements();

    // 5. Let newPasswordObserved be false.
    bool new_password_observed = false;

    // 6. For each field in elements, run the following steps:
    for (auto const& field : elements) {
        // 1. If field does not have an autocomplete attribute, then skip to the next field.
        if (auto attr = field->attribute(HTML::AttributeNames::autocomplete); !attr.has_value() || attr->is_empty())
            continue;

        // 2. Let name be the value of field’s name attribute.
        // 3. If formData’s has() method returns false when executed on name, then skip to the next field.
        auto name = field->attribute(HTML::AttributeNames::name);
        if (!name.has_value())
            continue;
        if (!form_data->has(*name))
            continue;

        // 4. If field’s autocomplete attribute’s value contains one or more autofill detail tokens (tokens), then:
        // 1. For each token in tokens:
        auto autocomplete_attribute = field->attribute(HTML::AttributeNames::autocomplete).value();
        for (auto token : autocomplete_attribute.split_view(u' ', SplitBehavior::Nothing)) {
            // 1. If token is an ASCII case-insensitive match for one of the following strings, run the associated steps:
            //    - "new-password"
            //       Set data’s password member’s value to the result of executing formData’s get() method on name,
            //       and newPasswordObserved to true.
            if (token.equals_ignoring_ascii_case(u"new-password"sv)) {
                if (auto password = form_data->get(*name); password.has<Utf16String>()) {
                    data.password = password.get<Utf16String>();
                    new_password_observed = true;
                }
            }
            //    - "current-password"
            //       If newPasswordObserved is false, set data’s password member’s value to the result of executing
            //       formData’s get() method on name.
            //       Note: By checking that newPasswordObserved is false, new-password fields take precedence over
            //             current-password fields.
            if (!new_password_observed && token.equals_ignoring_ascii_case(u"current-password"sv)) {
                if (auto password = form_data->get(*name); password.has<Utf16String>())
                    data.password = password.get<Utf16String>();
            }
            //    - "photo"
            //      Set data’s iconURL member’s value to the result of executing formData’s get() method on name.
            if (token.equals_ignoring_ascii_case(u"photo"sv)) {
                if (auto photo = form_data->get(*name); photo.has<Utf16String>())
                    data.icon_url = photo.get<Utf16String>();
            }
            //    - "name"
            //    - "nickname"
            //      Set data’s name member’s value to the result of executing formData’s get() method on name.
            if (token.equals_ignoring_ascii_case(u"name"sv)) {
                if (auto name_ = form_data->get(*name); name_.has<Utf16String>())
                    data.name = name_.get<Utf16String>();
            }
            if (token.equals_ignoring_ascii_case(u"nickname"sv)) {
                if (auto nickname = form_data->get(*name); nickname.has<Utf16String>())
                    data.name = nickname.get<Utf16String>();
            }
            //    - "username"
            //      Set data’s id member’s value to the result of executing formData’s get() method on name.
            if (token.equals_ignoring_ascii_case(u"username"sv)) {
                if (auto username = form_data->get(*name); username.has<Utf16String>()) {
                    auto id = username.get<Utf16String>();
                    data.id = id;
                }
            }
        }
    }

    // 7. Let c be the result of executing Create a PasswordCredential from PasswordCredentialData on data.
    //    If that threw an exception, rethrow that exception.
    // 8. Assert: c is a PasswordCredential.
    // 9. Return c.
    return create_password_credential(realm, data, move(origin));
}

// https://www.w3.org/TR/credential-management-1/#abstract-opdef-create-a-passwordcredential-from-passwordcredentialdata
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> create_password_credential(JS::Realm& realm, Bindings::PasswordCredentialData const& data, URL::Origin origin)
{
    // 1. Let c be a new PasswordCredential object.
    // 2. If any of the following are the empty string, throw a TypeError exception:
    //    - data’s id member’s value
    //    - data’s origin member’s value
    //      NOTE: origin cannot be an empty string at this time since it is retrieved from the current settings object
    //            in the constructor.
    //    - data’s password member’s value
    if (data.id.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "'id' must not be empty."_utf16 };
    if (data.password.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "'password' must not be empty."_utf16 };

    // 3. Set c’s properties as follows:
    //    - password
    //      - data’s password member’s value
    //    - id
    //      - data’s id member’s value
    //    - iconUrl
    //      - data’s iconURL member’s value
    //    - name
    //      - data’s name member’s value
    //    - [[origin]]
    //      - data’s origin member’s value.
    //        NOTE: origin is retrieved from the current settings object in the constructor.
    // 4. Return c.
    return realm.create<PasswordCredential>(realm, data, move(origin));
}

}
