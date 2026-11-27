#include "ui/workbench/status_badge.h"

#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>

namespace luciole {
  std::unique_ptr<Flex> makeStatusBadge(StatusBadge& out) {
    auto pill = ui::row(
        {.out = &out.pill,
         .align = FlexAlign::Center,
         .paddingH = Style::spaceSm,
         .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
         .radius = kStatusBadgeHeight / 2.0F,
         .height = kStatusBadgeHeight,
         .visible = false,
         .participatesInLayout = false},
        ui::label(
            {.out = &out.label,
             .fontSize = Style::fontSizeCaption,
             .maxLines = 1,
             .configure = [](Label& label) { label.setUseMarkup(false); }}
        )
    );
    return pill;
  }

  void showStatusBadge(const StatusBadge& badge, std::string_view text, bool problem, float textSize) {
    const bool shown = !text.empty();
    badge.pill->setVisible(shown);
    badge.pill->setParticipatesInLayout(shown);
    badge.label->setText(text);
    badge.label->setFontSize(textSize);
    badge.label->setColor(colorSpecFromRole(problem ? ColorRole::Error : ColorRole::OnSurfaceVariant));
    const float height = std::round(textSize * kStatusBadgeHeight / Style::fontSizeCaption);
    badge.pill->setMinHeight(height);
    badge.pill->setMaxHeight(height);
    badge.pill->setRadius(height / 2.0F);
  }
} // namespace luciole
