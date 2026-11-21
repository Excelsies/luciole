#pragma once

#include "core/timer_manager.h"
#include "media/media_player.h"
#include "media/media_probe.h"
#include "render/core/texture_handle.h"
#include "render/scene/node.h"
#include "ui/controls/audio_visual_view.h"
#include "ui/controls/media_bar_keys.h"
#include "ui/signal.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

class Button;
class Flex;
class ImageNode;
class InputArea;
class Label;
class MediaPositionBar;
struct LoadedImageFile;

// A video or audio file in a preview: the owner's poster until Play is pressed, then the decoded frames, with play
// and pause, a position bar that seeks by click and drag, elapsed and total time, and mute. A sound file without a
// cover shows a large sound glyph; while it plays, or is paused after playing, its visual takes the stage unless the
// visualizer is off, and two buttons beside Mute turn the visualizer on and off and choose the visual. A loop button
// beside Mute, for video and sound alike, makes a file that reaches its end play again from the start. In a row too
// narrow for them the times give way first, then the visual button, so the position bar keeps its room. A click on
// the picture plays or pauses, before the first Play too, and takes the keyboard; Space then toggles. Left and Right
// seek five seconds. A click on any control of the bar takes the keyboard the same way; Tab reaches the buttons, which
// Enter and Space then press. Before Play the view reads only the container's facts, off the main context. The pipeline
// stops and is freed when the view is hidden, its file changes or it is destroyed. Without playback in the build, only
// the poster shows. A sound file that fails shows the reason on the stage and the bar as before Play; any other failure
// replaces the time with its reason.
class MediaView final : public Node {
public:
  static constexpr double kSeekStep = 5;
  // How often the time and the position bar follow playback; nothing runs while paused.
  static constexpr std::chrono::milliseconds kClockInterval{250};
  // A seek in flight this long shows "Seeking..." beside the time; the watch looks every kSeekWatchInterval.
  static constexpr std::chrono::milliseconds kSeekNoticeDelay{300};
  static constexpr std::chrono::milliseconds kSeekWatchInterval{100};

