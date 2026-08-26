/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <LibGC/Weak.h>
#include <LibGfx/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DecodedImageData.h>

namespace Web::CSS {

class ImageStyleValue;

class ImageStyleValueResource final : public HTML::DecodedImageData::Client {
public:
    explicit ImageStyleValueResource(GC::Ref<HTML::SharedResourceRequest>, GC::Ref<DOM::Document> const&);
    ~ImageStyleValueResource();

    void visit_edges(JS::Cell::Visitor&);

    void register_image_style_value(ImageStyleValue const&);
    void unregister_image_style_value(ImageStyleValue const&);
    bool can_be_removed() const { return m_registration_counts_by_image_style_value.is_empty(); }

    ::URL::URL const& url() const;

    [[nodiscard]] virtual GC::Ptr<HTML::DecodedImageData> decoded_image_data() const override;

private:
    virtual void decoded_image_data_did_update() override { notify_image_style_values_did_update(); }

    void on_decoded_image_data_loaded();
    void notify_image_style_values_did_update();

    GC::Ref<HTML::SharedResourceRequest> m_resource_request;
    HashMap<ImageStyleValue const*, size_t> m_registration_counts_by_image_style_value;
};

class ImageStyleValue final
    : public AbstractImageStyleValue
    , public Weakable<ImageStyleValue> {

    using Base = AbstractImageStyleValue;

public:
    class Client {
        friend class ImageStyleValue;

    public:
        Client(DOM::Document&, ImageStyleValue const&);
        virtual ~Client();
        virtual void image_style_value_did_update(ImageStyleValue&) = 0;

        GC::Ptr<HTML::DecodedImageData> decoded_image_data() const;

    protected:
        void image_style_value_finalize();
        GC::Ptr<DOM::Document> document() const { return m_document.ptr(); }

        ImageStyleValue const& m_image_style_value;
        GC::Weak<DOM::Document> m_document;
        ImageStyleValueResource* m_resource { nullptr };
    };

    static ValueComparingNonnullRefPtr<ImageStyleValue const> create(URL const&);
    static ValueComparingNonnullRefPtr<ImageStyleValue const> create(URL const&, Optional<::URL::URL> style_resource_base_url);
    static ValueComparingNonnullRefPtr<ImageStyleValue const> create(::URL::URL const&);
    virtual ~ImageStyleValue() override;

    virtual void load_any_resources(DOM::Document&) override;

    virtual bool is_paintable(GC::Ptr<HTML::DecodedImageData> decoded_image_data) const override { return !!decoded_image_data; }

private:
    friend class ImageStyleValueResource;
    friend class Client;
    friend class CSSStyleSheet;
    ImageStyleValue(URL const&, Optional<::URL::URL> style_resource_base_url = {}, Optional<bool> parent_style_sheet_origin_clean = {}, bool should_absolutize_url_for_computed_value = false);
    explicit ImageStyleValue(StyleValueFFI::StyleValueData const*);

    void register_client(Client&) const;
    void unregister_client(Client&) const;
    void notify_clients_did_update() const;
    void update_style_sheet_resource_context(CSSStyleSheet const&);
    GC::Ptr<HTML::SharedResourceRequest> fetch_image(DOM::Document&) const;
    Optional<::URL::URL> resolved_url(DOM::Document const&) const;
    ::URL::URL style_resource_base_url(DOM::Document const&) const;

    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
    void set_style_sheet(GC::Ptr<CSSStyleSheet>);

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    URL url_value() const;

    static StyleValueFFI::StyleValueData const* make_image_url_data(URL const&, Optional<::URL::URL> const&, Optional<bool>, bool should_absolutize_url_for_computed_value);

    // NB: Style sheet attachment and loading state, not value data.
    Optional<::URL::URL> m_style_resource_base_url;
    Optional<bool> m_parent_style_sheet_origin_clean;
    bool m_should_absolutize_url_for_computed_value { false };

    mutable HashTable<Client*> m_clients;
};

}
