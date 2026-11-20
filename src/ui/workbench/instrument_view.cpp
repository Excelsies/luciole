#include "ui/workbench/instrument_view.h"

#include "ui/workbench/atlas_part.h"
#include "ui/workbench/iris_part.h"
#include "ui/workbench/nest_part.h"

#include <memory>
#include <utility>

namespace luciole {
  InstrumentView::InstrumentView(WorkbenchInstrument kind, std::function<std::uint32_t()> modifiers)
      : PartView(std::move(modifiers)), m_kind(kind) {
    setKind(kind);
  }

  void InstrumentView::setKind(WorkbenchInstrument kind) {
    m_kind = kind;
    switch (kind) {
    case WorkbenchInstrument::Iris:
      setPart(makeIrisPart());
      return;
    case WorkbenchInstrument::Atlas:
      setPart(makeAtlasPart([this] { partReady(); }));
      return;
    case WorkbenchInstrument::Nest:
      break;
    }
    setPart(makeNestPart());
  }

  workbench::InstrumentLimits InstrumentView::limits() { return workbench::kPlanningInstrumentLimits; }
} // namespace luciole
