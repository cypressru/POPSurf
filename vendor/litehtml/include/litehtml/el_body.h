#ifndef LITEHTML_EL_BODY_H
#define LITEHTML_EL_BODY_H

#include "html_tag.h"

namespace litehtml
{
    class el_body : public html_tag
    {
      public:
        explicit el_body(const std::shared_ptr<litehtml::document>& doc);

        bool is_body() const override;

        // KOSWeb: <body bgcolor> and text/link colours. Upstream maps these
        // presentational attributes on table elements only, but the pages this
        // browser targets are Dream Passport era and set them on body.
        void parse_attributes() override;
    };
} // namespace litehtml

#endif // LITEHTML_EL_BODY_H
