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
using interception_input::KeyStroke;
using interception_input::Status;

constexpr std::uint16_t scan_w = 0x11;
constexpr std::uint16_t scan_left_shift = 0x2A;

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

bool send_key_down(
    InterceptionInput& input,
    const char* label,
    std::uint16_t scan_code
)
{
    KeyStroke stroke {};
    stroke.scan_code = scan_code;
    stroke.action = KeyAction::Down;

    return report(label, input.send_key(stroke));
}

} // namespace

int main()
{
    std::puts("release_all_smoke");
    std::puts("Focus a text field now: W and Left Shift will be held down,");
    std::puts("then released via release_all().");
    std::puts("Starting in 3 seconds...");
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

    bool all_ok = true;

    all_ok = send_key_down(*input, "W down", scan_w) && all_ok;
    all_ok = send_key_down(*input, "Left Shift down", scan_left_shift) && all_ok;

    std::puts("Holding W + Left Shift for 2 seconds...");
    std::this_thread::sleep_for(std::chrono::seconds(2));

    all_ok = report("release_all()", input->release_all()) && all_ok;

    std::printf(
        "release_all_smoke %s\n",
        all_ok ? "completed successfully; all held input released"
               : "finished with errors"
    );
    return all_ok ? 0 : 1;
}