  // A video that sits paused or ended this long gives its pipeline back; its last frame, time and bar stay.
  static constexpr std::chrono::milliseconds kIdleRelease{30'000};

  explicit MediaView(luciole::AudioOutput audio = luciole::AudioOutput::System);
  ~MediaView() override;

  MediaView(const MediaView&) = delete;
  MediaView& operator=(const MediaView&) = delete;
  MediaView(MediaView&&) = delete;
  MediaView& operator=(MediaView&&) = delete;

  // The absolute path of the file to play. Another path stops playback, drops the poster and reads the new file's
  // facts while the view is shown.
  void setSource(const std::string& path);
  // Shown until Play and whenever playback stops; empty shows none. Rows are width * 4 bytes of RGBA.
  void setPoster(std::shared_ptr<const LoadedImageFile> poster);
  // The owner says when the view leaves the screen: it then stops, frees its pipeline, drops a facts read and
  // releases its textures. A view whose ancestor is hidden while it plays stops the same way.
  void setShown(bool shown);
  void setTextSize(float size);
  // Whether the file plays again from the start when it ends. It shows on the loop button and applies at once to a
  // playing file and to each file played later; the owner's call does not run onLoopChosen. Off at first.
  void setLoop(bool loop);

  // Runs when facts() changed. It must not destroy the view.
  std::function<void()> onFactsChanged;
  // Runs after the loop button changed the choice, which is already applied. It must not destroy the view.
  std::function<void(bool)> onLoopChosen;

  // The player while one exists: from Play until the view stops.
  const luciole::MediaPlayer* player() const noexcept { return m_player.get(); }
  // The time limits of the first facts read and of the one more try after it timed out; for tests.
  void setProbeLimits(std::chrono::milliseconds first, std::chrono::milliseconds retry) {
    m_firstLimit = first;
    m_retryLimit = retry;
  }
  // How long a paused or ended video waits before it gives its pipeline back; for tests.
  void setIdleRelease(std::chrono::milliseconds idle) { m_idleRelease = idle; }
  // The pipeline was given back and the view shows the last frame; Play or a seek opens the file again.
  bool parked() const noexcept { return m_parked.has_value(); }
  // The details could not be read in time, also on the one more try.
  bool detailsUnread() const noexcept { return m_factsFailed; }
  // What is known of the file: its container's facts, and the playing time the player found.
  const luciole::MediaFacts& facts() const noexcept { return m_facts; }
  // Whether the picture shows a decoded frame rather than the poster.
  bool showsFrame() const noexcept { return m_showingFrame; }
  bool showsPoster() const noexcept { return !m_showingFrame && m_posterTexture.valid(); }
  // Whether the stage shows the sound glyph: a file with sound only, no cover, no error and no visual.
  bool showsSoundGlyph() const noexcept { return m_visual->showsGlyph(); }
  // The pixel size of the frame shown; zeros while none shows.
  std::pair<int, int> framePixels() const noexcept {
    return m_showingFrame ? std::pair(m_frameTexture.width, m_frameTexture.height) : std::pair(0, 0);
  }
  Button& playButton() const noexcept { return *m_play; }
  Button& muteButton() const noexcept { return *m_mute; }
  Button& loopButton() const noexcept { return *m_loopButton; }
  bool loop() const noexcept { return m_looping; }
  MediaPositionBar& positionBar() const noexcept { return *m_bar; }
  InputArea& stageArea() const noexcept { return *m_stageArea; }
  const Label& elapsedLabel() const noexcept { return *m_elapsed; }
  const Label& totalLabel() const noexcept { return *m_total; }
  // An error in place of the time, or the note that no sound device was found beside it.
  const Label& messageLabel() const noexcept { return *m_message; }
  // The reason a sound file failed, in the middle of the stage where its glyph was; hidden otherwise.
  const Label& stageNotice() const noexcept { return *m_notice; }
  // The sound visualizer: its choice, its two buttons and whether it takes the stage.
  AudioVisualView& audioVisual() const noexcept { return *m_visual; }

private:
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;

  void togglePlay();
  void onKey(const InputArea::KeyData& key);
  bool openPlayer();
  void start();
  void stop();
  bool idleVideo() const;
  void watchIdle(bool restart);
  void park();
  bool wake();
  double knownDuration() const;
  void seekBy(double seconds);
  void scrub(double fraction, luciole::SeekMode mode, bool done);
  void playerChanged();
  void refreshControls();
  void refreshMessage(bool failed);
  void refreshNotice(bool shown);
  void fitTimes(Renderer& renderer, float width);
  void showTimes();
  void refreshPosition();
  void watchSeek();
  void seekTick();
  void clearSeek();
  void followClock();
  void readFacts();
  void factsArrived(std::optional<luciole::MediaFacts> facts);
  void scheduleCatchUp();
  void catchUp();
  bool onScreen() const;
  bool soundOnly() const;
  void uploadPoster();
  void showFrame(const luciole::MediaFrame& frame);
  void showPoster();
  void releaseFrame();
  void releasePoster();

  luciole::AudioOutput m_audio;
  MediaBarKeys m_barKeys;
  std::string m_path;
  std::shared_ptr<const LoadedImageFile> m_poster;
  std::unique_ptr<luciole::MediaPlayer> m_player;
  luciole::MediaProbe m_probe;
  luciole::MediaFacts m_facts;
  ImageNode* m_picture = nullptr;
  AudioVisualView* m_visual = nullptr;
  InputArea* m_stageArea = nullptr;
  Flex* m_controls = nullptr;
  Button* m_play = nullptr;
  Label* m_elapsed = nullptr;
  MediaPositionBar* m_bar = nullptr;
  Label* m_total = nullptr;
  Label* m_message = nullptr;
  Label* m_notice = nullptr;
  Button* m_mute = nullptr;
  Button* m_loopButton = nullptr;
  Renderer* m_renderer = nullptr;
  TextureHandle m_posterTexture;
  TextureHandle m_frameTexture;
  std::pair<int, int> m_stagePixels{0, 0};
  std::pair<int, int> m_askedPixels{-1, -1};
  Timer m_clock;     // follows playback while it plays
  Timer m_seekWatch; // runs while a seek is in flight
  std::chrono::steady_clock::time_point m_seekSince;
  bool m_seekShown = false; // the seek note shows
  Timer m_catchUp;          // the work a frame notice, a poster or a new size leaves for the next turn of the main loop
  float m_textSize;
  double m_dragSeconds = 0;
  long long m_shownSecond = -1;
  bool m_factsRead = false;    // a facts read finished for this path
  bool m_factsRetried = false; // the first read timed out and the one more try was made
  bool m_factsFailed = false;  // the one more try timed out too: the details stay unknown
  std::chrono::milliseconds m_firstLimit{luciole::MediaProbe::kTimeLimit};
  std::chrono::milliseconds m_retryLimit{luciole::MediaProbe::kRetryLimit};
  // Where the file stood when its pipeline was given back, until the player that took its place has loaded.
  struct Parked {
    double at = 0;
    double duration = 0;
    bool ended = false;
  };
  std::optional<Parked> m_parked;
  Timer m_idle; // waits kIdleRelease after the video paused or ended
  std::chrono::milliseconds m_idleRelease = kIdleRelease;
  bool m_shown = true;
  bool m_muted = false;
  bool m_looping = false;
  bool m_timesWanted = true;   // the elapsed and total time show unless the file failed
  bool m_timesFit = true;      // the row has room for them beside a usable position bar
  bool m_choiceWanted = false; // a sound file shows the visual button, unless it failed
  bool m_choiceFit = true;     // the row has room for it once the times are gone
  bool m_dragging = false;
  bool m_playRequested = false; // Play was asked for and not paused since
  bool m_posterPending = false;
  bool m_showingFrame = false;
  Signal<>::ScopedConnection m_paletteConn;
};
