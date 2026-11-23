#include "ui/workbench/column_preview_view.h"

#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/column_preview_header.h"
#include "ui/workbench/list_rows.h"
#include "ui/workbench/workbench_preferences.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <tuple>
#include <utility>

namespace luciole {
  namespace {
    constexpr float kButtonHeightPerText = 2; // the Open button's height in text sizes
    constexpr float kSizeWidthPerText = 3.2F; // the step's width in text sizes: "300%" fits
    constexpr int kMessageLines = 6;
    constexpr int kNoticeLines = 3;
    constexpr auto kEnableTooltip = "Edit this file here";
    constexpr auto kSmaller = "Smaller";
    constexpr auto kLarger = "Larger";
    constexpr auto kReset = "Back to 100%";

    // Holds the view's parts and stands behind every control in them: where no control takes a press, this takes it,
    // and a press on an area that does not ask for the keyboard leaves it where it was.
    class Keeper final : public InputArea {
    protected:
      LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
        // Fills a bounded space; with a side unbounded, that side is as large as the parts.
        LayoutSize parts{};
        if (!constraints.hasMaxWidth || !constraints.hasMaxHeight) {
          for (const auto& child : children()) {
            const auto size = child->measure(renderer, constraints);
            parts = {std::max(parts.width, size.width), std::max(parts.height, size.height)};
          }
        }
        return constraints.constrain(
            {constraints.hasMaxWidth ? constraints.maxWidth : parts.width,
             constraints.hasMaxHeight ? constraints.maxHeight : parts.height}
        );
      }
      void doLayout(Renderer& renderer) override {
        for (const auto& child : children()) {
          child->arrange(renderer, {0, 0, width(), height()});
        }
      }
    };

