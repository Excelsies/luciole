#include "imaging/text_tile_service.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <glib.h>
#include <map>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace luciole {
  namespace {
    // A document's queued work. Guarded by Queue::mutex.
    struct Work {
      std::shared_ptr<const formats::StyledDocument> document;
      std::uint64_t revision = 1;
      std::optional<TextTileOptions> layout; // made before any tile
      std::vector<std::size_t> tiles;        // drawn front to back
      std::uint64_t sequence = 0;            // when it was last asked for
    };

    struct TileResult {
      std::size_t index = 0;
      std::expected<TextTile, std::string> tile;
    };

    struct Outcome {
      std::uint64_t id = 0;
      std::uint64_t revision = 0;
      std::variant<std::expected<TextTileGeometry, std::string>, TileResult> payload;
    };

    // One unit of work taken from the queue.
    struct Job {
      std::uint64_t id = 0;
      std::uint64_t revision = 0;
      std::shared_ptr<const formats::StyledDocument> document;
      std::optional<TextTileOptions> layout; // a layout to make, or else
      std::size_t tile = 0;                  // the tile to draw
    };

    // A layout the worker made, with the revision it belongs to. Worker thread only.
    struct Held {
      std::uint64_t revision = 0;
      std::unique_ptr<TextTileLayout> layout;
    };
  } // namespace

  struct detail::TextTileServiceState : std::enable_shared_from_this<TextTileServiceState> {
    struct Entry {
      std::uint64_t revision = 1;
      std::shared_ptr<const TextTileEvents> events;
    };

    // Shared with the worker.
    struct Queue {
      std::mutex mutex;
      std::condition_variable wake;
      std::map<std::uint64_t, Work> work; // every open document
      std::vector<std::uint64_t> closed;  // layouts the worker frees
      std::vector<Outcome> outbox;
      std::uint64_t sequence = 0;
      bool posted = false; // a main-context delivery is pending
      bool stopping = false;
      std::weak_ptr<TextTileServiceState> owner; // set before the worker starts
    };

    std::shared_ptr<Queue> queue = std::make_shared<Queue>();
    std::thread worker;
    std::unordered_map<std::uint64_t, Entry> documents; // main context only
    std::uint64_t nextId = 1;

    void deliver();
    void shutdown();
  };

  namespace {
    using State = detail::TextTileServiceState;
    using Queue = State::Queue;

    bool hasWork(const Work& work) { return work.layout.has_value() || !work.tiles.empty(); }

    // The most recently asked document with work; caller holds the mutex.
    std::optional<Job> takeJob(Queue& queue) {
      auto best = queue.work.end();
      for (auto it = queue.work.begin(); it != queue.work.end(); ++it) {
        if (hasWork(it->second) && (best == queue.work.end() || it->second.sequence > best->second.sequence)) {
          best = it;
        }
      }
      if (best == queue.work.end()) {
        return std::nullopt;
      }
      auto& work = best->second;
      Job job{.id = best->first, .revision = work.revision, .document = work.document, .layout = {}, .tile = 0};
      if (work.layout) {
        job.layout = std::exchange(work.layout, std::nullopt);
      } else {
        job.tile = work.tiles.front();
        work.tiles.erase(work.tiles.begin());
      }
      return job;
    }

    // Schedules delivery on the default main context; caller holds the mutex.
    void post(Queue& queue) {
      if (queue.posted) {
        return;
      }
      queue.posted = true;
      GSource* source = g_idle_source_new();
      g_source_set_priority(source, G_PRIORITY_DEFAULT);
      g_source_set_callback(
          source,
          [](gpointer data) -> gboolean {
            if (const auto state = static_cast<std::weak_ptr<State>*>(data)->lock()) {
              state->deliver();
            }
            return G_SOURCE_REMOVE;
          },
          new std::weak_ptr<State>(queue.owner), [](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
      );
      g_source_attach(source, g_main_context_default());
      g_source_unref(source);
    }

    std::expected<TextTileGeometry, std::string>
    makeLayout(Job& job, std::unordered_map<std::uint64_t, Held>& layouts) {
      try {
        auto layout = std::make_unique<TextTileLayout>(job.document, *job.layout);
        TextTileGeometry geometry{
            .width = layout->width(),
            .height = layout->height(),
            .tileCount = layout->tileCount(),
            .truncated = layout->truncated(),
            .lineTops = layout->lineTops(),
        };
        layouts[job.id] = Held{.revision = job.revision, .layout = std::move(layout)};
        return geometry;
      } catch (const std::exception& error) {
        layouts.erase(job.id);
        return std::unexpected(std::string("cannot lay the text out: ") + error.what());
      }
    }

    std::expected<TextTile, std::string> drawTile(const Job& job, std::unordered_map<std::uint64_t, Held>& layouts) {
      const auto found = layouts.find(job.id);
      if (found == layouts.end() || found->second.revision != job.revision) {
        return std::unexpected(std::string("the text has no layout"));
      }
      try {
        return found->second.layout->renderTile(job.tile);
      } catch (const std::exception& error) {
        return std::unexpected(std::string("cannot draw a text tile: ") + error.what());
      }
    }

    // The worker: frees closed layouts and runs one job at a time until the service stops.
    void work(const std::shared_ptr<Queue>& queue) {
      ::pthread_setname_np(::pthread_self(), "luciole-text");
      std::unordered_map<std::uint64_t, Held> layouts;
      for (;;) {
        std::optional<Job> job;
        std::vector<std::uint64_t> closed;
        {
          std::unique_lock lock(queue->mutex);
          queue->wake.wait(lock, [&] {
            return queue->stopping
                || !queue->closed.empty()
                || (queue->outbox.size() < kTextTileOutboxLimit
                    && std::ranges::any_of(queue->work, [](const auto& w) { return hasWork(w.second); }));
          });
          if (queue->stopping) {
            return;
          }
          closed = std::exchange(queue->closed, {});
          if (queue->outbox.size() < kTextTileOutboxLimit) {
            job = takeJob(*queue);
          }
        }
        for (const auto id : closed) {
          layouts.erase(id);
        }
        if (!job) {
          continue;
        }
        Outcome outcome{.id = job->id, .revision = job->revision, .payload = {}};
        if (job->layout) {
          outcome.payload = makeLayout(*job, layouts);
        } else {
          outcome.payload = TileResult{.index = job->tile, .tile = drawTile(*job, layouts)};
        }
        const std::scoped_lock lock(queue->mutex);
        const auto open = queue->work.find(job->id);
        if (open != queue->work.end() && open->second.revision == job->revision) {
          queue->outbox.push_back(std::move(outcome));
          post(*queue);
        }
      }
    }
  } // namespace

  void detail::TextTileServiceState::deliver() {
    const auto self = shared_from_this(); // a callback may destroy the service
    std::vector<Outcome> outcomes;
    {
      const std::scoped_lock lock(queue->mutex);
      outcomes = std::exchange(queue->outbox, {});
      queue->posted = false;
    }
    queue->wake.notify_one();
    for (auto& outcome : outcomes) {
      const auto found = documents.find(outcome.id);
      if (found == documents.end() || found->second.revision != outcome.revision) {
        continue;
      }
      const auto events = found->second.events;
      if (auto* geometry = std::get_if<std::expected<TextTileGeometry, std::string>>(&outcome.payload)) {
        if (events->laidOut) {
          events->laidOut(outcome.revision, std::move(*geometry));
        }
      } else if (auto* tile = std::get_if<TileResult>(&outcome.payload); tile != nullptr && events->tile) {
        events->tile(outcome.revision, tile->index, std::move(tile->tile));
      }
    }
  }

  void detail::TextTileServiceState::shutdown() {
    {
      const std::scoped_lock lock(queue->mutex);
      queue->stopping = true;
      queue->work.clear();
      queue->outbox.clear();
    }
    queue->wake.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
    documents.clear();
  }

  TextTileService::TextTileService() : m_state(std::make_shared<detail::TextTileServiceState>()) {
    m_state->queue->owner = m_state;
    m_state->worker = std::thread([queue = m_state->queue] { work(queue); });
  }

  TextTileService::~TextTileService() { m_state->shutdown(); }

  TextTileDocument TextTileService::open(
      std::shared_ptr<const formats::StyledDocument> document, const TextTileOptions& options, TextTileEvents events
  ) {
    const auto id = m_state->nextId++;
    m_state->documents[id] = {.revision = 1, .events = std::make_shared<const TextTileEvents>(std::move(events))};
    auto& queue = *m_state->queue;
    {
      const std::scoped_lock lock(queue.mutex);
      queue.work[id] = Work{
          .document = document ? std::move(document) : std::make_shared<const formats::StyledDocument>(),
          .revision = 1,
          .layout = options,
          .tiles = {},
          .sequence = ++queue.sequence,
      };
    }
    queue.wake.notify_one();
    return {m_state, id};
  }

  TextTileDocument::TextTileDocument(std::weak_ptr<detail::TextTileServiceState> service, std::uint64_t id)
      : m_service(std::move(service)), m_id(id) {}

  TextTileDocument::~TextTileDocument() { close(); }

  TextTileDocument::TextTileDocument(TextTileDocument&& other) noexcept
      : m_service(std::move(other.m_service)), m_id(std::exchange(other.m_id, 0)) {}

  TextTileDocument& TextTileDocument::operator=(TextTileDocument&& other) noexcept {
    if (this != &other) {
      close();
      m_service = std::move(other.m_service);
      m_id = std::exchange(other.m_id, 0);
    }
    return *this;
  }

  bool TextTileDocument::valid() const {
    const auto state = m_service.lock();
    return state && state->documents.contains(m_id);
  }

  std::uint64_t TextTileDocument::revision() const {
    const auto state = m_service.lock();
    if (!state) {
      return 0;
    }
    const auto found = state->documents.find(m_id);
    return found != state->documents.end() ? found->second.revision : 0;
  }

  std::uint64_t TextTileDocument::relayout(const TextTileOptions& options) {
    const auto state = m_service.lock();
    if (!state || !state->documents.contains(m_id)) {
      return 0;
    }
    const auto revision = ++state->documents[m_id].revision;
    auto& queue = *state->queue;
    {
      const std::scoped_lock lock(queue.mutex);
      auto& work = queue.work[m_id];
      work.revision = revision;
      work.layout = options;
      work.tiles.clear();
      work.sequence = ++queue.sequence;
    }
    queue.wake.notify_one();
    return revision;
  }

  void TextTileDocument::request(std::span<const std::size_t> indices) {
    const auto state = m_service.lock();
    if (!state || !state->documents.contains(m_id)) {
      return;
    }
    auto& queue = *state->queue;
    {
      const std::scoped_lock lock(queue.mutex);
      auto& work = queue.work[m_id];
      const auto count = std::min(indices.size(), kTextTileRequestLimit);
      work.tiles.assign(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(count));
      work.sequence = ++queue.sequence;
    }
    queue.wake.notify_one();
  }

  void TextTileDocument::close() {
    const auto state = m_service.lock();
    m_service.reset();
    if (!state || state->documents.erase(m_id) == 0) {
      return;
    }
    auto& queue = *state->queue;
    {
      const std::scoped_lock lock(queue.mutex);
      queue.work.erase(m_id);
      queue.closed.push_back(m_id);
    }
    queue.wake.notify_one();
  }
} // namespace luciole
