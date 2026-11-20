#pragma once

#include "render/scene/node.h"

#include <memory>

namespace luciole {
  class JumpListView;

  // The body of the Jump ahead pane: the Jump ahead list, or in its place the preview of a file that has no column,
  // filling the pane below its header.
  class JumpPane final : public Node {
  public:
    JumpPane();

    JumpListView& list() const noexcept { return *m_list; }
    // The preview node the pane holds while it is shown; returns the one it replaces.
    std::unique_ptr<Node> setPreview(std::unique_ptr<Node> preview);
    // Takes the preview out and gives it back; null when there is none.
    std::unique_ptr<Node> takePreview();
    // Shows the preview in place of the list, or the list again. Ignored without a preview node.
    void showPreview(bool shown);

  protected:
    void doLayout(Renderer& renderer) override;

  private:
    Node* m_fill = nullptr;
    JumpListView* m_list = nullptr;
    Node* m_preview = nullptr;
    bool m_shown = false;
  };
} // namespace luciole
