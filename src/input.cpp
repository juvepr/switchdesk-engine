/**
 * @file src/input.cpp
 * @brief Definitions for keyboard and mouse input handling.
 */
#include <cstdint>
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

// standard includes
#include <chrono>
#include <list>
#include <unordered_map>

// lib includes
#include <boost/endian/buffers.hpp>

// local includes
#include "config.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_pool.h"
#include "utility.h"

// Win32 WHEEL_DELTA constant
#ifndef WHEEL_DELTA
constexpr int WHEEL_DELTA = 120;
#endif

using namespace std::literals;

namespace input {

#define DISABLE_LEFT_BUTTON_DELAY ((thread_pool_util::ThreadPool::task_id_t) 0x01)
#define ENABLE_LEFT_BUTTON_DELAY nullptr

  constexpr auto VKEY_SHIFT = 0x10;
  constexpr auto VKEY_LSHIFT = 0xA0;
  constexpr auto VKEY_RSHIFT = 0xA1;
  constexpr auto VKEY_CONTROL = 0x11;
  constexpr auto VKEY_LCONTROL = 0xA2;
  constexpr auto VKEY_RCONTROL = 0xA3;
  constexpr auto VKEY_MENU = 0x12;
  constexpr auto VKEY_LMENU = 0xA4;
  constexpr auto VKEY_RMENU = 0xA5;

  enum class button_state_e {
    NONE,  ///< No button state
    DOWN,  ///< Button is down
    UP  ///< Button is up
  };

  typedef uint32_t key_press_id_t;

  key_press_id_t make_kpid(uint16_t vk, uint8_t flags) {
    return (key_press_id_t) vk << 8 | flags;
  }

  uint16_t vk_from_kpid(key_press_id_t kpid) {
    return kpid >> 8;
  }

  uint8_t flags_from_kpid(key_press_id_t kpid) {
    return kpid & 0xFF;
  }

  static task_pool_util::TaskPool::task_id_t key_press_repeat_id {};
  static std::unordered_map<key_press_id_t, bool> key_press {};
  static std::array<std::uint8_t, 5> mouse_press {};

  static platf::input_t platf_input;
  struct input_t {
    enum shortkey_e {
      CTRL = 0x1,  ///< Control key
      ALT = 0x2,  ///< Alt key
      SHIFT = 0x4,  ///< Shift key
      SHORTCUT = CTRL | ALT | SHIFT  ///< Shortcut combination
    };

    input_t(
      safe::mail_raw_t::event_t<input::touch_port_t> touch_port_event
    ):
        shortcutFlags {},
        touch_port_event {std::move(touch_port_event)},
        mouse_left_button_timeout {},
        touch_port {{0, 0, 0, 0}, 0, 0, 1.0f, 1.0f, 0, 0},
        accumulated_vscroll_delta {},
        accumulated_hscroll_delta {} {
    }

    // Keep track of alt+ctrl+shift key combo
    int shortcutFlags;

    safe::mail_raw_t::event_t<input::touch_port_t> touch_port_event;

    std::list<std::vector<uint8_t>> input_queue;
    std::mutex input_queue_lock;

    thread_pool_util::ThreadPool::task_id_t mouse_left_button_timeout;

    input::touch_port_t touch_port;

    int32_t accumulated_vscroll_delta;
    int32_t accumulated_hscroll_delta;
  };

  /**
   * @brief Apply shortcut based on VKEY
   * @param keyCode The VKEY code
   * @return 0 if no shortcut applied, > 0 if shortcut applied.
   */
  inline int apply_shortcut(short keyCode) {
    constexpr auto VK_F1 = 0x70;
    constexpr auto VK_F13 = 0x7C;

    BOOST_LOG(debug) << "Apply Shortcut: 0x"sv << util::hex((std::uint8_t) keyCode).to_string_view();

    if (keyCode >= VK_F1 && keyCode <= VK_F13) {
      mail::man->event<int>(mail::switch_display)->raise(keyCode - VK_F1);
      return 1;
    }

    switch (keyCode) {
      case 0x4E /* VKEY_N */:
        // When cursor capture is disabled in the config, the toggle is a no-op
        // so client input cannot re-enable the captured cursor.
        if (config::video.capture_cursor) {
          display_cursor = !display_cursor;
        } else {
          BOOST_LOG(debug) << "Cursor toggle shortcut ignored: cursor capture is disabled"sv;
        }
        return 1;
    }

    return 0;
  }

