#include "ui/controls/label.h"
#include "ui/controls/text_tile_view.h"
#include "ui/style.h"

#include <algorithm>

// What a card around the text tile view needs to hold it whole.
LayoutSize TextTileView::fitWords(Renderer& renderer, float width, std::optional<float> height) {
  m_label->setVisible(!m_words.empty());
  if (m_words.empty()) {
    return {};
  }
  const float room = std::max(0.0F, width - (2 * Style::spaceSm));
  m_label->setText(m_words);
  m_label->setFontSize(m_textSize);
  m_label->setMaxLines(m_caption ? 1 : 0);
  m_label->setMaxWidth(room);
  auto constraints = height ? LayoutConstraints::available(room, *height) : LayoutConstraints::unconstrained();
  if (!height) {
    constraints.setMaxWidth(room);
  }
  return m_label->Node::measure(renderer, constraints);
}

float TextTileView::naturalHeight(Renderer& renderer, float width) {
  const auto words = fitWords(renderer, width, std::nullopt);
  const auto text = static_cast<float>(m_shown.geometry.height / m_shown.scale);
  const float bar = maxScrollX() > 0 ? Style::scrollbarWidth : 0.0F;
  if (m_words.empty()) {
    return text + bar;
  }
  // The words above the text as laid out, or alone with the same room around them.
  const float above = Style::spaceSm + words.height;
  return m_caption ? above + Style::spaceXs + text + bar : above + Style::spaceSm;
}
