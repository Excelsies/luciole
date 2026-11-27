#include "ui/workbench/column_preview_page.h"

#include "fs/file_icon_source.h"
#include "ui/browser/file_icon.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace luciole {
  namespace {
    constexpr float kFallbackIconSize = 96; // the type icon in place of a picture that is loading or cannot render
    constexpr float kKeyShare = 0.36F;      // of the width, for a fact's name
    constexpr float kLongestKey = 88;
    constexpr int kMessageLines = 3;
    constexpr int kTextLines = 400;
    constexpr int kKeyLines = 2;
    constexpr int kValueLines = 3;

    // The picture's box: the column's width, its height following the picture's shape within what the slot allows.
    struct ImageBox {
      float width, height;
    };

    ImageBox imageBox(int sourceWidth, int sourceHeight, float width, float height) {
      const float budget = std::max(40.0F, width - Style::spaceMd * 2 - Style::scrollbarWidth - Style::scrollbarGap);
      const float maximumHeight = std::max(80.0F, height - 250.0F);
      const bool known = sourceWidth > 0 && sourceHeight > 0;
      const float imageHeight = std::clamp(
          known ? budget * static_cast<float>(sourceHeight) / static_cast<float>(sourceWidth) : 96.0F, 48.0F,
          maximumHeight
      );
      return {budget, imageHeight};
    }

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
    content->addChild(ui::row({.justify = FlexJustify::Center}, std::move(image)));
    content->addChild(ui::label({.out = &m_message, .maxLines = kMessageLines, .configure = literal}));
    content->addChild(ui::label({.out = &m_text, .maxLines = kTextLines, .configure = [](Label& label) {
                                   label.setUseMarkup(false);
                                   label.setFontFamily("monospace");
                                 }}));
    Flex* facts = nullptr;
    content->addChild(
        ui::column(
            {.out = &facts, .align = FlexAlign::Stretch, .gap = Style::spaceXs},
            ui::label({.out = &m_details, .text = "Details", .fontWeight = FontWeight::SemiBold, .maxLines = 1})
        )
    );
    m_facts.resize(kMaxFacts);
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
    show(*m_message, false);
    show(*m_text, false);
    show(*facts, false);
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
    for (auto* label : {m_message, m_text, m_details}) {
      label->setFontSize(m_textSize);
    }
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
    for (auto* label : {m_message, m_text}) {
      label->setMaxWidth(budget);
    }
    const auto* pixels = snapshot.image && snapshot.image->pixels ? &*snapshot.image->pixels : nullptr;
    if (pixels != nullptr) {
      const auto image = imageBox(pixels->width, pixels->height, width, height);
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
    m_message->setText(snapshot.message);
    m_message->setColor(
        colorSpecFromRole(snapshot.status == PreviewStatus::Error ? ColorRole::Error : ColorRole::OnSurfaceVariant)
    );
    show(*m_message, !snapshot.message.empty());
    m_text->setText(snapshot.text);
    show(*m_text, !snapshot.text.empty());
    const float keyWidth = std::min(kLongestKey, budget * kKeyShare);
    std::size_t count = 0;
    // Bounded by the snapshot's facts and kMaxFacts.
    for (const auto& field : snapshot.metadata) {
      if (count == m_facts.size()) {
        break;
      }
      if (!field.preview) {
        continue;
      }
      const auto& fact = m_facts[count++];
      fact.key->setText(field.label);
      fact.key->setMinWidth(keyWidth);
      fact.key->setMaxWidth(keyWidth);
      fact.value->setText(field.value);
      fact.value->setMaxWidth(std::max(1.0F, budget - keyWidth - Style::spaceSm));
      show(*fact.row, true);
    }
    for (std::size_t i = count; i < m_facts.size(); ++i) {
      show(*m_facts[i].row, false);
    }
    show(*m_details->parent(), count > 0);
  }

  void ColumnPreviewPage::clear(Renderer& renderer) { m_image->releaseFile(renderer); }
} // namespace luciole
