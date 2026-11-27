#include "ui/workbench/column_preview_page.h"

#include "browser/location_presentation.h"
#include "fs/file_icon_source.h"
#include "ui/browser/file_icon.h"
#include "ui/builders.h"
#include "ui/controls/card_style.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/column_preview_layout.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace luciole {
  namespace {
    constexpr float kFallbackIconSize = 96; // the type icon in place of a picture that is loading or cannot render
    constexpr float kKeyShare = 0.36F;      // of the width, for a fact's name
    constexpr float kLongestKey = 88;
    constexpr int kTextLines = 400;
    constexpr int kKeyLines = 2;
    constexpr int kValueLines = 3;

    void show(Node& node, bool shown) {
      node.setVisible(shown);
      node.setParticipatesInLayout(shown);
    }
  } // namespace

  ColumnPreviewPage::ColumnPreviewPage(FileIconCache& icons) : m_textSize(Style::fontSizeCaption) {
    setDirection(FlexDirection::Vertical);
    setAlign(FlexAlign::Stretch);
    setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    setClipChildren(true);
    const auto literal = [](Label& label) { label.setUseMarkup(false); };
    const auto quiet = colorSpecFromRole(ColorRole::OnSurfaceVariant);
    auto body = ui::scrollView(
        {.viewportPaddingH = Style::spaceSm, .viewportPaddingV = Style::spaceSm, .fillWidth = true, .flexGrow = 1}
    );
    m_scroll = body.get();
    auto* content = body->content();
    content->setAlign(FlexAlign::Stretch);
    content->setGap(Style::spaceSm);
    auto image = std::make_unique<FileIcon>(icons, kFallbackIconSize);
    m_image = image.get();
    content->addChild(ui::row({.justify = FlexJustify::Center, .paddingV = kColumnImageInset}, std::move(image)));
    // The text reads as a page, styled like the side preview's text page and Details card.
    content->addChild(
        ui::column(
            {.out = &m_pageCard,
             .align = FlexAlign::Stretch,
             .configure =
                 [](Flex& page) {
                   page.setCardStyle();
                   page.setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
                   page.setPadding(Style::spaceSm);
                 }},
            ui::label({.out = &m_text, .maxLines = kTextLines, .configure = [](Label& label) {
                         label.setUseMarkup(false);
                         label.setFontFamily("monospace");
                       }})
        )
    );
    Flex* facts = nullptr;
    auto detailsCard = luciole::card({}, &facts);
    m_detailsCard = detailsCard.get();
    // The heading keeps the page's one text size, so the side preview's larger title is not used; its badge is the
    // side preview's own.
    detailsCard->insertChildAt(
        0,
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm},
            ui::label(
                {.out = &m_details,
                 .text = "Details",
                 .fontWeight = FontWeight::SemiBold,
                 .color = colorSpecFromRole(ColorRole::OnSurface),
                 .maxLines = 1,
                 .flexGrow = 1}
            ),
            makeStatusBadge(m_badge)
        )
    );
    facts->setGap(Style::spaceXs);
    content->addChild(std::move(detailsCard));
    m_facts.resize(kMaxFacts + 1);
    // Bounded by kMaxFacts.
    for (auto& fact : m_facts) {
      facts->addChild(
          ui::row(
              {.out = &fact.row, .align = FlexAlign::Start, .gap = Style::spaceSm},
              ui::label({.out = &fact.key, .color = quiet, .maxLines = kKeyLines, .configure = literal}),
              ui::label({.out = &fact.value, .maxLines = kValueLines, .flexGrow = 1, .configure = literal})
          )
      );
      show(*fact.row, false);
    }
    show(*m_pageCard, false);
    show(*m_detailsCard, false);
    addChild(std::move(body));
    applyTextSize();
  }

  ColumnPreviewPage::~ColumnPreviewPage() = default;

  void ColumnPreviewPage::setTextSize(float size) {
    if (!std::isfinite(size) || size <= 0 || size == m_textSize) {
      return;
    }
    m_textSize = size;
    applyTextSize();
  }

  void ColumnPreviewPage::applyTextSize() {
    for (auto* label : {m_text, m_details}) {
      label->setFontSize(m_textSize);
    }
    showStatusBadge(m_badge, m_badgeText, m_badgeProblem, m_textSize);
    for (const auto& fact : m_facts) {
      fact.key->setFontSize(m_textSize);
      fact.value->setFontSize(m_textSize);
    }
  }

  void ColumnPreviewPage::sync(const PreviewSnapshot& snapshot, float width, float height) {
    const auto& entry = snapshot.entry;
    const std::string location = entry ? entry->location().uri() : std::string{};
    if (location != m_location) {
      m_location = location;
      m_scroll->requestScrollToOffset(0);
    }
    const float budget = std::max(1.0F, width - (Style::spaceSm * 2) - Style::scrollbarWidth - Style::scrollbarGap);
    // The cards' own padding and outline come off the width of what they hold.
    const float pageWidth = std::max(1.0F, budget - (2 * Style::spaceSm) - (2 * Style::borderWidth));
    const float cardWidth = std::max(1.0F, budget - (2 * Style::spaceMd) - (2 * Style::borderWidth));
    m_text->setMaxWidth(pageWidth);
    const auto* pixels = snapshot.image && snapshot.image->pixels ? &*snapshot.image->pixels : nullptr;
    if (pixels != nullptr) {
      const auto image = columnImageLayout(pixels->width, pixels->height, width, height, 1);
      m_image->setIconSize(image.width);
      m_image->setPreviewHeight(image.height);
      m_image->setPreparedImage(snapshot.image);
    } else {
      m_image->setIconSize(kFallbackIconSize);
      m_image->setPreviewHeight(0);
      m_image->setPreparedImage(nullptr);
    }
    if (entry) {
      m_image->bind(iconSource(entry->icon), colorSpecFromRole(ColorRole::OnSurface));
    }
    // A text speaks for itself; the icon would only push it down.
    show(*m_image->parent(), entry && (pixels != nullptr || snapshot.text.empty()));
    m_text->setText(snapshot.text);
    show(*m_pageCard, !snapshot.text.empty());
    m_badgeText = snapshot.message;
    m_badgeProblem = snapshot.status == PreviewStatus::Error;
    showStatusBadge(m_badge, m_badgeText, m_badgeProblem, m_textSize);
    const float keyWidth = std::min(kLongestKey, cardWidth * kKeyShare);
    std::size_t count = 0;
    // Bounded by the snapshot's facts and kMaxFacts.
    const auto put = [&](const MetadataField& field) {
      const auto& fact = m_facts[count++];
      fact.key->setText(field.label);
      fact.key->setMinWidth(keyWidth);
      fact.key->setMaxWidth(keyWidth);
      fact.value->setText(field.value);
      fact.value->setMaxLines(field.singleLine ? 1 : kValueLines);
      fact.value->setEllipsize(field.singleLine ? TextEllipsize::Middle : TextEllipsize::End);
      fact.value->setMaxWidth(std::max(1.0F, cardWidth - keyWidth - Style::spaceSm));
      show(*fact.row, true);
    };
    for (const auto& field : snapshot.metadata) {
      if (count == kMaxFacts) {
        break;
      }
      if (field.preview) {
        put(field);
      }
    }
    if (entry) {
      put({.label = "Location", .value = locationEditText(entry->location()), .singleLine = true});
    }
    for (std::size_t i = count; i < m_facts.size(); ++i) {
      show(*m_facts[i].row, false);
    }
    show(*m_detailsCard, count > 0 || !snapshot.message.empty());
  }

  void ColumnPreviewPage::clear(Renderer& renderer) { m_image->releaseFile(renderer); }
} // namespace luciole
