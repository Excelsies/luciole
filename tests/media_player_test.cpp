#include "media/media_player.h"
#include "media_clips.h"
#include "test_check.h"

#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <print>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  using luciole::AudioOutput;
  using luciole::MediaFrame;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using luciole::SeekMode;
  using namespace test::media;

  struct Clips {
    fs::path video;  // 160x120 at 10 frames a second, 3 s, with sound
    fs::path tone;   // 2 s of sound only
    fs::path broken; // not media at all
    fs::path stream; // a playlist of network segments
  };

  struct Watched {
    MediaPlayer player{AudioOutput::Silent};
    int changes = 0;
    int frames = 0;
    std::optional<MediaFrame> frame;

    Watched() {
      player.setChangedCallback([this] { ++changes; });
      player.setFrameCallback([this] { ++frames; });
    }
    // Waits for the next frame and keeps it.
    bool nextFrame(Pump& pump) {
      frame.reset();
      return pump.until([this] {
        if (auto taken = player.takeFrame()) {
          frame = std::move(taken);
        }
        return frame.has_value();
      });
    }
  };

  bool reached(Pump& pump, const MediaPlayer& player, MediaState state) {
    return pump.until([&] { return player.state() == state || player.state() == MediaState::Error; })
        && player.state() == state;
  }

  // A video decoder that says it runs on a graphics device, registered at the rank automatic choice takes first.
  // It has no pads, so nothing ever picks it.
  GstPluginFeature* registerHardwareDecoder() {
    const auto classInit = [](gpointer klass, gpointer /*data*/) {
      gst_element_class_set_static_metadata(
          GST_ELEMENT_CLASS(klass), "Graphics device decoder", "Codec/Decoder/Video/Hardware", "Decodes nothing",
          "tests"
      );
    };
    const GType type = g_type_register_static_simple(
        GST_TYPE_ELEMENT, "LucioleGraphicsDeviceDecoder", sizeof(GstElementClass), classInit, sizeof(GstElement),
        nullptr, GTypeFlags{}
    );
    TEST_CHECK(gst_element_register(nullptr, "luciolegraphicsdec", GST_RANK_PRIMARY, type) == TRUE);
    GstElementFactory* factory = gst_element_factory_find("luciolegraphicsdec");
    TEST_CHECK(factory != nullptr);
    return GST_PLUGIN_FEATURE(factory);
  }

  void refusesWhatIsNotAnAbsolutePath() {
    MediaPlayer player(AudioOutput::Silent);
    TEST_CHECK(player.state() == MediaState::Idle);
    TEST_CHECK(!player.open("clip.avi"));
    TEST_CHECK(player.state() == MediaState::Error);
    TEST_CHECK(!player.error().empty());
    TEST_CHECK(!player.open(""));
    TEST_CHECK(!player.takeFrame());
    player.close();
    TEST_CHECK(player.state() == MediaState::Idle);
    TEST_CHECK(player.error().empty());
  }

  void loadsPausedWithDurationAndAFittedFrame(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    player.setFrameSize(80, 80);
    TEST_CHECK(player.open(clips.video.string()));
    TEST_CHECK(player.state() == MediaState::Loading);
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(watched.changes > 0);
    TEST_NEAR(player.duration(), 3.0, 0.05);
    TEST_NEAR(player.position(), 0.0, 0.05);
    TEST_CHECK(player.hasVideo());
    TEST_CHECK(!player.soundDeviceMissing());
    TEST_CHECK(watched.nextFrame(pump));
    // The notice reaches the main context after the frame can already be taken.
    TEST_CHECK(pump.until([&] { return watched.frames > 0; }));
    const auto& frame = *watched.frame;
    // The 4:3 source fits the 80 by 80 box as 80 by 60.
    TEST_CHECK(frame.width == 80 && frame.height == 60);
    TEST_CHECK(frame.rgba.size() == std::size_t{80} * 60 * 4);
    TEST_CHECK(frame.hold != nullptr);
    // The first bar of the color bars is a light gray, fully opaque.
    TEST_CHECK(frame.rgba[3] == 255);
    TEST_CHECK(frame.rgba[0] > 150 && std::abs(frame.rgba[0] - frame.rgba[2]) < 20);
    TEST_CHECK(!player.takeFrame()); // taken once
  }

  void playsPausesAndSeeks(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    TEST_CHECK(player.open(clips.video.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    TEST_CHECK(pump.until([&] { return player.position() >= 1.0; }));
    TEST_CHECK(player.state() == MediaState::Playing);
    player.pause();
    TEST_CHECK(player.state() == MediaState::Paused);
    pump.run(std::chrono::milliseconds(100)); // let a frame that was in flight land
    const double held = player.position();
    TEST_CHECK(held >= 1.0 && held < 2.5);
    pump.run(std::chrono::milliseconds(300));
    TEST_NEAR(player.position(), held, 0.05);

    // A seek while paused lands on the time asked and brings the frame there.
    (void)player.takeFrame();
    player.seek(2.0);
    TEST_CHECK(watched.nextFrame(pump));
    TEST_NEAR(player.position(), 2.0, 0.11);
    // A larger box never grows the frame past the source; a smaller one fits it.
    player.setFrameSize(400, 400);
    player.seek(1.0, SeekMode::KeyFrame);
    TEST_CHECK(watched.nextFrame(pump));
    TEST_CHECK(watched.frame->width == 160 && watched.frame->height == 120);
    TEST_NEAR(player.position(), 1.0, 0.11);
    player.setFrameSize(100, 100);
    player.seek(0.5);
    TEST_CHECK(watched.nextFrame(pump));
    TEST_CHECK(watched.frame->width == 100 && watched.frame->height == 75);
    // Out of range and NaN.
    player.seek(std::numeric_limits<double>::quiet_NaN());
    TEST_NEAR(player.position(), 0.5, 0.11);
    player.seek(-4);
    TEST_CHECK(watched.nextFrame(pump));
    TEST_NEAR(player.position(), 0.0, 0.11);

    // Played to the end, then again from the start.
    player.seek(2.5);
    player.play();
    TEST_CHECK(reached(pump, player, MediaState::Ended));
    TEST_NEAR(player.position(), 3.0, 0.11);
    player.play();
    TEST_CHECK(pump.until([&] { return player.state() == MediaState::Playing && player.position() < 1.0; }));
    player.close();
    TEST_CHECK(player.state() == MediaState::Idle);
    TEST_CHECK(pump.until([&] { return !holdsOpen(clips.video); }));
  }

  void keepsVolumeAndMute() {
    MediaPlayer player(AudioOutput::Silent);
    TEST_NEAR(player.volume(), 1.0, 1e-9);
    TEST_CHECK(!player.muted());
    player.setVolume(0.25);
    TEST_NEAR(player.volume(), 0.25, 1e-9);
    player.setVolume(std::numeric_limits<double>::quiet_NaN());
    TEST_NEAR(player.volume(), 0.25, 1e-9);
    player.setVolume(7);
    TEST_NEAR(player.volume(), 1.0, 1e-9);
    player.setVolume(-1);
    TEST_NEAR(player.volume(), 0.0, 1e-9);
    player.setMuted(true);
    TEST_CHECK(player.muted());
  }

  void playsSoundOnly(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    player.setVolume(0.5);
    player.setMuted(true);
    TEST_CHECK(player.open(clips.tone.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(!player.hasVideo());
    TEST_NEAR(player.duration(), 2.0, 0.05);
    player.play();
    TEST_CHECK(pump.until([&] { return player.position() >= 0.5; }));
    TEST_CHECK(watched.frames == 0);
    TEST_CHECK(!player.takeFrame());
    TEST_CHECK(player.muted());
    TEST_NEAR(player.volume(), 0.5, 1e-9);
  }

  void reportsBrokenFilesAndFreesThem(Pump& pump, const Clips& clips) {
    MediaPlayer player(AudioOutput::Silent);
    TEST_CHECK(player.open(clips.broken.string()));
    TEST_CHECK(pump.until([&] { return player.state() == MediaState::Error; }));
    TEST_CHECK(!player.error().empty());
    std::println("broken file: {}", player.error());
    TEST_CHECK(pump.until([&] { return !holdsOpen(clips.broken); }));
    // A missing file is an error as well.
    TEST_CHECK(player.open((clips.broken.parent_path() / "missing.avi").string()));
    TEST_CHECK(pump.until([&] { return player.state() == MediaState::Error; }));
  }

  void refusesNetworkMedia(Pump& pump, const fs::path& playlist) {
    if (const auto missing = missingElements({"hlsdemux2"}); !missing.empty()) {
      std::println("network refusal not checked: no {}", missing);
      return;
    }
    const Listener server;
    writeBytes(
        playlist,
        std::format(
            "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\nhttp://127.0.0.1:{}/a.ts\n"
            "#EXT-X-ENDLIST\n",
            server.port()
        )
    );
    MediaPlayer player(AudioOutput::Silent);
    TEST_CHECK(player.open(playlist.string()));
    TEST_CHECK(pump.until([&] { return player.state() == MediaState::Error; }));
    std::println("playlist: {}", player.error());
    TEST_CHECK(player.error() == "Only local files are played.");
    pump.run(std::chrono::milliseconds(500)); // time for a request that slipped through to arrive
    TEST_CHECK(!server.connected());
  }

  // A seek asked for while the file loads lands once it has loaded.
  void seeksWhileLoading(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    TEST_CHECK(player.open(clips.video.string()));
    player.seek(2.0);
    TEST_CHECK(player.state() == MediaState::Loading);
    TEST_NEAR(player.position(), 2.0, 1e-9);
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    pump.run(std::chrono::milliseconds(200)); // the seek settles
    TEST_NEAR(player.position(), 2.0, 0.11);
    TEST_CHECK(watched.nextFrame(pump));
  }

  // A FIFO and a directory end in an error at once, and the files opened after them still load.
  void refusesWhatIsNotARegularFile(Pump& pump, const Clips& clips) {
    const auto fifo = clips.broken.parent_path() / "trap.mp4";
    TEST_CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    for (const auto& path : {fifo, clips.broken.parent_path()}) {
      MediaPlayer player(AudioOutput::Silent);
      TEST_CHECK(player.open(path.string()));
      TEST_CHECK(pump.until([&] { return player.state() == MediaState::Error; }));
      TEST_CHECK(player.error() == "Only regular files are played.");
    }
    MediaPlayer player(AudioOutput::Silent);
    TEST_CHECK(player.open(clips.video.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
  }

  // Frame notices and failures reach a main context that stays busy with other work of default priority, as it is
  // while a stream of bus messages arrives.
  void noticesReachABusyMainContext(Pump& pump, const Clips& clips) {
    GSource* busy = g_idle_source_new();
    g_source_set_priority(busy, G_PRIORITY_DEFAULT);
    g_source_set_callback(busy, [](gpointer /*data*/) -> gboolean { return G_SOURCE_CONTINUE; }, nullptr, nullptr);
    (void)g_source_attach(busy, g_main_context_default());
    {
      Watched watched;
      TEST_CHECK(watched.player.open(clips.video.string()));
      TEST_CHECK(reached(pump, watched.player, MediaState::Paused));
      TEST_CHECK(pump.until([&] { return watched.frames > 0; }));
      MediaPlayer missing(AudioOutput::Silent);
      TEST_CHECK(missing.open((clips.broken.parent_path() / "missing.avi").string()));
      TEST_CHECK(pump.until([&] { return missing.state() == MediaState::Error; }));
    }
    g_source_destroy(busy);
    g_source_unref(busy);
  }

  // A box that changes while paused brings the shown frame again at the new size, where it was.
  void refitsThePausedFrame(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    player.setFrameSize(160, 160);
    TEST_CHECK(player.open(clips.video.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(watched.nextFrame(pump));
    TEST_CHECK(watched.frame->width == 160 && watched.frame->height == 120);
    player.seek(1.5);
    TEST_CHECK(watched.nextFrame(pump));
    player.setFrameSize(80, 80);
    TEST_CHECK(watched.nextFrame(pump));
    TEST_CHECK(watched.frame->width == 80 && watched.frame->height == 60);
    TEST_CHECK(player.state() == MediaState::Paused);
    TEST_NEAR(player.position(), 1.5, 0.11);
    // The same box asks for nothing.
    player.setFrameSize(80, 80);
    pump.run(std::chrono::milliseconds(300));
    TEST_CHECK(!player.takeFrame());
  }

  // With no sound output, a video plays silent and says so, and a sound file cannot be played.
  void playsSilentWithoutASoundDevice(Pump& pump, const Clips& clips) {
    breakSystemSoundOutput();

    MediaPlayer video(AudioOutput::System);
    TEST_CHECK(video.open(clips.video.string()));
    TEST_CHECK(reached(pump, video, MediaState::Paused));
    TEST_CHECK(video.hasVideo());
    TEST_CHECK(video.soundDeviceMissing());
    video.play();
    TEST_CHECK(pump.until([&] { return video.position() >= 0.5; }));
    TEST_CHECK(video.state() == MediaState::Playing);

    MediaPlayer tone(AudioOutput::System);
    TEST_CHECK(tone.open(clips.tone.string()));
    TEST_CHECK(pump.until([&] { return tone.state() == MediaState::Error; }));
    TEST_CHECK(tone.soundDeviceMissing());
    TEST_CHECK(tone.error() == "This file cannot be played: no sound device was found.");
  }

  void freesEverythingWhenDestroyedWhilePlaying(Pump& pump, const Clips& clips) {
    {
      Watched watched;
      TEST_CHECK(watched.player.open(clips.video.string()));
      watched.player.play();
      TEST_CHECK(watched.nextFrame(pump));
      TEST_CHECK(holdsOpen(clips.video));
    }
    TEST_CHECK(pump.until([&] { return !holdsOpen(clips.video); }));
    // Reopening another file frees the first.
    MediaPlayer player(AudioOutput::Silent);
    TEST_CHECK(player.open(clips.video.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(player.open(clips.tone.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(pump.until([&] { return !holdsOpen(clips.video); }));
    TEST_CHECK(holdsOpen(clips.tone));
  }
} // namespace

int main() {
  if (!MediaPlayer::available()) {
    MediaPlayer player;
    TEST_CHECK(!player.open("/clip.avi"));
    TEST_CHECK(player.state() == MediaState::Error);
    std::println("media-player: playback not built; {}", player.error());
    return 0;
  }
  Scratch scratch;
  const auto missing = missingElements(
      {"videotestsrc", "audiotestsrc", "avimux", "filesink", "capsfilter", "avidemux", "wavparse", "playbin", "appsink",
       "videoconvertscale", "fakesink"}
  );
  if (!missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  const Clips clips{
      .video = scratch.path("clip.avi"),
      .tone = scratch.path("tone.wav"),
      .broken = scratch.path("broken.avi"),
      .stream = scratch.path("stream.m3u8"),
  };
  TEST_CHECK(writeClip(clips.video, {}));
  writeTone(clips.tone, 2);
  writeBytes(clips.broken, "this is not a movie at all, only a few words of text");
  Pump pump;
  GstPluginFeature* graphicsDecoder = registerHardwareDecoder();
  TEST_CHECK(gst_plugin_feature_get_rank(graphicsDecoder) == GST_RANK_PRIMARY);
  refusesWhatIsNotAnAbsolutePath();
  keepsVolumeAndMute();
  loadsPausedWithDurationAndAFittedFrame(pump, clips);
  // Decoders on a graphics device start a context of their own that stays resident; playback lowers them below
  // automatic choice.
  TEST_CHECK(gst_plugin_feature_get_rank(graphicsDecoder) == GST_RANK_NONE);
  gst_object_unref(graphicsDecoder);
  playsPausesAndSeeks(pump, clips);
  playsSoundOnly(pump, clips);
  reportsBrokenFilesAndFreesThem(pump, clips);
  refusesNetworkMedia(pump, clips.stream);
  freesEverythingWhenDestroyedWhilePlaying(pump, clips);
  seeksWhileLoading(pump, clips);
  refusesWhatIsNotARegularFile(pump, clips);
  noticesReachABusyMainContext(pump, clips);
  refitsThePausedFrame(pump, clips);
  playsSilentWithoutASoundDevice(pump, clips);
  std::println("media-player: all checks passed");
  return 0;
}
