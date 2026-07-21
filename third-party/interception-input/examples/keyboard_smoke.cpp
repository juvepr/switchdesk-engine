#include <interception_input/input.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace
{

using interception_input::ErrorCode;
using interception_input::InterceptionInput;
using interception_input::KeyAction;
using interception_input::KeyPrefix;
using interception_input::KeyStroke;
using interception_input::Status;

// Set 1 make codes; no virtual-key translation anywhere in this project.
constexpr std::uint16_t scan_a = 0x1E;
constexpr std::uint16_t scan_b = 0x30;
constexpr std::uint16_t scan_c = 0x2E;
constexpr std::uint16_t scan_left_shift = 0x2A;
constexpr std::uint16_t scan_right_arrow = 0x4D; // requires E0 prefix

const char* error_name(ErrorCode code)
{
    switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::DriverUnavailable: return "DriverUnavailable";
    case ErrorCode::KeyboardUnavailable: return "KeyboardUnavailable";
    case ErrorCode::MouseUnavailable: return "MouseUnavailable";
    case ErrorCode::InvalidKeyboardIndex: return "InvalidKeyboardIndex";
    case ErrorCode::InvalidMouseIndex: return "InvalidMouseIndex";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::SendFailed: return "SendFailed";
    case ErrorCode::AlreadyShutdown: return "AlreadyShutdown";
    case ErrorCode::InternalError: return "InternalError";
    }
    return "Unknown";
}

bool report(const char* action, Status status)
{
    std::printf("  %-28s -> %s\n", action, error_name(status.code));
    return status.ok();
}

bool send_key_event(
    InterceptionInput& input,
    const char* label,
    std::uint16_t scan_code,
    KeyAction action,
    KeyPrefix prefix = KeyPrefix::None
)
{
    KeyStroke stroke {};
    stroke.scan_code = scan_code;
    stroke.action = action;
    stroke.prefix = prefix;

    return report(label, input.send_key(stroke));
}

bool tap_key(
    InterceptionInput& input,
    const char* label_down,
    const char* label_up,
    std::uint16_t scan_code,
    KeyPrefix prefix = KeyPrefix::None
)
{
    const bool down_ok =
        send_key_event(input, label_down, scan_code, KeyAction::Down, prefix);
    const bool up_ok =
        send_key_event(input, label_up, scan_code, KeyAction::Up, prefix);

    return down_ok && up_ok;
}

} // namespace

int main()
{
    std::puts("keyboard_smoke");
    std::puts("Focus Notepad (or another text field) now.");
    std::puts("Sending keystrokes in 3 seconds...");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    interception_input::Options options;
    options.require_keyboard = true;
    options.require_mouse = false;

    Status status;
    const auto input = InterceptionInput::create(options, &status);

    if (input == nullptr) {
        std::printf("create() failed: %s\n", error_name(status.code));
        std::puts("Is the Interception driver installed and a keyboard attached?");
        return 1;
    }

    std::printf(
        "created; keyboard_available=%s mouse_available=%s\n",
        input->keyboard_available() ? "true" : "false",
        input->mouse_available() ? "true" : "false"
    );

    bool all_ok = true;

    all_ok = tap_key(*input, "A down", "A up", scan_a) && all_ok;
    all_ok = tap_key(*input, "B down", "B up", scan_b) && all_ok;
    all_ok = tap_key(*input, "C down", "C up", scan_c) && all_ok;

    all_ok = tap_key(
        *input,
        "Right Arrow (E0) down",
        "Right Arrow (E0) up",
        scan_right_arrow,
        KeyPrefix::E0
    ) && all_ok;

    // Shift+A with explicit down/up events.
    all_ok = send_key_event(
        *input, "Left Shift down", scan_left_shift, KeyAction::Down) && all_ok;
    all_ok = send_key_event(
        *input, "A down (shifted)", scan_a, KeyAction::Down) && all_ok;
    all_ok = send_key_event(
        *input, "A up (shifted)", scan_a, KeyAction::Up) && all_ok;
    all_ok = send_key_event(
        *input, "Left Shift up", scan_left_shift, KeyAction::Up) && all_ok;

    std::printf(
        "keyboard_smoke %s\n",
        all_ok ? "completed successfully" : "finished with errors"
    );
    return all_ok ? 0 : 1;
}
