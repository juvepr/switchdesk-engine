/**
 * @file src/platform/windows/input.cpp
 * @brief Definitions for input handling on Windows.
 *
 * Two injection backends are supported:
 *
 * - `SendInput`, the Win32 user-mode API. Always available.
 * - The Interception kernel driver, via the `interception_input` wrapper.
 *   Only usable when the driver is installed on the host.
 *
 * The driver backend is preferred where it can express the event, because it
 * writes below the user-mode input stack: games that read raw HID input see it,
 * and it is not scoped to a single desktop (so it keeps working across the
 * secure desktop, UAC prompts and the lock screen).
 *
 * It cannot express every event we need, so this is a hybrid rather than a
 * swappable backend. `abs_mouse()` and `unicode()` always use `SendInput`, and
 * any driver send that fails falls through to `SendInput` as well.
 */
#define WINVER 0x0A00

// platform includes
#include <Windows.h>

// standard includes
#include <memory>
#include <vector>

// lib includes
#include <interception_input/input.hpp>

// local includes
#include "keylayout.h"
#include "misc.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"

namespace platf {
  using namespace std::literals;

  namespace ii = interception_input;

  thread_local HDESK _lastKnownInputDesktop = nullptr;

  constexpr touch_port_t target_touch_port {
    0,
    0,
    65535,
    65535
  };

  /**
   * @brief Per-process input state.
   *
   * `driver` is null whenever the Interception backend is unavailable or
   * disabled, in which case every operation uses `SendInput`. A single instance
   * is created by `input()` at startup and lives until `freeInput()`; the
   * wrapper requires exactly one persistent driver context per instance and
   * must never be reconstructed per event.
   */
  struct input_raw_t {
    std::unique_ptr<ii::InterceptionInput> driver;
  };

  namespace {
    /**
     * @brief Human-readable name for a wrapper error, for logging.
     */
    std::string_view driver_error_name(ii::ErrorCode code) {
      switch (code) {
        case ii::ErrorCode::Ok:
          return "ok"sv;
        case ii::ErrorCode::DriverUnavailable:
          return "driver not installed"sv;
        case ii::ErrorCode::KeyboardUnavailable:
          return "no keyboard device"sv;
        case ii::ErrorCode::MouseUnavailable:
          return "no mouse device"sv;
        case ii::ErrorCode::InvalidKeyboardIndex:
          return "invalid keyboard index"sv;
        case ii::ErrorCode::InvalidMouseIndex:
          return "invalid mouse index"sv;
        case ii::ErrorCode::InvalidArgument:
          return "invalid argument"sv;
        case ii::ErrorCode::SendFailed:
          return "send failed"sv;
        case ii::ErrorCode::AlreadyShutdown:
          return "already shut down"sv;
        default:
          return "internal error"sv;
      }
    }

    /**
     * @brief Returns the driver backend if it can currently drive the keyboard.
     * @return Backend pointer, or nullptr to indicate the caller should use SendInput.
     */
    ii::InterceptionInput *keyboard_driver(input_t &input) {
      auto raw = static_cast<input_raw_t *>(input.get());
      if (!raw || !raw->driver || !raw->driver->keyboard_available()) {
        return nullptr;
      }

      return raw->driver.get();
    }

    /**
     * @brief Returns the driver backend if it can currently drive the mouse.
     * @return Backend pointer, or nullptr to indicate the caller should use SendInput.
     */
    ii::InterceptionInput *mouse_driver(input_t &input) {
      auto raw = static_cast<input_raw_t *>(input.get());
      if (!raw || !raw->driver || !raw->driver->mouse_available()) {
        return nullptr;
      }

      return raw->driver.get();
    }

    /**
     * @brief Whether a virtual-key code denotes an extended key.
     *
     * Extended keys are the E0-prefixed set. Both backends need this: SendInput
     * expresses it as KEYEVENTF_EXTENDEDKEY, the driver as KeyPrefix::E0.
     *
     * @see https://docs.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input#keystroke-message-flags
     */
    constexpr bool is_extended_vk(uint16_t modcode) {
      switch (modcode) {
        case VK_LWIN:
        case VK_RWIN:
        case VK_RMENU:
        case VK_RCONTROL:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_DIVIDE:
        case VK_APPS:
          return true;
        default:
          return false;
      }
    }

