#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Button;
class Label;

struct PaneHeaderOptions {
  std::string name;           // the pane's name in button tooltips
  bool expandable = false;    // offers Expand, which turns into Return while the pane is expanded
  bool collapsesDown = false; // the pane sits at the bottom of its column and folds downward
};

// A small glyph button with a tooltip, styled like the header's own.
std::unique_ptr<Button>
paneHeaderButton(std::string_view glyph, std::string_view tooltip, std::function<void()> onClick);

// The title of a pane with its reason when the view closed it by force: "Jump Ahead (closed: too short)". An empty
// reason gives the title as it is.
std::string closedPaneTitle(std::string_view title, std::string_view reason);

// The bar on top of a pane: its title, the owner's controls, then Expand and Collapse. A pane the view closed by
// force says why in its title and cannot be restored by hand.
class PaneHeader final : public Flex {
public:
  static constexpr float kHeight = 24;

  explicit PaneHeader(PaneHeaderOptions options);

  // The bar's height: kHeight unless the owner's text size asks for more.
  void setHeight(float height);
  void setTitle(std::string_view title);
  // Placed before the pane's own buttons, replacing earlier controls; an empty pointer removes them.
  Node* setControls(std::unique_ptr<Node> controls);
  void setCollapsed(bool collapsed);
  void setExpanded(bool expanded);
  // Why the view closed the pane, such as "too short"; empty while the pane is not closed by force.
  void setClosedReason(std::string_view reason);
  void setOnCollapse(std::function<void()> callback);
  void setOnExpand(std::function<void()> callback);

  Label* titleLabel() const noexcept { return m_titleLabel; }
  Button* collapseButton() const noexcept { return m_collapse; }
  Button* expandButton() const noexcept { return m_expand; } // empty for a pane that cannot expand

private:
  void refresh();

  PaneHeaderOptions m_options;
  std::string m_title;
  std::string m_reason;
  Label* m_titleLabel = nullptr;
  Node* m_controls = nullptr;
  Button* m_expand = nullptr;
  Button* m_collapse = nullptr;
  std::function<void()> m_onCollapse;
  std::function<void()> m_onExpand;
  bool m_collapsed = false;
  bool m_expanded = false;
};
