#include <interception_input/input.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace
{

using interception_input::ErrorCode;
using interception_input::InterceptionInput;
using interception_input::Status;

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

void pause_briefly()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

} // namespace

int main()
{
    std::puts("mouse_smoke");
    std::puts("Sending mouse movement and scrolling in 3 seconds.");
    std::puts("No clicks will be sent.");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    interception_input::Options options;
    options.require_keyboard = false;
    options.require_mouse = true;

    Status status;
    const auto input = InterceptionInput::create(options, &status);

    if (input == nullptr) {
        std::printf("create() failed: %s\n", error_name(status.code));
        std::puts("Is the Interception driver installed and a mouse attached?");
        return 1;
    }

    std::printf(
        "created; keyboard_available=%s mouse_available=%s\n",
        input->keyboard_available() ? "true" : "false",
        input->mouse_available() ? "true" : "false"
    );

    bool all_ok = true;

    all_ok = report("move +100 X", input->move_mouse_relative(100, 0)) && all_ok;
    pause_briefly();
    all_ok = report("move +100 Y", input->move_mouse_relative(0, 100)) && all_ok;
    pause_briefly();
    all_ok = report("move -100 X", input->move_mouse_relative(-100, 0)) && all_ok;
    pause_briefly();
    all_ok = report("move -100 Y", input->move_mouse_relative(0, -100)) && all_ok;
    pause_briefly();

    all_ok = report("scroll vertical +240", input->scroll_vertical(240)) && all_ok;
    pause_briefly();
    all_ok = report("scroll vertical -240", input->scroll_vertical(-240)) && all_ok;
    pause_briefly();
    all_ok = report("scroll horizontal +240", input->scroll_horizontal(240)) && all_ok;
    pause_briefly();
    all_ok = report("scroll horizontal -240", input->scroll_horizontal(-240)) && all_ok;

    std::printf(
        "mouse_smoke %s\n",
        all_ok ? "completed successfully" : "finished with errors"
    );
    return all_ok ? 0 : 1;
}
