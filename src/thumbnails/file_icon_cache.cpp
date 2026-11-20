#include "thumbnails/file_icon_cache.h"

#include "fs/gio_interop.h"
#include "fs/gio_ptr.h"
#include "system/icon_resolver.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>
#include <unordered_map>

namespace {
  constexpr std::size_t kMaxEntries = 128;
  constexpr std::size_t kPixelBudget = 8U * 1024U * 1024U;

  std::string desktopIcon(const std::string& path) {
    if (path.empty()) {
      return {};
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
      return {};
    }
    std::array<char, 256U * 1024U + 1> bytes{};
    std::size_t count = 0;
    while (count < bytes.size()) {
      const auto read = ::read(fd, bytes.data() + count, bytes.size() - count);
      if (read <= 0) {
        break;
      }
      count += static_cast<std::size_t>(read);
    }
    ::close(fd);
    if (count == bytes.size()) {
      return {};
    }
    const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> file(g_key_file_new(), &g_key_file_unref);
    if (!g_key_file_load_from_data(file.get(), bytes.data(), count, G_KEY_FILE_NONE, nullptr)) {
      return {};
    }
    const luciole::GString icon(g_key_file_get_string(file.get(), "Desktop Entry", "Icon", nullptr));
    return icon ? icon.get() : "";
  }

  luciole::FileIconImage load(const luciole::FileIconSource& source, int size) {
    IconResolver resolver(true);
    std::vector<std::string> candidates;
    if (const auto icon = desktopIcon(source.desktopFile); !icon.empty()) {
      candidates.push_back(icon);
    }
    if (!source.filePath.empty()) {
      candidates.push_back(source.filePath);
    }
    candidates.insert(candidates.end(), source.names.begin(), source.names.end());
    for (const auto& candidate : candidates) {
      const auto& path = resolver.resolve(candidate, size);
      if (path.empty()) {
        continue;
      }
      if (auto image = loadImageFile(path, size)) {
        return {
            .pixels = std::make_shared<const LoadedImageFile>(std::move(*image)),
            .path = path,
            .symbolic = path.ends_with("-symbolic.svg") || path.ends_with("-symbolic.png"),
            .ready = true,
            .sourceKey = {},
            .revision = {}
        };
      }
    }
    return {.pixels = {}, .path = {}, .symbolic = false, .ready = true, .sourceKey = {}, .revision = {}};
  }

  std::string keyFor(const luciole::FileIconSource& source, int size) {
    std::string key = std::to_string(size);
    const auto append = [&](const std::string& part) { key += ':' + std::to_string(part.size()) + ':' + part; };
    append(source.filePath);
    append(source.desktopFile);
    for (const auto& name : source.names) {
      append(name);
    }
    return key;
  }
} // namespace

struct luciole::FileIconCache::State : std::enable_shared_from_this<State> {
  struct Item {
    std::shared_ptr<FileIconImage> image;
    std::uint64_t used = 0;
  };
  struct Request {
    std::shared_ptr<State> owner;
    FileIconSource source;
    std::shared_ptr<FileIconImage> image;
    int pixels = 0;
    std::uint64_t generation = 0;
  };
  std::unordered_map<std::string, Item> cache;
  std::deque<Request> pending;
  std::function<void()> changed;
  std::uint64_t generation = 0;
  std::uint64_t clock = 0;
  std::size_t active = 0;
  bool alive = true;
  // Jobs handed to a worker thread that have not finished their body yet.
  std::mutex runningMutex;
  std::condition_variable runningDone;
  std::size_t running = 0;

  void jobFinished() {
    const std::scoped_lock lock(runningMutex);
    --running;
    runningDone.notify_all();
  }

  void waitForJobs() {
    std::unique_lock lock(runningMutex);
    runningDone.wait(lock, [this] { return running == 0; });
  }

  void trim(bool reserveEntry) {
    std::erase_if(cache, [](const auto& item) {
      return item.second.image->ready && item.second.image.use_count() == 1;
    });
    std::size_t bytes = 0;
    for (const auto& [_, item] : cache) {
      bytes += item.image->pixels ? item.image->pixels->rgba.size() : 0;
    }
    while (cache.size() + (reserveEntry ? 1 : 0) > kMaxEntries || bytes > kPixelBudget) {
      auto oldest = cache.end();
      for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->second.image.use_count() == 1 && (oldest == cache.end() || it->second.used < oldest->second.used)) {
          oldest = it;
        }
      }
      if (oldest == cache.end()) {
        break;
      }
      bytes -= oldest->second.image->pixels ? oldest->second.image->pixels->rgba.size() : 0;
      cache.erase(oldest);
    }
  }

  void pump() {
    while (alive && active < 2 && !pending.empty()) {
      auto* request = new Request(std::move(pending.front()));
      pending.pop_front();
      ++active;
      {
        const std::scoped_lock lock(runningMutex);
        ++running;
      }
      GioPtr<GTask> task(g_task_new(
          nullptr, nullptr,
          [](GObject*, GAsyncResult* result, gpointer) {
            auto* task = G_TASK(result);
            const auto* request = static_cast<Request*>(g_task_get_task_data(task));
            const auto owner = request->owner;
            const std::unique_ptr<FileIconImage> image(
                static_cast<FileIconImage*>(g_task_propagate_pointer(task, nullptr))
            );
            --owner->active;
            if (owner->alive && owner->generation == request->generation && image) {
              image->sourceKey = "icon:" + keyFor(request->source, request->pixels);
              image->revision = "artwork-v1:" + std::to_string(request->generation);
              *request->image = std::move(*image);
              owner->trim(false);
              const auto changed = owner->changed;
              if (changed) {
                changed();
              }
            }
            owner->pump();
          },
          nullptr
      ));
      g_task_set_task_data(task.get(), request, [](gpointer data) { delete static_cast<Request*>(data); });
      g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
        const auto& request = *static_cast<Request*>(data);
        const auto owner = request.owner;
        auto image = std::make_unique<FileIconImage>();
        try {
          *image = load(request.source, request.pixels);
        } catch (const std::exception&) {
          image->ready = true;
        }
        g_task_return_pointer(task, image.release(), [](gpointer data) { delete static_cast<FileIconImage*>(data); });
        owner->jobFinished();
      });
    }
  }
};

luciole::FileIconCache::FileIconCache() : m_state(std::make_shared<State>()) {}
luciole::FileIconCache::~FileIconCache() {
  m_state->alive = false;
  m_state->changed = {};
  m_state->pending.clear();
  // Workers read process-wide icon state; none may still run once the owner is gone.
  m_state->waitForJobs();
}
void luciole::FileIconCache::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void luciole::FileIconCache::invalidate() {
  ++m_state->generation;
  m_state->pending.clear();
  m_state->cache.clear();
}
std::shared_ptr<const luciole::FileIconImage>
luciole::FileIconCache::request(const FileIconSource& source, int pixels) {
  m_state->trim(false);
  pixels = std::clamp(pixels, 1, 512);
  const auto key = keyFor(source, pixels);
  if (const auto found = m_state->cache.find(key); found != m_state->cache.end()) {
    found->second.used = ++m_state->clock;
    return found->second.image;
  }
  m_state->trim(true);
  if (m_state->cache.size() >= kMaxEntries) {
    return {};
  }
  auto image = std::make_shared<FileIconImage>();
  m_state->cache.emplace(key, State::Item{image, ++m_state->clock});
  m_state->pending.push_back({m_state, source, image, pixels, m_state->generation});
  m_state->pump();
  return image;
}

void luciole::FileIconCache::imageChanged() {
  if (m_state->alive && m_state->changed) {
    m_state->changed();
  }
}
