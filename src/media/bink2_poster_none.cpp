#include "media/bink2_poster.h"

#include <print>

int luciole::media::runBink2PosterWorker(std::span<const std::string_view> /*arguments*/) {
  std::println(stderr, "Playback is not available in this build.");
  return 1;
}

int luciole::media::runBink2FactsWorker(std::span<const std::string_view> /*arguments*/) {
  std::println(stderr, "Playback is not available in this build.");
  return 1;
}
