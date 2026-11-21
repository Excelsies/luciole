#include "media/media_probe.h"

// The probe of a build without GStreamer: nothing is read and no callback runs.
struct luciole::MediaProbe::Read {};

luciole::MediaProbe::MediaProbe() = default;

luciole::MediaProbe::~MediaProbe() = default;

bool luciole::MediaProbe::available() noexcept { return false; }

bool luciole::MediaProbe::readsPending() noexcept { return false; }

// NOLINTBEGIN(performance-unnecessary-value-param): the header's signature; no callback runs in this build.
void luciole::MediaProbe::start(
    const std::string& /*path*/, std::function<void(std::optional<MediaFacts>)> /*done*/,
    std::chrono::milliseconds /*limit*/
) {}
// NOLINTEND(performance-unnecessary-value-param)

void luciole::MediaProbe::cancel() {}

bool luciole::MediaProbe::busy() const noexcept { return false; }

bool luciole::MediaProbe::timedOut() const noexcept { return false; }