    /**
     * @brief Resolves a virtual-key code to a scancode.
     *
     * Shared by both backends so they can never disagree about which key was
     * pressed. A zero result means the key has no scancode representation, and
     * only the SendInput virtual-key path can express it.
     *
     * @param modcode The virtual-key code from the client.
     * @param flags Keyboard event flags from the client.
     * @return The scancode, or 0 if there is no mapping.
     */
    uint16_t scancode_for(uint16_t modcode, uint8_t flags) {
      // If the client did not normalize this VK code to a US English layout, we can't accurately convert it to a scancode.
      // If we're set to always send scancodes, we will use the current keyboard layout to convert to a scancode. This will
      // assume the client and host have the same keyboard layout, but it's probably better than always using US English.
      if (!(flags & SS_KBE_FLAG_NON_NORMALIZED)) {
        // Mask off the extended key byte
        return VK_TO_SCANCODE_MAP[modcode & 0xFF];
      }

      if (config::input.always_send_scancodes && modcode != VK_LWIN && modcode != VK_RWIN && modcode != VK_PAUSE) {
        // For some reason, MapVirtualKey(VK_LWIN, MAPVK_VK_TO_VSC) doesn't seem to work :/
        return (uint16_t) MapVirtualKey(modcode, MAPVK_VK_TO_VSC);
      }

      return 0;
    }
  }  // namespace

  /**
   * @brief Creates the process-wide input state, bringing up the driver backend if configured.
   */
  input_t input() {
    auto raw = std::make_unique<input_raw_t>();

    if (config::input.driver != config::input_driver_e::SENDINPUT) {
      ii::Options options {};

      // Either device on its own is worth having; whatever is missing falls
      // through to SendInput per call rather than failing construction.
      options.require_keyboard = false;
      options.require_mouse = false;

      ii::Status status {};
      raw->driver = ii::InterceptionInput::create(options, &status);

      if (raw->driver) {
        BOOST_LOG(info) << "Interception input driver active (keyboard: "sv
                        << (raw->driver->keyboard_available() ? "yes"sv : "no"sv)
                        << ", mouse: "sv
                        << (raw->driver->mouse_available() ? "yes"sv : "no"sv) << ')';
      } else if (config::input.driver == config::input_driver_e::INTERCEPTION) {
        // Explicitly requested, so a failure is a misconfiguration worth surfacing.
        BOOST_LOG(error) << "Interception input driver was requested but is unavailable ("sv
                         << driver_error_name(status.code)
                         << "); falling back to SendInput"sv;
      } else {
        BOOST_LOG(debug) << "Interception input driver unavailable ("sv
                         << driver_error_name(status.code)
                         << "); using SendInput"sv;
      }
    }

    return input_t {raw.release()};
  }

  /**
   * @brief Calls SendInput() and switches input desktops if required.
   * @param i The `INPUT` struct to send.
   */
  void send_input(INPUT &i) {
  retry:
    auto send = SendInput(1, &i, sizeof(INPUT));
    if (send != 1) {
      auto hDesk = syncThreadDesktop();
      if (_lastKnownInputDesktop != hDesk) {
        _lastKnownInputDesktop = hDesk;
        goto retry;
      }
      BOOST_LOG(error) << "Couldn't send input"sv;
    }
  }