    void show(Node& node, bool shown) {
      node.setVisible(shown);
      node.setParticipatesInLayout(shown);
    }
  } // namespace

  ColumnPreviewView::ColumnPreviewView()
      : m_textSize(quietTextSize(Style::fontSizeBody, 0)), m_previewSize(kDefaultPreviewSize) {
    setDirection(FlexDirection::Vertical);
    setAlign(FlexAlign::Stretch);
    setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    setClipChildren(true);
    const auto literal = [](Label& label) { label.setUseMarkup(false); };
    const auto quiet = colorSpecFromRole(ColorRole::OnSurfaceVariant);
    auto keeper = std::make_unique<Keeper>();
    keeper->setEnabled(false);
    keeper->setFlexGrow(1);
    auto frame = std::make_unique<Flex>();
    frame->setDirection(FlexDirection::Vertical);
    frame->setAlign(FlexAlign::Stretch);
    auto& parts = *frame;
    keeper->addChild(std::move(frame));
    m_keeper = static_cast<InputArea*>(addChild(std::move(keeper)));
    parts.addChild(
        ui::column(
            {.align = FlexAlign::Stretch,
             .gap = ColumnPreviewHeaderRow::kLineGap,
             .paddingV = Style::spaceXs,
             .paddingH = Style::spaceSm},
            makeNameRow(),
            ui::label(
                {.out = &m_kind, .color = quiet, .maxLines = 1, .ellipsize = TextEllipsize::End, .configure = literal}
            ),
            ui::label(
                {.out = &m_facts, .color = quiet, .maxLines = 1, .ellipsize = TextEllipsize::End, .configure = literal}
            ),
            ui::row(
                {.out = &m_editRow, .align = FlexAlign::Center, .gap = Style::spaceSm},
                ui::button(
                    {.out = &m_enable,
                     .text = "Enable Editing",
                     .variant = ButtonVariant::Default,
                     .paddingH = Style::spaceSm,
                     .onClick =
                         [this] {
                           if (m_bar.enableAvailable && onEnableEditing) {
                             onEnableEditing();
                           }
                         }}
                ),
                ui::button(
                    {.out = &m_save,
                     .text = "Save",
                     .variant = ButtonVariant::Primary,
                     .paddingH = Style::spaceSm,
                     .onClick =
                         [this] {
                           if (m_bar.saveAvailable && onSave) {
                             onSave();
                           }
                         }}
                ),
                ui::button(
                    {.out = &m_stop,
                     .text = "Stop Editing",
                     .variant = ButtonVariant::Default,
                     .paddingH = Style::spaceSm,
                     .onClick =
                         [this] {
                           if (m_bar.editing && onStopEditing) {
                             onStopEditing();
                           }
                         }}
                ),
                ui::label(
                    {.out = &m_unsaved,
                     .text = "Unsaved changes",
                     .color = quiet,
                     .maxLines = 1,
                     .ellipsize = TextEllipsize::End,
                     .flexGrow = 1,
                     .configure = literal}
                )
            ),
            ui::label({.out = &m_notice, .color = quiet, .maxLines = kNoticeLines, .configure = literal})
        )
    );
    parts.addChild(ui::box({.fill = colorSpecFromRole(ColorRole::OutlineVariant), .height = Style::borderWidth}));
    parts.addChild(
        ui::column(
            {.out = &m_body, .align = FlexAlign::Stretch, .clipChildren = true, .flexGrow = 1},
            ui::column(
                {.align = FlexAlign::Center, .padding = Style::spaceMd},
                ui::label(
                    {.out = &m_messageLabel,
                     .color = quiet,
                     .maxLines = kMessageLines,
                     .textAlign = TextAlign::Center,
                     .configure = literal}
                )
            )
        )
    );
    m_editRow->setWrap(true); // the unsaved mark goes under the buttons rather than being cut
    show(*m_facts, false);
    show(*m_messageLabel->parent(), false);
    applyTextSize();
    applySizeControls();
    applyEditBar();
  }

  ColumnPreviewView::~ColumnPreviewView() = default;

  std::unique_ptr<Node> ColumnPreviewView::makeNameRow() {
    const auto literal = [](Label& label) { label.setUseMarkup(false); };
    auto name = ui::label(
        {.out = &m_name,
         .fontWeight = FontWeight::SemiBold,
         .maxLines = 1,
         .ellipsize = TextEllipsize::Middle,
         .configure = literal}
    );
    auto controls = ui::row(
        {.align = FlexAlign::Center},
        ui::button(
            {.out = &m_smaller,
             .glyph = "minus",
             .variant = ButtonVariant::Ghost,
             .padding = 0,
             .onClick = [this] { choose(steppedPreviewSize(m_previewSize, -1)); }}
        ),
        ui::button(
            {.out = &m_size,
             .variant = ButtonVariant::Ghost,
             .paddingH = 0,
             .onClick = [this] { choose(kDefaultPreviewSize); }}
        ),
        ui::button(
            {.out = &m_larger, .glyph = "plus", .variant = ButtonVariant::Ghost, .padding = 0, .onClick = [this] {
               choose(steppedPreviewSize(m_previewSize, 1));
             }}
        )
    );
    auto open = ui::button(
        {.out = &m_open,
         .text = "Open",
         .variant = ButtonVariant::Default,
         .paddingH = Style::spaceSm,
         .onClick = [this] {
           if (onOpen && !m_name->text().empty()) {
             onOpen();
           }
         }}
    );
    return std::make_unique<ColumnPreviewHeaderRow>(std::move(name), std::move(controls), *m_size, std::move(open));
  }

  void ColumnPreviewView::setFile(std::string_view name, std::string_view kind) {
    m_open->setEnabled(!name.empty());
    if (name.empty()) {
      setSizeAvailable(false);
    }
    m_name->setText(name);
    m_kind->setText(kind);
  }

  void ColumnPreviewView::setKind(std::string_view kind) { m_kind->setText(kind); }

  void ColumnPreviewView::setFacts(std::string_view facts) {
    show(*m_facts, !facts.empty());
    m_facts->setText(facts);
  }

  void ColumnPreviewView::setTextSize(float size) {
    if (!std::isfinite(size) || size <= 0 || size == m_textSize) {
      return;
    }
    m_textSize = size;
    applyTextSize();
  }

  void ColumnPreviewView::applyTextSize() {
    for (auto* label : {m_name, m_kind, m_facts, m_messageLabel, m_unsaved, m_notice}) {
      label->setFontSize(m_textSize);
    }
    const float height = std::round(m_textSize * kButtonHeightPerText);
    for (auto* button : {m_open, m_smaller, m_size, m_larger, m_enable, m_save, m_stop}) {
      button->setFontSize(m_textSize);
      button->setControlHeight(height);
    }
    // The step buttons are square and the step keeps one width, so the name keeps the most room and nothing shifts.
    for (auto* button : {m_smaller, m_larger}) {
      button->setGlyphSize(m_textSize);
      button->setMinWidth(height);
      button->setMaxWidth(height);
    }
    m_size->setMinWidth(std::round(m_textSize * kSizeWidthPerText));
  }

  void ColumnPreviewView::setPreviewSize(int percent) {
    m_previewSize = steppedPreviewSize(percent, 0);
    applySizeControls();
  }

  void ColumnPreviewView::setSizeAvailable(bool available, std::string_view reason) {
    if (available == m_sizeAvailable && reason == m_sizeReason) {
      return;
    }
    m_sizeAvailable = available;
    m_sizeReason = available ? std::string() : std::string(reason);
    m_wheelRest = 0;
    applySizeControls();
  }

  bool ColumnPreviewView::wheelSize(float x, float y, double notches) {
    float localX = 0;
    float localY = 0;
    if (!m_sizeAvailable
        || m_content == nullptr
        || !Node::mapFromScene(m_body, x, y, localX, localY)
        || localX < 0
        || localY < 0
        || localX >= m_body->width()
        || localY >= m_body->height()) {
      m_wheelRest = 0;
      return false;
    }
    if (!std::isfinite(notches)) {
      return true;
    }
    const auto limit = static_cast<double>(kPreviewSizes.size());
    m_wheelRest = std::clamp(m_wheelRest - notches, -limit, limit);
    const auto steps = static_cast<int>(m_wheelRest);
    m_wheelRest -= steps;
    if (steps != 0) {
      choose(steppedPreviewSize(m_previewSize, steps));
    }
    return true;
  }

  float ColumnPreviewView::headerHeight() const {
    float x = 0;
    float y = 0;
    Node::absolutePosition(m_body, x, y);
    float top = 0;
    Node::absolutePosition(this, x, top);
    return std::max(0.0F, y - top);
  }

  void ColumnPreviewView::choose(int percent) {
    if (percent == m_previewSize) {
      return;
    }
    setPreviewSize(percent);
    if (onPreviewSizeChosen) {
      onPreviewSizeChosen(m_previewSize);
    }
  }

  void ColumnPreviewView::applySizeControls() {
    m_size->setText(std::format("{}%", m_previewSize));
    const std::array controls{
        std::tuple(m_smaller, m_previewSize != kPreviewSizes.front(), kSmaller),
        std::tuple(m_size, true, kReset),
        std::tuple(m_larger, m_previewSize != kPreviewSizes.back(), kLarger),
    };
    for (const auto& [button, usable, tooltip] : controls) {
      button->setEnabled(m_sizeAvailable && usable);
      button->setTooltip(m_sizeAvailable ? std::string_view(tooltip) : std::string_view(m_sizeReason));
      // A control disabled for a reason keeps its pointer area, so its tooltip can say why; a click does nothing.
      button->inputArea()->setEnabled(button->enabled() || (!m_sizeAvailable && !m_sizeReason.empty()));
    }
  }

  void ColumnPreviewView::setContent(std::unique_ptr<Node> content) {
    dropContent();
    m_message.clear();
    m_messageLabel->setText("");
    show(*m_messageLabel->parent(), false);
    if (content) {
      content->setFlexGrow(1);
      m_content = m_body->addChild(std::move(content));
    }
  }

  void ColumnPreviewView::showMessage(std::string message) {
    dropContent();
    m_message = std::move(message);
    m_messageLabel->setText(m_message);
    show(*m_messageLabel->parent(), !m_message.empty());
  }

  void ColumnPreviewView::clear() {
    showMessage({});
    setFile({}, {});
    setFacts({});
  }

  void ColumnPreviewView::setEditBar(const ColumnPreviewEditBar& bar) {
    if (bar == m_bar) {
      return;
    }
    m_bar = bar;
    applyEditBar();
  }

  void ColumnPreviewView::applyEditBar() {
    show(*m_editRow, m_bar.shown);
    show(*m_enable, m_bar.shown && !m_bar.editing);
    show(*m_save, m_bar.shown && m_bar.editing);
    show(*m_stop, m_bar.shown && m_bar.editing);
    // The mark keeps its place for as long as a file is edited, so the content below does not move when it comes or
    // goes.
    show(*m_unsaved, m_bar.shown && m_bar.editing);
    m_unsaved->setOpacity(m_bar.unsaved ? 1.0F : 0.0F);
    show(*m_notice, m_bar.shown && !m_bar.notice.empty());
    m_notice->setText(m_bar.notice);
    m_enable->setEnabled(m_bar.enableAvailable);
    m_enable->setTooltip(m_bar.enableAvailable ? std::string_view(kEnableTooltip) : std::string_view(m_bar.reason));
    // Disabled for a reason, the button keeps its pointer area so its tooltip can say why; a click does nothing.
    if (!m_bar.enableAvailable && !m_bar.reason.empty()) {
      m_enable->inputArea()->setEnabled(true);
    }
    m_save->setEnabled(m_bar.saveAvailable);
  }

  void ColumnPreviewView::setKeepsKeyboard(bool keeps) {
    if (keeps == m_keepsKeyboard) {
      return;
    }
    m_keepsKeyboard = keeps;
    m_keeper->setEnabled(keeps);
    // A focusable control takes the focus on a press, and a press on nothing drops it.
    for (auto* button : {m_open, m_smaller, m_size, m_larger, m_enable, m_save, m_stop}) {
      button->inputArea()->setFocusable(!keeps);
    }
  }

  void ColumnPreviewView::dropContent() {
    if (m_content != nullptr) {
      m_body->removeChild(std::exchange(m_content, nullptr)).reset();
    }
  }
} // namespace luciole
