#include "ui/workbench/jump_pane.h"

#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/workbench/jump_list_view.h"

#include <utility>

namespace luciole {
  JumpPane::JumpPane() {
    auto fill = std::make_unique<Box>();
    fill->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    m_fill = addChild(std::move(fill));
    m_list = static_cast<JumpListView*>(addChild(std::make_unique<JumpListView>()));
  }

  std::unique_ptr<Node> JumpPane::setPreview(std::unique_ptr<Node> preview) {
    auto previous = takePreview();
    if (preview) {
      preview->setVisible(false);
      m_preview = addChild(std::move(preview));
    }
    markLayoutDirty();
    return previous;
  }

  std::unique_ptr<Node> JumpPane::takePreview() {
    if (m_preview == nullptr) {
      return nullptr;
    }
    showPreview(false);
    return removeChild(std::exchange(m_preview, nullptr));
  }

  void JumpPane::showPreview(bool shown) {
    const bool wanted = shown && m_preview != nullptr;
    if (wanted == m_shown) {
      return;
    }
    m_shown = wanted;
    m_list->setVisible(!wanted);
    if (m_preview != nullptr) {
      m_preview->setVisible(wanted);
    }
    markLayoutDirty();
  }

  void JumpPane::doLayout(Renderer& renderer) {
    const LayoutRect rect{0, 0, width(), height()};
    m_fill->arrange(renderer, rect);
    if (m_shown) {
      m_preview->arrange(renderer, rect);
    } else {
      m_list->arrange(renderer, rect);
    }
  }
} // namespace luciole