  /**
   * @note Always uses SendInput. The wrapper does not implement absolute
   *       positioning, and the driver has no equivalent of
   *       MOUSEEVENTF_VIRTUALDESK for mapping onto a multi-monitor desktop.
   */
  void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y) {
    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags =
      MOUSEEVENTF_MOVE |
      MOUSEEVENTF_ABSOLUTE |

      // MOUSEEVENTF_VIRTUALDESK maps to the entirety of the desktop rather than the primary desktop
      MOUSEEVENTF_VIRTUALDESK;

    // Note: x and y already include the display offset (offset_x/offset_y) from client_to_touchport(),
    // so we must not add offset_x/offset_y again here to avoid double-offsetting on multi-monitor setups.
    auto scaled_x = std::lround(x * ((float) target_touch_port.width / (float) touch_port.width));
    auto scaled_y = std::lround(y * ((float) target_touch_port.height / (float) touch_port.height));

    mi.dx = scaled_x;
    mi.dy = scaled_y;

    send_input(i);
  }

  void move_mouse(input_t &input, int deltaX, int deltaY) {
    if (auto driver = mouse_driver(input); driver && driver->move_mouse_relative(deltaX, deltaY)) {
      return;
    }

    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags = MOUSEEVENTF_MOVE;
    mi.dx = deltaX;
    mi.dy = deltaY;

    send_input(i);
  }

  util::point_t get_mouse_loc(input_t &input) {
    throw std::runtime_error("not implemented yet, has to pass tests");
    // TODO: Tests are failing, something wrong here?
    POINT p;
    if (!GetCursorPos(&p)) {
      return util::point_t {0.0, 0.0};
    }

    return util::point_t {
      (double) p.x,
      (double) p.y
    };
  }

  void button_mouse(input_t &input, int button, bool release) {
    ii::MouseButton driver_button;
    if (button == 1) {
      driver_button = ii::MouseButton::Left;
    } else if (button == 2) {
      driver_button = ii::MouseButton::Middle;
    } else if (button == 3) {
      driver_button = ii::MouseButton::Right;
    } else if (button == 4) {
      driver_button = ii::MouseButton::X1;
    } else {
      driver_button = ii::MouseButton::X2;
    }

    if (auto driver = mouse_driver(input); driver && driver->set_mouse_button(driver_button, !release)) {
      return;
    }

    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    if (button == 1) {
      mi.dwFlags = release ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
    } else if (button == 2) {
      mi.dwFlags = release ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
    } else if (button == 3) {
      mi.dwFlags = release ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
    } else if (button == 4) {
      mi.dwFlags = release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
      mi.mouseData = XBUTTON1;
    } else {
      mi.dwFlags = release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
      mi.mouseData = XBUTTON2;
    }

    send_input(i);
  }

  void scroll(input_t &input, int distance) {
    if (auto driver = mouse_driver(input); driver && driver->scroll_vertical(distance)) {
      return;
    }

    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags = MOUSEEVENTF_WHEEL;
    mi.mouseData = distance;

    send_input(i);
  }

  void hscroll(input_t &input, int distance) {
    if (auto driver = mouse_driver(input); driver && driver->scroll_horizontal(distance)) {
      return;
    }

    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags = MOUSEEVENTF_HWHEEL;
    mi.mouseData = distance;

    send_input(i);
  }

  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
    const uint16_t scan_code = scancode_for(modcode, flags);
    const bool extended = is_extended_vk(modcode);

    // The driver backend takes scancodes only and cannot express a key that has
    // no scancode mapping; those keys stay on the SendInput virtual-key path.
    if (scan_code) {
      if (auto driver = keyboard_driver(input)) {
        ii::KeyStroke stroke {};
        stroke.scan_code = scan_code;
        stroke.action = release ? ii::KeyAction::Up : ii::KeyAction::Down;
        stroke.prefix = extended ? ii::KeyPrefix::E0 : ii::KeyPrefix::None;

        if (driver->send_key(stroke)) {
          return;
        }
      }
    }

    INPUT i {};
    i.type = INPUT_KEYBOARD;
    auto &ki = i.ki;

    ki.wScan = scan_code;

    // If we can map this to a scancode, send it as a scancode for maximum game compatibility.
    if (ki.wScan) {
      ki.dwFlags = KEYEVENTF_SCANCODE;
    } else {
      // If there is no scancode mapping or it's non-normalized, send it as a regular VK event.
      ki.wVk = modcode;
    }

    if (extended) {
      ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    if (release) {
      ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    send_input(i);
  }

  /**
   * @note Always uses SendInput. KEYEVENTF_UNICODE has no scancode
   *       representation, so the driver backend cannot express it at all.
   */
  void unicode(input_t &input, char *utf8, int size) {
    // We can do no worse than one UTF-16 character per byte of UTF-8
    std::vector<WCHAR> wide(size);

    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, size, wide.data(), size);
    if (chars <= 0) {
      return;
    }

    // Send all key down events
    for (int i = 0; i < chars; i++) {
      INPUT input {};
      input.type = INPUT_KEYBOARD;
      input.ki.wScan = wide[i];
      input.ki.dwFlags = KEYEVENTF_UNICODE;
      send_input(input);
    }

    // Send all key up events
    for (int i = 0; i < chars; i++) {
      INPUT input {};
      input.type = INPUT_KEYBOARD;
      input.ki.wScan = wide[i];
      input.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
      send_input(input);
    }
  }

  void freeInput(void *p) {
    auto input = (input_raw_t *) p;

    // ~InterceptionInput performs a best-effort release of anything still held
    // and tears the driver context down; no exception escapes it.
    delete input;
  }

  /**
   * @brief Returns the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t get_capabilities() {
    // No gamepad, touch, or pen support after the Phase 1 step 6 cuts.
    return 0;
  }
}  // namespace platf