  void print(PNV_REL_MOUSE_MOVE_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin relative mouse move packet--"sv << std::endl
      << "deltaX ["sv << util::endian::big(packet->deltaX) << ']' << std::endl
      << "deltaY ["sv << util::endian::big(packet->deltaY) << ']' << std::endl
      << "--end relative mouse move packet--"sv;
  }

  void print(PNV_ABS_MOUSE_MOVE_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin absolute mouse move packet--"sv << std::endl
      << "x      ["sv << util::endian::big(packet->x) << ']' << std::endl
      << "y      ["sv << util::endian::big(packet->y) << ']' << std::endl
      << "width  ["sv << util::endian::big(packet->width) << ']' << std::endl
      << "height ["sv << util::endian::big(packet->height) << ']' << std::endl
      << "--end absolute mouse move packet--"sv;
  }

  void print(PNV_MOUSE_BUTTON_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse button packet--"sv << std::endl
      << "action ["sv << util::hex(packet->header.magic).to_string_view() << ']' << std::endl
      << "button ["sv << util::hex(packet->button).to_string_view() << ']' << std::endl
      << "--end mouse button packet--"sv;
  }

  void print(PNV_SCROLL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse scroll packet--"sv << std::endl
      << "scrollAmt1 ["sv << util::endian::big(packet->scrollAmt1) << ']' << std::endl
      << "--end mouse scroll packet--"sv;
  }

  void print(PSS_HSCROLL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse hscroll packet--"sv << std::endl
      << "scrollAmount ["sv << util::endian::big(packet->scrollAmount) << ']' << std::endl
      << "--end mouse hscroll packet--"sv;
  }

  void print(PNV_KEYBOARD_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin keyboard packet--"sv << std::endl
      << "keyAction ["sv << util::hex(packet->header.magic).to_string_view() << ']' << std::endl
      << "keyCode ["sv << util::hex(packet->keyCode).to_string_view() << ']' << std::endl
      << "modifiers ["sv << util::hex(packet->modifiers).to_string_view() << ']' << std::endl
      << "flags ["sv << util::hex(packet->flags).to_string_view() << ']' << std::endl
      << "--end keyboard packet--"sv;
  }

  void print(PNV_UNICODE_PACKET packet) {
    std::string text(packet->text, util::endian::big(packet->header.size) - sizeof(packet->header.magic));
    BOOST_LOG(debug)
      << "--begin unicode packet--"sv << std::endl
      << "text ["sv << text << ']' << std::endl
      << "--end unicode packet--"sv;
  }

