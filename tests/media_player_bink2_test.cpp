#include "bink2_sample.h"
#include "media/media_player.h"
#include "media/media_probe.h"
#include "media_clips.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <print>
#include <vector>

// The Bink 2 player through the ordinary MediaPlayer: open, first frame, play, seeks, loop, end, resizing, damaged
// files. They play a real file named by LUCIOLE_BINK2_SAMPLE and skip without one. The sound output is the silent one.
namespace {
  using luciole::AudioOutput;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using luciole::SeekMode;
  using namespace test::media;
  using namespace std::chrono_literals;
  namespace bink = test::bink2;

  // What the container header and index say, read here independently of the player.
  struct Facts {
    std::uint32_t frames = 0;
    double fps = 0;
    std::vector<std::uint32_t> keys;
    double duration() const { return frames / fps; }
    double timeOf(std::uint32_t frame) const { return frame / fps; }
  };

  Facts factsOf(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> head(4096);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    TEST_CHECK(in.gcount() == static_cast<std::streamsize>(head.size()));
    const auto le32 = [&](std::size_t at) {
      return static_cast<std::uint32_t>(head[at])
          | static_cast<std::uint32_t>(head[at + 1]) << 8U
          | static_cast<std::uint32_t>(head[at + 2]) << 16U
          | static_cast<std::uint32_t>(head[at + 3]) << 24U;
    };
    Facts facts;
    facts.frames = le32(8);
    facts.fps = static_cast<double>(le32(28)) / le32(32);
    const std::uint32_t tracks = le32(40);
    const std::size_t index = 44 + 4 + 12UL * tracks;
    std::vector<std::uint8_t> table(4UL * facts.frames + 4);
    in.seekg(static_cast<std::streamoff>(index));
    in.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()));
    TEST_CHECK(in.gcount() == static_cast<std::streamsize>(table.size()));
    for (std::uint32_t frame = 0; frame < facts.frames; ++frame) {
      if ((table[4UL * frame] & 1U) != 0) {
        facts.keys.push_back(frame);
      }
    }
    return facts;
  }

  // Whether the container header lists a sound track.
  bool factsHaveSound(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::array<std::uint8_t, 44> head{};
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    return head[40] != 0;
  }

  // A seek far into a long run of frames decodes all of them first, and a Play asked meanwhile waits behind it.
  constexpr auto kLongWait = 150s;

  bool reached(Pump& pump, const MediaPlayer& player, MediaState state, std::chrono::milliseconds wait = 20s) {
    return pump.until([&] { return player.state() == state || player.state() == MediaState::Error; }, wait)
        && player.state() == state;
  }

  // Waits for the next frame at `width` wide. A seek decodes up to a few hundred 4K frames before it, so the wait
  // is long.
  bool nextFrame(Pump& pump, MediaPlayer& player, int width) {
    return pump.until(
        [&] {
          if (auto frame = player.takeFrame()) {
            return frame->width == width;
          }
          return false;
        },
        120s
    );
  }

  constexpr int kBoxWidth = 480;

  void opensAtTheFirstFrame(Pump& pump, const fs::path& path, const Facts& facts) {
    MediaPlayer player(AudioOutput::Silent);
    player.setFrameSize(kBoxWidth, 270);
    TEST_CHECK(player.open(path.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(player.hasVideo());
    TEST_NEAR(player.duration(), facts.duration(), 0.002);
    TEST_NEAR(player.position(), 0.0, 0.001);
    TEST_CHECK(nextFrame(pump, player, kBoxWidth));
    player.close();
  }

  // The probe, which the preview reads before Play, finds the picture size, the playing time and the sound.
  void theProbeReadsTheFacts(Pump& pump, const fs::path& path, const Facts& facts, bool sound) {
    luciole::MediaProbe probe;
    std::optional<luciole::MediaFacts> found;
    bool done = false;
    probe.start(path.string(), [&](std::optional<luciole::MediaFacts> result) {
      found = result;
      done = true;
    });
    TEST_CHECK(pump.until([&] { return done; }));
    TEST_CHECK(found.has_value());
    TEST_CHECK(found->video && found->width > 0 && found->height > 0);
    TEST_CHECK(found->audio == sound);
    TEST_NEAR(found->seconds, facts.duration(), 0.002);
  }

  void playsAndPauses(Pump& pump, const fs::path& path, const Facts& facts) {
    MediaPlayer player(AudioOutput::Silent);
    player.setFrameSize(kBoxWidth, 270);
    TEST_CHECK(player.open(path.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    const double goal = std::min(0.5, facts.duration() / 2);
    TEST_CHECK(pump.until([&] { return player.position() >= goal || player.state() == MediaState::Error; }));
    TEST_CHECK(player.state() == MediaState::Playing);
    player.pause();
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    const double stopped = player.position();
    pump.run(300ms);
    TEST_NEAR(player.position(), stopped, 0.02);
  }

  void seeksExactly(Pump& pump, const fs::path& path, const Facts& facts) {
    MediaPlayer player(AudioOutput::Silent);
    player.setFrameSize(kBoxWidth, 270);
    TEST_CHECK(player.open(path.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(nextFrame(pump, player, kBoxWidth));
    // A frame in the middle of the longest run between key frames, so the seek decodes forward.
    std::uint32_t from = 0;
    std::uint32_t span = 0;
    for (std::size_t i = 0; i < facts.keys.size(); ++i) {
      const std::uint32_t end = i + 1 < facts.keys.size() ? facts.keys[i + 1] : facts.frames;
      if (end - facts.keys[i] > span) {
        span = end - facts.keys[i];
        from = facts.keys[i];
      }
    }
    const std::uint32_t frame = from + span * 3 / 4;
    const double wanted = facts.timeOf(frame) + 0.3 / facts.fps;
    player.seek(wanted, SeekMode::Exact);
    TEST_CHECK(nextFrame(pump, player, kBoxWidth));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_NEAR(player.position(), facts.timeOf(frame), 1.0 / facts.fps);
    // Back to the start, and forward again in the same run: the held picture is the reference.
    player.seek(0.0, SeekMode::Exact);
    TEST_CHECK(nextFrame(pump, player, kBoxWidth));
    TEST_NEAR(player.position(), 0.0, 0.001);
    player.seek(facts.timeOf(frame + 1), SeekMode::Exact);
    TEST_CHECK(nextFrame(pump, player, kBoxWidth));
    TEST_NEAR(player.position(), facts.timeOf(frame + 1), 1.0 / facts.fps);
    // The frame already shown, again.
    player.seek(facts.timeOf(frame + 1), SeekMode::Exact);
    TEST_CHECK(nextFrame(pump, player, kBoxWidth));
    TEST_NEAR(player.position(), facts.timeOf(frame + 1), 1.0 / facts.fps);
  }

  void keyFrameSeekLandsOnAKeyFrame(Pump& pump, const fs::path& path, const Facts& facts) {
    MediaPlayer player(AudioOutput::Silent);
    player.setFrameSize(kBoxWidth, 270);
    TEST_CHECK(player.open(path.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    const auto lands = [&](double wanted) {
      player.seek(wanted, SeekMode::KeyFrame);
      TEST_CHECK(nextFrame(pump, player, kBoxWidth));
      // The player reports the asked time until the seek has settled.
      pump.run(400ms);
      const double at = player.position();
      return std::ranges::any_of(facts.keys, [&](std::uint32_t key) {
        return std::abs(facts.timeOf(key) - at) < 0.0005;
      });
    };
    // A wish in the middle of the file, and one just after a key frame.
    TEST_CHECK(lands(facts.duration() * 0.55));
    TEST_CHECK(lands(facts.timeOf(facts.keys.back()) + 0.2 / facts.fps));
    TEST_CHECK(lands(0.0));
  }

  void loopsAndEnds(Pump& pump, const fs::path& path, const Facts& facts) {
    const double nearEnd = facts.timeOf(facts.frames - 1) - 0.05;
    {
      MediaPlayer player(AudioOutput::Silent);
      player.setFrameSize(kBoxWidth, 270);
      player.setLoop(true);
      TEST_CHECK(player.open(path.string()));
      TEST_CHECK(reached(pump, player, MediaState::Paused));
      // The first picture is taken first, so the next one is the seek's.
      TEST_CHECK(nextFrame(pump, player, kBoxWidth));
      player.seek(std::max(0.0, nearEnd), SeekMode::Exact);
      TEST_CHECK(nextFrame(pump, player, kBoxWidth));
      player.play();
      // The end arrives and the file starts again: the position comes back below where it was and it keeps playing.
      TEST_CHECK(pump.until(
          [&] { return player.position() < nearEnd - 0.2 || player.state() == MediaState::Error; }, kLongWait
      ));
      TEST_CHECK(player.state() == MediaState::Playing);
      TEST_CHECK(pump.until([&] { return player.position() > 0.05 || player.state() == MediaState::Error; }));
    }
    MediaPlayer player(AudioOutput::Silent);
    player.setFrameSize(kBoxWidth, 270);
    TEST_CHECK(player.open(path.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(nextFrame(pump, player, kBoxWidth));
    player.seek(std::max(0.0, nearEnd), SeekMode::Exact);
    TEST_CHECK(nextFrame(pump, player, kBoxWidth));
    player.play();
    TEST_CHECK(reached(pump, player, MediaState::Ended, kLongWait));
    TEST_NEAR(player.position(), facts.duration(), 0.1);
    // Play from the end starts again.
    player.play();
    TEST_CHECK(pump.until([&] { return player.position() < 0.2 && player.state() == MediaState::Playing; }));
  }

  void resizesWhilePausedAndEnded(Pump& pump, const fs::path& path, const Facts& facts) {
    MediaPlayer player(AudioOutput::Silent);
    player.setFrameSize(kBoxWidth, 270);
    TEST_CHECK(player.open(path.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(nextFrame(pump, player, kBoxWidth));
    player.setFrameSize(320, 180);
    TEST_CHECK(nextFrame(pump, player, 320));
    player.seek(std::max(0.0, facts.timeOf(facts.frames - 1) - 0.05), SeekMode::Exact);
    player.play();
    TEST_CHECK(reached(pump, player, MediaState::Ended, kLongWait));
    player.setFrameSize(400, 225);
    TEST_CHECK(nextFrame(pump, player, 400));
  }

  // Half the file, or its first 6 MiB, in a private folder: the player meets a file that ends in the middle of a frame.
  void aFileThatEndsEarlyEndsInAnError(Pump& pump, const fs::path& path, const fs::path& folder) {
    const fs::path cut = folder / "cut.bik";
    {
      std::ifstream in(path, std::ios::binary);
      std::error_code error;
      std::vector<char> bytes(std::min<std::uintmax_t>(fs::file_size(path, error) / 2, 6 * 1024 * 1024));
      in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      bytes.resize(static_cast<std::size_t>(in.gcount()));
      std::ofstream(cut, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    MediaPlayer player(AudioOutput::Silent);
    player.setFrameSize(kBoxWidth, 270);
    TEST_CHECK(player.open(cut.string()));
    // The picture the header and the first frames give may show; playing on reaches the cut.
    if (reached(pump, player, MediaState::Paused)) {
      player.play();
    }
    TEST_CHECK(pump.until([&] { return player.state() == MediaState::Error || player.state() == MediaState::Ended; }));
    TEST_CHECK(player.state() == MediaState::Error);
    TEST_CHECK(!player.error().empty());
    std::println("a cut file: {}", player.error());
  }

} // namespace

int main() {
  if (!MediaPlayer::available()) {
    std::println("media-player-bink2: playback not built");
    return 0;
  }
  Scratch scratch;
  Pump pump;
  const auto sample = bink::sampleFrom(bink::kSampleVariable);
  const fs::path folder = scratch.path("damaged").parent_path();
  const auto& path = bink::needSample(sample, bink::kSampleVariable);
  const Facts facts = factsOf(path);
  std::println("sample: {} frames at {} fps, {} key frames", facts.frames, facts.fps, facts.keys.size());
  theProbeReadsTheFacts(pump, path, facts, factsHaveSound(path));
  opensAtTheFirstFrame(pump, path, facts);
  playsAndPauses(pump, path, facts);
  seeksExactly(pump, path, facts);
  keyFrameSeekLandsOnAKeyFrame(pump, path, facts);
  loopsAndEnds(pump, path, facts);
  resizesWhilePausedAndEnded(pump, path, facts);
  aFileThatEndsEarlyEndsInAnError(pump, path, folder);
  std::println("media player bink2 checks passed");
  return 0;
}
