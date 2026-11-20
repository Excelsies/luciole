// Virtual pointer and keyboard client for window_check. Each stdin line is one command, answered with "ok" or
// "error <reason>" once the compositor has processed it: motion <x> <y> <x-extent> <y-extent> (a fraction of the
// output layout), button <evdev-code> <1|0>, wheel <v|h> <notches>, key <press|release|tap> <mods|-> <key> (mods is
// a comma list of ctrl, shift, alt, super) and type <hexadecimal codepoint>...
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

namespace {

  struct Globals {
    wl_seat* seat = nullptr;
    zwlr_virtual_pointer_manager_v1* pointers = nullptr;
    zwp_virtual_keyboard_manager_v1* keyboards = nullptr;
  };

  void onGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto* globals = static_cast<Globals*>(data);
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      globals->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
      globals->pointers = static_cast<zwlr_virtual_pointer_manager_v1*>(
          wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, version < 2 ? version : 2)
      );
    } else if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
      globals->keyboards = static_cast<zwp_virtual_keyboard_manager_v1*>(
          wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1)
      );
    }
  }

  constexpr wl_registry_listener kRegistryListener = {
      .global = onGlobal, .global_remove = [](void*, wl_registry*, uint32_t) {}
  };

  uint32_t now() {
    static const auto start = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return 1000 + static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
  }

  struct KeyPlace {
    uint32_t keycode = 0;
    xkb_mod_mask_t mods = 0;
  };

  class Input {
  public:
    Input(wl_display* display, const Globals& globals) : m_display(display) {
      m_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(globals.pointers, globals.seat);
      m_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(globals.keyboards, globals.seat);
    }

    // The keymap the keyboard announces; key names resolve against it.
    std::string loadKeymap() {
      m_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      m_keymap =
          m_context != nullptr ? xkb_keymap_new_from_names(m_context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
      if (m_keymap == nullptr) {
        return "cannot compile the default XKB keymap";
      }
      char* text = xkb_keymap_get_as_string(m_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
      const size_t size = std::strlen(text) + 1;
      const int fd = memfd_create("window-check-keymap", MFD_CLOEXEC);
      const bool written = fd >= 0 && write(fd, text, size) == static_cast<ssize_t>(size);
      std::free(text);
      if (!written) {
        return "cannot write the keymap";
      }
      zwp_virtual_keyboard_v1_keymap(m_keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<uint32_t>(size));
      const bool sent = wl_display_roundtrip(m_display) >= 0;
      close(fd);
      for (const auto& [name, xkbName, keysym] : kModifiers) {
        const xkb_mod_index_t index = xkb_keymap_mod_get_index(m_keymap, xkbName);
        const auto place = find(keysym);
        if (index == XKB_MOD_INVALID || index >= 32 || !place) {
          return std::string("the keymap has no usable ") + name;
        }
        m_modifierMask[name] = uint32_t{1} << index;
        m_modifierKey[uint32_t{1} << index] = place->keycode;
      }
      return sent ? "" : "connection lost while sending the keymap";
    }

    std::string run(const std::string& line) {
      std::istringstream words(line);
      std::string command;
      words >> command;
      if (command == "motion") {
        uint32_t x = 0, y = 0, width = 0, height = 0;
        if (!(words >> x >> y >> width >> height) || width == 0 || height == 0) {
          return "motion needs x y width height";
        }
        zwlr_virtual_pointer_v1_motion_absolute(m_pointer, now(), x, y, width, height);
        zwlr_virtual_pointer_v1_frame(m_pointer);
      } else if (command == "button") {
        uint32_t code = 0, pressed = 0;
        if (!(words >> code >> pressed)) {
          return "button needs a code and 1 or 0";
        }
        const auto state = pressed != 0 ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
        zwlr_virtual_pointer_v1_button(m_pointer, now(), code, state);
        zwlr_virtual_pointer_v1_frame(m_pointer);
      } else if (command == "wheel") {
        std::string axisName;
        int notches = 0;
        if (!(words >> axisName >> notches) || (axisName != "v" && axisName != "h") || notches == 0) {
          return "wheel needs v or h and a nonzero count";
        }
        const auto axis = axisName == "v" ? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
        const int step = notches < 0 ? -1 : 1;
        for (int done = 0; done != notches; done += step) {
          zwlr_virtual_pointer_v1_axis_source(m_pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
          zwlr_virtual_pointer_v1_axis_discrete(m_pointer, now(), axis, wl_fixed_from_int(15 * step), step);
          zwlr_virtual_pointer_v1_frame(m_pointer);
        }
      } else if (command == "key") {
        std::string action, mods, key;
        if (!(words >> action >> mods >> key) || (action != "press" && action != "release" && action != "tap")) {
          return "key needs press, release or tap, modifiers and a key";
        }
        return sendKey(action, mods, key);
      } else if (command == "type") {
        return typeText(words);
      } else {
        return "unknown command '" + command + "'";
      }
      return sync();
    }

  private:
    struct Modifier {
      const char* name;
      const char* xkbName;
      xkb_keysym_t keysym;
    };
    static constexpr Modifier kModifiers[] = {
        {"ctrl", XKB_MOD_NAME_CTRL, XKB_KEY_Control_L},
        {"shift", XKB_MOD_NAME_SHIFT, XKB_KEY_Shift_L},
        {"alt", XKB_MOD_NAME_ALT, XKB_KEY_Alt_L},
        {"super", XKB_MOD_NAME_LOGO, XKB_KEY_Super_L},
    };

    std::string sync() { return wl_display_roundtrip(m_display) >= 0 ? "" : "connection to the compositor lost"; }

    std::optional<KeyPlace> find(xkb_keysym_t keysym) const {
      const xkb_keycode_t last = xkb_keymap_max_keycode(m_keymap);
      for (xkb_keycode_t code = xkb_keymap_min_keycode(m_keymap); code <= last; ++code) {
        const xkb_level_index_t levels = xkb_keymap_num_levels_for_key(m_keymap, code, 0);
        for (xkb_level_index_t level = 0; level < levels; ++level) {
          const xkb_keysym_t* syms = nullptr;
          if (xkb_keymap_key_get_syms_by_level(m_keymap, code, 0, level, &syms) != 1 || syms[0] != keysym) {
            continue;
          }
          xkb_mod_mask_t mask = 0;
          if (level > 0 && xkb_keymap_key_get_mods_for_level(m_keymap, code, 0, level, &mask, 1) < 1) {
            continue;
          }
          return KeyPlace{.keycode = code - 8, .mods = mask};
        }
      }
      return std::nullopt;
    }

    std::optional<KeyPlace> resolve(const std::string& key) const {
      xkb_keysym_t keysym = XKB_KEY_NoSymbol;
      const auto first = static_cast<unsigned char>(key[0]);
      if (key.size() == 1 && first < 0x80) {
        keysym = xkb_utf32_to_keysym(first);
      } else {
        keysym = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_NO_FLAGS);
        if (keysym == XKB_KEY_NoSymbol) {
          keysym = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
        }
      }
      return keysym == XKB_KEY_NoSymbol ? std::nullopt : find(keysym);
    }

    void setMods(xkb_mod_mask_t target) {
      for (const auto& [bit, keycode] : m_modifierKey) {
        const bool want = (target & bit) != 0;
        if (want != ((m_mods & bit) != 0)) {
          zwp_virtual_keyboard_v1_key(m_keyboard, now(), keycode, want ? 1 : 0);
        }
      }
      m_mods = target;
      zwp_virtual_keyboard_v1_modifiers(m_keyboard, m_mods, 0, 0, 0);
    }

    std::string sendKey(const std::string& action, const std::string& mods, const std::string& key) {
      xkb_mod_mask_t mask = 0;
      std::istringstream names(mods == "-" ? "" : mods);
      for (std::string name; std::getline(names, name, ',');) {
        if (!m_modifierMask.contains(name)) {
          return "unknown modifier '" + name + "'";
        }
        mask |= m_modifierMask[name];
      }
      // A modifier on its own, as in keydown:shift, is held or released through the modifier state.
      if (const auto modifier = m_modifierMask.find(key); modifier != m_modifierMask.end()) {
        if (action != "release") {
          setMods(m_mods | mask | modifier->second);
        }
        if (action != "press") {
          setMods(m_mods & ~(mask | modifier->second));
        }
        return sync();
      }
      const auto place = resolve(key);
      if (!place) {
        return "the keymap has no key '" + key + "'";
      }
      if (action != "release") {
        setMods(m_mods | mask | place->mods);
        zwp_virtual_keyboard_v1_key(m_keyboard, now(), place->keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
        if (const auto error = sync(); !error.empty()) {
          return error;
        }
      }
      if (action != "press") {
        zwp_virtual_keyboard_v1_key(m_keyboard, now(), place->keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
        setMods(m_mods & ~(mask | place->mods));
      }
      return sync();
    }

    std::string typeText(std::istringstream& words) {
      for (uint32_t codepoint = 0; words >> std::hex >> codepoint;) {
        const auto place = find(xkb_utf32_to_keysym(codepoint));
        if (!place) {
          return std::format("the keymap cannot type U+{:04X}", codepoint);
        }
        const xkb_mod_mask_t held = m_mods;
        setMods(held | place->mods);
        zwp_virtual_keyboard_v1_key(m_keyboard, now(), place->keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(m_keyboard, now(), place->keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
        setMods(held);
        if (const auto error = sync(); !error.empty()) {
          return error;
        }
      }
      return words.eof() ? "" : "type needs hexadecimal codepoints";
    }

    wl_display* m_display;
    zwlr_virtual_pointer_v1* m_pointer = nullptr;
    zwp_virtual_keyboard_v1* m_keyboard = nullptr;
    xkb_context* m_context = nullptr;
    xkb_keymap* m_keymap = nullptr;
    xkb_mod_mask_t m_mods = 0;
    std::map<std::string, xkb_mod_mask_t> m_modifierMask;
    std::map<xkb_mod_mask_t, uint32_t> m_modifierKey;
  };

} // namespace

int main() {
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::cout << "error cannot connect to the private compositor" << std::endl;
    return EXIT_FAILURE;
  }
  Globals globals;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &globals);
  if (wl_display_roundtrip(display) < 0
      || globals.seat == nullptr
      || globals.pointers == nullptr
      || globals.keyboards == nullptr) {
    std::cout << "error the compositor offers no seat, virtual pointer or virtual keyboard" << std::endl;
    return EXIT_FAILURE;
  }
  Input input(display, globals);
  if (const auto error = input.loadKeymap(); !error.empty()) {
    std::cout << "error " << error << std::endl;
    return EXIT_FAILURE;
  }
  std::cout << "ready" << std::endl;
  for (std::string line; std::getline(std::cin, line);) {
    const auto error = input.run(line);
    std::cout << (error.empty() ? "ok" : "error " + error) << std::endl;
  }
  return EXIT_SUCCESS;
}