  void print(void *payload) {
    auto header = (PNV_INPUT_HEADER) payload;

    switch (util::endian::little(header->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        print((PNV_REL_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_MOVE_ABS_MAGIC:
        print((PNV_ABS_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        print((PNV_MOUSE_BUTTON_PACKET) payload);
        break;
      case SCROLL_MAGIC_GEN5:
        print((PNV_SCROLL_PACKET) payload);
        break;
      case SS_HSCROLL_MAGIC:
        print((PSS_HSCROLL_PACKET) payload);
        break;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        print((PNV_KEYBOARD_PACKET) payload);
        break;
      case UTF8_TEXT_EVENT_MAGIC:
        print((PNV_UNICODE_PACKET) payload);
        break;
    }
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_REL_MOUSE_MOVE_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    input->mouse_left_button_timeout = DISABLE_LEFT_BUTTON_DELAY;
    platf::move_mouse(platf_input, util::endian::big(packet->deltaX), util::endian::big(packet->deltaY));
  }

  /**
   * @brief Converts client coordinates on the specified surface into screen coordinates.
   * @param input The input context.
   * @param val The cartesian coordinate pair to convert.
   * @param size The size of the client's surface containing the value.
   * @return The host-relative coordinate pair if a touchport is available.
   */
  std::optional<std::pair<float, float>> client_to_touchport(std::shared_ptr<input_t> &input, const std::pair<float, float> &val, const std::pair<float, float> &size) {
    auto &touch_port_event = input->touch_port_event;
    auto &touch_port = input->touch_port;
    if (touch_port_event->peek()) {
      touch_port = *touch_port_event->pop();
    }
    if (!touch_port) {
      BOOST_LOG(verbose) << "Ignoring early absolute input without a touch port"sv;
      return std::nullopt;
    }

    auto scalarX = touch_port.width / size.first;
    auto scalarY = touch_port.height / size.second;

    float x = std::clamp(val.first, 0.0f, size.first) * scalarX;
    float y = std::clamp(val.second, 0.0f, size.second) * scalarY;

    auto offsetX = touch_port.client_offsetX;
    auto offsetY = touch_port.client_offsetY;

    x = std::clamp(x, offsetX, (size.first * scalarX) - offsetX);
    y = std::clamp(y, offsetY, (size.second * scalarY) - offsetY);

    /*
    x and y here below have the coordinates of the surface of the streaming resolution,
    and are dependent on how that comes configured from the client (scalar_inv is calculated
    from the proportion of that and the device's **physical** size).
    */
    x = (x - offsetX) * touch_port.scalar_inv;
    y = (y - offsetY) * touch_port.scalar_inv;

    /*
    This final operation is a bit weird and has been brought about with lots of trial and error. A better
    way to do this may exist.

    Basically, this is what makes the touchscreen map to the coordinates inputtino expects properly.
    Since inputtino's dimensions are now logical (because scaling breaks everything otherwise), using the previous
    x and y coordinates would be incorrect when screens are scaled, because the touch port is smaller (or larger)
    by a factor (that factor is touch_port.scalar_tpcoords), and that factor must be used to account for that difference
    when moving the cursor. Otherwise, it will move either slower or faster than your finger proportionally to
    scalar_tpcoords, and be offset *inversely* proportionally to scalar_tpcoords. So you must account for both differences
    by multiplying and dividing.
    */
    float final_x = (x + touch_port.offset_x * touch_port.scalar_tpcoords) / touch_port.scalar_tpcoords;
    float final_y = (y + touch_port.offset_y * touch_port.scalar_tpcoords) / touch_port.scalar_tpcoords;
    return std::pair {final_x, final_y};
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_ABS_MOUSE_MOVE_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (input->mouse_left_button_timeout == DISABLE_LEFT_BUTTON_DELAY) {
      input->mouse_left_button_timeout = ENABLE_LEFT_BUTTON_DELAY;
    }

    float x = util::endian::big(packet->x);
    float y = util::endian::big(packet->y);

    // Prevent divide by zero
    // Don't expect it to happen, but just in case
    if (!packet->width || !packet->height) {
      BOOST_LOG(warning) << "Moonlight passed invalid dimensions"sv;

      return;
    }

    auto width = (float) util::endian::big(packet->width);
    auto height = (float) util::endian::big(packet->height);

    auto tpcoords = client_to_touchport(input, {x, y}, {width, height});
    if (!tpcoords) {
      return;
    }

    auto &touch_port = input->touch_port;

    int touch_port_dim_x;
    int touch_port_dim_y;
    if (touch_port.env_logical_width != 0 && touch_port.env_logical_height != 0) {
      touch_port_dim_x = touch_port.env_logical_width;
      touch_port_dim_y = touch_port.env_logical_height;
    } else {
      touch_port_dim_x = touch_port.env_width;
      touch_port_dim_y = touch_port.env_height;
    }

    platf::touch_port_t abs_port {
      touch_port.offset_x,
      touch_port.offset_y,
      touch_port_dim_x,
      touch_port_dim_y
    };

    platf::abs_mouse(platf_input, abs_port, tpcoords->first, tpcoords->second);
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_MOUSE_BUTTON_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    auto release = util::endian::little(packet->header.magic) == MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5;
    auto button = util::endian::big(packet->button);
    if (button > 0 && button < mouse_press.size()) {
      if (mouse_press[button] != release) {
        // button state is already what we want
        return;
      }

      mouse_press[button] = !release;
    }
    /**
     * When Moonlight sends mouse input through absolute coordinates,
     * it's possible that BUTTON_RIGHT is pressed down immediately after releasing BUTTON_LEFT.
     * As a result, Sunshine will left-click on hyperlinks in the browser before right-clicking
     *
     * This can be solved by delaying BUTTON_LEFT, however, any delay on input is undesirable during gaming
     * As a compromise, Sunshine will only put delays on BUTTON_LEFT when
     * absolute mouse coordinates have been sent.
     *
     * Try to make sure BUTTON_RIGHT gets called before BUTTON_LEFT is released.
     *
     * input->mouse_left_button_timeout can only be nullptr
     * when the last mouse coordinates were absolute
     */
    if (button == BUTTON_LEFT && release && !input->mouse_left_button_timeout) {
      auto f = [=]() {
        auto left_released = mouse_press[BUTTON_LEFT];
        if (left_released) {
          // Already released left button
          return;
        }
        platf::button_mouse(platf_input, BUTTON_LEFT, release);

        mouse_press[BUTTON_LEFT] = false;
        input->mouse_left_button_timeout = nullptr;
      };

      input->mouse_left_button_timeout = task_pool.pushDelayed(std::move(f), 10ms).task_id;

      return;
    }
    if (
      button == BUTTON_RIGHT && !release &&
      input->mouse_left_button_timeout > DISABLE_LEFT_BUTTON_DELAY
    ) {
      platf::button_mouse(platf_input, BUTTON_RIGHT, false);
      platf::button_mouse(platf_input, BUTTON_RIGHT, true);

      mouse_press[BUTTON_RIGHT] = false;

      return;
    }

    platf::button_mouse(platf_input, button, release);
  }

  short map_keycode(short keycode) {
    auto it = config::input.keybindings.find(keycode);
    if (it != std::end(config::input.keybindings)) {
      return it->second;
    }

    return keycode;
  }

  /**
   * @brief Update flags for keyboard shortcut combo's
   */
  inline void update_shortcutFlags(int *flags, short keyCode, bool release) {
    switch (keyCode) {
      case VKEY_SHIFT:
      case VKEY_LSHIFT:
      case VKEY_RSHIFT:
        if (release) {
          *flags &= ~input_t::SHIFT;
        } else {
          *flags |= input_t::SHIFT;
        }
        break;
      case VKEY_CONTROL:
      case VKEY_LCONTROL:
      case VKEY_RCONTROL:
        if (release) {
          *flags &= ~input_t::CTRL;
        } else {
          *flags |= input_t::CTRL;
        }
        break;
      case VKEY_MENU:
      case VKEY_LMENU:
      case VKEY_RMENU:
        if (release) {
          *flags &= ~input_t::ALT;
        } else {
          *flags |= input_t::ALT;
        }
        break;
    }
  }

  bool is_modifier(uint16_t keyCode) {
    switch (keyCode) {
      case VKEY_SHIFT:
      case VKEY_LSHIFT:
      case VKEY_RSHIFT:
      case VKEY_CONTROL:
      case VKEY_LCONTROL:
      case VKEY_RCONTROL:
      case VKEY_MENU:
      case VKEY_LMENU:
      case VKEY_RMENU:
        return true;
      default:
        return false;
    }
  }

  void send_key_and_modifiers(uint16_t key_code, bool release, uint8_t flags, uint8_t synthetic_modifiers) {
    if (!release) {
      // Press any synthetic modifiers required for this key
      if (synthetic_modifiers & MODIFIER_SHIFT) {
        platf::keyboard_update(platf_input, VKEY_SHIFT, false, flags);
      }
      if (synthetic_modifiers & MODIFIER_CTRL) {
        platf::keyboard_update(platf_input, VKEY_CONTROL, false, flags);
      }
      if (synthetic_modifiers & MODIFIER_ALT) {
        platf::keyboard_update(platf_input, VKEY_MENU, false, flags);
      }
    }

    platf::keyboard_update(platf_input, map_keycode(key_code), release, flags);

    if (!release) {
      // Raise any synthetic modifier keys we pressed
      if (synthetic_modifiers & MODIFIER_SHIFT) {
        platf::keyboard_update(platf_input, VKEY_SHIFT, true, flags);
      }
      if (synthetic_modifiers & MODIFIER_CTRL) {
        platf::keyboard_update(platf_input, VKEY_CONTROL, true, flags);
      }
      if (synthetic_modifiers & MODIFIER_ALT) {
        platf::keyboard_update(platf_input, VKEY_MENU, true, flags);
      }
    }
  }

  void repeat_key(uint16_t key_code, uint8_t flags, uint8_t synthetic_modifiers) {
    // If key no longer pressed, stop repeating
    if (!key_press[make_kpid(key_code, flags)]) {
      key_press_repeat_id = nullptr;
      return;
    }

    send_key_and_modifiers(key_code, false, flags, synthetic_modifiers);

    key_press_repeat_id = task_pool.pushDelayed(repeat_key, config::input.key_repeat_period, key_code, flags, synthetic_modifiers).task_id;
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_KEYBOARD_PACKET packet) {
    if (!config::input.keyboard) {
      return;
    }

    auto release = util::endian::little(packet->header.magic) == KEY_UP_EVENT_MAGIC;
    auto keyCode = packet->keyCode & 0x00FF;

    // Set synthetic modifier flags if the keyboard packet is requesting modifier
    // keys that are not current pressed.
    uint8_t synthetic_modifiers = 0;
    if (!release && !is_modifier(keyCode)) {
      if (!(input->shortcutFlags & input_t::SHIFT) && (packet->modifiers & MODIFIER_SHIFT)) {
        synthetic_modifiers |= MODIFIER_SHIFT;
      }
      if (!(input->shortcutFlags & input_t::CTRL) && (packet->modifiers & MODIFIER_CTRL)) {
        synthetic_modifiers |= MODIFIER_CTRL;
      }
      if (!(input->shortcutFlags & input_t::ALT) && (packet->modifiers & MODIFIER_ALT)) {
        synthetic_modifiers |= MODIFIER_ALT;
      }
    }

    auto &pressed = key_press[make_kpid(keyCode, packet->flags)];
    if (!pressed) {
      if (!release) {
        // A new key has been pressed down, we need to check for key combo's
        // If a key-combo has been pressed down, don't pass it through
        if (input->shortcutFlags == input_t::SHORTCUT && apply_shortcut(keyCode) > 0) {
          return;
        }

        if (key_press_repeat_id) {
          task_pool.cancel(key_press_repeat_id);
        }

        if (config::input.key_repeat_delay.count() > 0) {
          key_press_repeat_id = task_pool.pushDelayed(repeat_key, config::input.key_repeat_delay, keyCode, packet->flags, synthetic_modifiers).task_id;
        }
      } else {
        // Already released
        return;
      }
    } else if (!release) {
      // Already pressed down key
      return;
    }

    pressed = !release;

    send_key_and_modifiers(keyCode, release, packet->flags, synthetic_modifiers);

    update_shortcutFlags(&input->shortcutFlags, map_keycode(keyCode), release);
  }

  /**
   * @brief Called to pass a vertical scroll message the platform backend.
   * @param input The input context pointer.
   * @param packet The scroll packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PNV_SCROLL_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (config::input.high_resolution_scrolling) {
      platf::scroll(platf_input, util::endian::big(packet->scrollAmt1));
    } else {
      input->accumulated_vscroll_delta += util::endian::big(packet->scrollAmt1);
      auto full_ticks = input->accumulated_vscroll_delta / WHEEL_DELTA;
      if (full_ticks) {
        // Send any full ticks that have accumulated and store the rest
        platf::scroll(platf_input, full_ticks * WHEEL_DELTA);
        input->accumulated_vscroll_delta -= full_ticks * WHEEL_DELTA;
      }
    }
  }

  /**
   * @brief Called to pass a horizontal scroll message the platform backend.
   * @param input The input context pointer.
   * @param packet The scroll packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_HSCROLL_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (config::input.high_resolution_scrolling) {
      platf::hscroll(platf_input, util::endian::big(packet->scrollAmount));
    } else {
      input->accumulated_hscroll_delta += util::endian::big(packet->scrollAmount);
      auto full_ticks = input->accumulated_hscroll_delta / WHEEL_DELTA;
      if (full_ticks) {
        // Send any full ticks that have accumulated and store the rest
        platf::hscroll(platf_input, full_ticks * WHEEL_DELTA);
        input->accumulated_hscroll_delta -= full_ticks * WHEEL_DELTA;
      }
    }
  }

  void passthrough(PNV_UNICODE_PACKET packet) {
    if (!config::input.keyboard) {
      return;
    }

    int size = util::endian::big(packet->header.size) - sizeof(packet->header.magic);
    platf::unicode(platf_input, packet->text, size);
  }

  enum class batch_result_e {
    batched,  ///< This entry was batched with the source entry
    not_batchable,  ///< Not eligible to batch but continue attempts to batch
    terminate_batch,  ///< Stop trying to batch with this entry
  };

  /**
   * @brief Batch two relative mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_REL_MOUSE_MOVE_PACKET dest, PNV_REL_MOUSE_MOVE_PACKET src) {
    short deltaX;
    short deltaY;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (!__builtin_add_overflow(util::endian::big(dest->deltaX), util::endian::big(src->deltaX), &deltaX)) {
      return batch_result_e::terminate_batch;
    }
    if (!__builtin_add_overflow(util::endian::big(dest->deltaY), util::endian::big(src->deltaY), &deltaY)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of deltas
    dest->deltaX = util::endian::big(deltaX);
    dest->deltaY = util::endian::big(deltaY);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two absolute mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_ABS_MOUSE_MOVE_PACKET dest, PNV_ABS_MOUSE_MOVE_PACKET src) {
    // Batching must only happen if the reference width and height don't change
    if (dest->width != src->width || dest->height != src->height) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest absolute position
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two vertical scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_SCROLL_PACKET dest, PNV_SCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (!__builtin_add_overflow(util::endian::big(dest->scrollAmt1), util::endian::big(src->scrollAmt1), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmt1 = util::endian::big(scrollAmt);
    dest->scrollAmt2 = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two horizontal scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_HSCROLL_PACKET dest, PSS_HSCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (!__builtin_add_overflow(util::endian::big(dest->scrollAmount), util::endian::big(src->scrollAmount), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmount = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two input messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_INPUT_HEADER dest, PNV_INPUT_HEADER src) {
    // We can only batch if the packet types are the same
    if (dest->magic != src->magic) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch certain message types
    switch (util::endian::little(dest->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        return batch((PNV_REL_MOUSE_MOVE_PACKET) dest, (PNV_REL_MOUSE_MOVE_PACKET) src);
      case MOUSE_MOVE_ABS_MAGIC:
        return batch((PNV_ABS_MOUSE_MOVE_PACKET) dest, (PNV_ABS_MOUSE_MOVE_PACKET) src);
      case SCROLL_MAGIC_GEN5:
        return batch((PNV_SCROLL_PACKET) dest, (PNV_SCROLL_PACKET) src);
      case SS_HSCROLL_MAGIC:
        return batch((PSS_HSCROLL_PACKET) dest, (PSS_HSCROLL_PACKET) src);
      default:
        // Not a batchable message type
        return batch_result_e::terminate_batch;
    }
  }

  /**
   * @brief Called on a thread pool thread to process an input message.
   * @param input The input context pointer.
   */
  void passthrough_next_message(std::shared_ptr<input_t> input) {
    // 'entry' backs the 'payload' pointer, so they must remain in scope together
    std::vector<uint8_t> entry;
    PNV_INPUT_HEADER payload;

    // Lock the input queue while batching, but release it before sending
    // the input to the OS. This avoids potentially lengthy lock contention
    // in the control stream thread while input is being processed by the OS.
    {
      std::lock_guard<std::mutex> lg(input->input_queue_lock);

      // If all entries have already been processed, nothing to do
      if (input->input_queue.empty()) {
        return;
      }

      // Pop off the first entry, which we will send
      entry = input->input_queue.front();
      payload = (PNV_INPUT_HEADER) entry.data();
      input->input_queue.pop_front();

      // Try to batch with remaining items on the queue
      auto i = input->input_queue.begin();
      while (i != input->input_queue.end()) {
        auto batchable_entry = *i;
        auto batchable_payload = (PNV_INPUT_HEADER) batchable_entry.data();

        auto batch_result = batch(payload, batchable_payload);
        if (batch_result == batch_result_e::terminate_batch) {
          // Stop batching
          break;
        } else if (batch_result == batch_result_e::batched) {
          // Erase this entry since it was batched
          i = input->input_queue.erase(i);
        } else {
          // We couldn't batch this entry, but try to batch later entries.
          i++;
        }
      }
    }

    // Print the final input packet
    input::print((void *) payload);

    // Send the batched input to the OS
    switch (util::endian::little(payload->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        passthrough(input, (PNV_REL_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_MOVE_ABS_MAGIC:
        passthrough(input, (PNV_ABS_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        passthrough(input, (PNV_MOUSE_BUTTON_PACKET) payload);
        break;
      case SCROLL_MAGIC_GEN5:
        passthrough(input, (PNV_SCROLL_PACKET) payload);
        break;
      case SS_HSCROLL_MAGIC:
        passthrough(input, (PSS_HSCROLL_PACKET) payload);
        break;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        passthrough(input, (PNV_KEYBOARD_PACKET) payload);
        break;
      case UTF8_TEXT_EVENT_MAGIC:
        passthrough((PNV_UNICODE_PACKET) payload);
        break;
    }
  }

  /**
   * @brief Called on the control stream thread to queue an input message.
   * @param input The input context pointer.
   * @param input_data The input message.
   */
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data) {
    {
      std::lock_guard<std::mutex> lg(input->input_queue_lock);
      input->input_queue.push_back(std::move(input_data));
    }
    task_pool.push(passthrough_next_message, input);
  }

  void reset(std::shared_ptr<input_t> &input) {
    task_pool.cancel(key_press_repeat_id);
    task_pool.cancel(input->mouse_left_button_timeout);

    // Ensure input is synchronous, by using the task_pool
    task_pool.push([]() {
      for (int x = 0; x < mouse_press.size(); ++x) {
        if (mouse_press[x]) {
          platf::button_mouse(platf_input, x, true);
          mouse_press[x] = false;
        }
      }

      for (auto &kp : key_press) {
        if (!kp.second) {
          // already released
          continue;
        }
        platf::keyboard_update(platf_input, vk_from_kpid(kp.first) & 0x00FF, true, flags_from_kpid(kp.first));
        key_press[kp.first] = false;
      }
    });
  }

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() override {
      platf_input.reset();
    }
  };

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init() {
    platf_input = platf::input();

    return std::make_unique<deinit_t>();
  }

  std::shared_ptr<input_t> alloc(safe::mail_t mail) {
    auto input = std::make_shared<input_t>(
      mail->event<input::touch_port_t>(mail::touch_port)
    );

    // Workaround to ensure new frames will be captured when a client connects
    task_pool.pushDelayed([]() {
      platf::move_mouse(platf_input, 1, 1);
      platf::move_mouse(platf_input, -1, -1);
    },
                          100ms);

    return input;
  }
}  // namespace input
