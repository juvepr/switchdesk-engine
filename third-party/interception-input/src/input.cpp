#include <interception_input/input.hpp>

#include "device_selector.hpp"
#include "input_state.hpp"
#include "interception_context.hpp"

#include "interception.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace interception_input
{

namespace
{

// Maps public key action/prefix to Interception state bits; std::nullopt for
// enum values outside the public contract (possible via casts).
[[nodiscard]]
std::optional<unsigned short> key_state_bits(
    KeyAction action,
    KeyPrefix prefix
) noexcept
{
    unsigned short state = 0;

    switch (action) {
    case KeyAction::Down:
        state = INTERCEPTION_KEY_DOWN;
        break;
    case KeyAction::Up:
        state = INTERCEPTION_KEY_UP;
        break;
    default:
        return std::nullopt;
    }

    switch (prefix) {
    case KeyPrefix::None:
        break;
    case KeyPrefix::E0:
        state = static_cast<unsigned short>(state | INTERCEPTION_KEY_E0);
        break;
    case KeyPrefix::E1:
        state = static_cast<unsigned short>(state | INTERCEPTION_KEY_E1);
        break;
    default:
        return std::nullopt;
    }

    return state;
}

[[nodiscard]]
std::optional<unsigned short> button_state_bits(
    MouseButton button,
    bool pressed
) noexcept
{
    switch (button) {
    case MouseButton::Left:
        return static_cast<unsigned short>(
            pressed ? INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN
                    : INTERCEPTION_MOUSE_LEFT_BUTTON_UP
        );
    case MouseButton::Right:
        return static_cast<unsigned short>(
            pressed ? INTERCEPTION_MOUSE_RIGHT_BUTTON_DOWN
                    : INTERCEPTION_MOUSE_RIGHT_BUTTON_UP
        );
    case MouseButton::Middle:
        return static_cast<unsigned short>(
            pressed ? INTERCEPTION_MOUSE_MIDDLE_BUTTON_DOWN
                    : INTERCEPTION_MOUSE_MIDDLE_BUTTON_UP
        );
    case MouseButton::X1:
        return static_cast<unsigned short>(
            pressed ? INTERCEPTION_MOUSE_BUTTON_4_DOWN
                    : INTERCEPTION_MOUSE_BUTTON_4_UP
        );
    case MouseButton::X2:
        return static_cast<unsigned short>(
            pressed ? INTERCEPTION_MOUSE_BUTTON_5_DOWN
                    : INTERCEPTION_MOUSE_BUTTON_5_UP
        );
    }

    return std::nullopt;
}

} // namespace

class InterceptionInput::Impl
{
public:
    // Runs before the instance is visible to any other thread; no lock needed.
    [[nodiscard]]
    ErrorCode initialize(const Options& options)
    {
        if (!context_.valid()) {
            return ErrorCode::DriverUnavailable;
        }

        const auto keyboard = detail::resolve_device_slot(
            options.keyboard_index,
            [this](std::uint8_t slot) {
                return context_.device_exists(INTERCEPTION_KEYBOARD(slot));
            }
        );

        switch (keyboard.resolution) {
        case detail::SlotResolution::Selected:
            keyboard_device_ = INTERCEPTION_KEYBOARD(*keyboard.slot);
            break;
        case detail::SlotResolution::InvalidIndex:
            return ErrorCode::InvalidKeyboardIndex;
        case detail::SlotResolution::ExplicitSlotEmpty:
            // Explicit selection fails even when the device is not required.
            return ErrorCode::KeyboardUnavailable;
        case detail::SlotResolution::NoDeviceFound:
            if (options.require_keyboard) {
                return ErrorCode::KeyboardUnavailable;
            }
            break;
        }

        const auto mouse = detail::resolve_device_slot(
            options.mouse_index,
            [this](std::uint8_t slot) {
                return context_.device_exists(INTERCEPTION_MOUSE(slot));
            }
        );

        switch (mouse.resolution) {
        case detail::SlotResolution::Selected:
            mouse_device_ = INTERCEPTION_MOUSE(*mouse.slot);
            break;
        case detail::SlotResolution::InvalidIndex:
            return ErrorCode::InvalidMouseIndex;
        case detail::SlotResolution::ExplicitSlotEmpty:
            return ErrorCode::MouseUnavailable;
        case detail::SlotResolution::NoDeviceFound:
            if (options.require_mouse) {
                return ErrorCode::MouseUnavailable;
            }
            break;
        }

        return ErrorCode::Ok;
    }

    [[nodiscard]]
    bool keyboard_available() const
    {
        const std::scoped_lock lock {mutex_};
        return !shut_down_ && keyboard_device_.has_value();
    }

    [[nodiscard]]
    bool mouse_available() const
    {
        const std::scoped_lock lock {mutex_};
        return !shut_down_ && mouse_device_.has_value();
    }

    [[nodiscard]]
    Status send_key(const KeyStroke& stroke)
    {
        const std::scoped_lock lock {mutex_};

        if (shut_down_) {
            return {ErrorCode::AlreadyShutdown};
        }
        if (!keyboard_device_.has_value()) {
            return {ErrorCode::KeyboardUnavailable};
        }

        const std::optional<unsigned short> state =
            key_state_bits(stroke.action, stroke.prefix);
        if (!state.has_value()) {
            return {ErrorCode::InvalidArgument};
        }

        // Reserve tracking capacity up front so a tracking allocation failure
        // cannot happen after the stroke already reached the driver.
        if (stroke.action == KeyAction::Down) {
            held_.reserve_key_slot();
        }

        InterceptionKeyStroke native {};
        native.code = stroke.scan_code;
        native.state = *state;
        native.information = stroke.information;

        const bool transmitted =
            context_.send_key_stroke(*keyboard_device_, native);

        detail::apply_key_transmission(
            held_,
            detail::KeyIdentity {stroke.scan_code, stroke.prefix},
            stroke.action,
            transmitted
        );

        return transmitted ? Status::success()
                           : Status {ErrorCode::SendFailed};
    }

    [[nodiscard]]
    Status move_mouse_relative(std::int32_t delta_x, std::int32_t delta_y)
    {
        const std::scoped_lock lock {mutex_};

        if (shut_down_) {
            return {ErrorCode::AlreadyShutdown};
        }
        if (!mouse_device_.has_value()) {
            return {ErrorCode::MouseUnavailable};
        }

        InterceptionMouseStroke native {};
        native.flags = INTERCEPTION_MOUSE_MOVE_RELATIVE;
        native.x = delta_x;
        native.y = delta_y;

        if (!context_.send_mouse_stroke(*mouse_device_, native)) {
            return {ErrorCode::SendFailed};
        }

        return Status::success();
    }

    [[nodiscard]]
    Status set_mouse_button(MouseButton button, bool pressed)
    {
        const std::scoped_lock lock {mutex_};

        if (shut_down_) {
            return {ErrorCode::AlreadyShutdown};
        }
        if (!mouse_device_.has_value()) {
            return {ErrorCode::MouseUnavailable};
        }

        const std::optional<unsigned short> state =
            button_state_bits(button, pressed);
        if (!state.has_value()) {
            return {ErrorCode::InvalidArgument};
        }

        InterceptionMouseStroke native {};
        native.state = *state;

        const bool transmitted =
            context_.send_mouse_stroke(*mouse_device_, native);

        detail::apply_button_transmission(held_, button, pressed, transmitted);

        return transmitted ? Status::success()
                           : Status {ErrorCode::SendFailed};
    }

    [[nodiscard]]
    Status scroll(std::int32_t distance, bool horizontal)
    {
        const std::scoped_lock lock {mutex_};

        if (shut_down_) {
            return {ErrorCode::AlreadyShutdown};
        }
        if (!mouse_device_.has_value()) {
            return {ErrorCode::MouseUnavailable};
        }

        // distance 0 yields no chunks: zero strokes are sent and Ok returned.
        for (const std::int16_t chunk : detail::chunk_scroll(distance)) {
            InterceptionMouseStroke native {};
            native.state = horizontal
                ? static_cast<unsigned short>(INTERCEPTION_MOUSE_HWHEEL)
                : static_cast<unsigned short>(INTERCEPTION_MOUSE_WHEEL);
            native.rolling = chunk;

            if (!context_.send_mouse_stroke(*mouse_device_, native)) {
                return {ErrorCode::SendFailed};
            }
        }

        return Status::success();
    }

    [[nodiscard]]
    Status release_all()
    {
        const std::scoped_lock lock {mutex_};

        if (shut_down_) {
            return {ErrorCode::AlreadyShutdown};
        }

        return {release_all_unlocked()};
    }

    [[nodiscard]]
    Status shutdown()
    {
        const std::scoped_lock lock {mutex_};

        if (shut_down_) {
            return {ErrorCode::AlreadyShutdown};
        }

        return {shutdown_unlocked()};
    }

    void best_effort_teardown() noexcept
    {
        try {
            const std::scoped_lock lock {mutex_};

            if (!shut_down_) {
                shutdown_unlocked();
            }
        } catch (...) {
            // Destructor-path cleanup must never throw (contract §14).
        }
    }

private:
    // Callers must hold mutex_. Never re-acquires the mutex (contract §12).
    [[nodiscard]]
    ErrorCode release_all_unlocked()
    {
        return detail::release_all_held(
            held_,
            [this](const detail::KeyIdentity& key) {
                return send_key_up_unlocked(key);
            },
            [this](MouseButton button) {
                return send_button_up_unlocked(button);
            }
        );
    }

    // Callers must hold mutex_. Teardown always completes even when releases
    // fail: after the context is destroyed no retry is possible, so held-state
    // containers are cleared (approved shutdown semantics).
    ErrorCode shutdown_unlocked() noexcept
    {
        ErrorCode release_result {ErrorCode::Ok};

        try {
            release_result = release_all_unlocked();
        } catch (...) {
            release_result = ErrorCode::InternalError;
        }

        context_.reset();
        keyboard_device_.reset();
        mouse_device_.reset();
        held_.clear();
        shut_down_ = true;

        return release_result;
    }

    [[nodiscard]]
    bool send_key_up_unlocked(const detail::KeyIdentity& key)
    {
        if (!keyboard_device_.has_value()) {
            return false;
        }

        const std::optional<unsigned short> state =
            key_state_bits(KeyAction::Up, key.prefix);
        if (!state.has_value()) {
            return false;
        }

        InterceptionKeyStroke native {};
        native.code = key.scan_code;
        native.state = *state;

        return context_.send_key_stroke(*keyboard_device_, native);
    }

    [[nodiscard]]
    bool send_button_up_unlocked(MouseButton button)
    {
        if (!mouse_device_.has_value()) {
            return false;
        }

        const std::optional<unsigned short> state =
            button_state_bits(button, false);
        if (!state.has_value()) {
            return false;
        }

        InterceptionMouseStroke native {};
        native.state = *state;

        return context_.send_mouse_stroke(*mouse_device_, native);
    }

    mutable std::mutex mutex_;
    detail::ContextOwner context_;
    std::optional<InterceptionDevice> keyboard_device_;
    std::optional<InterceptionDevice> mouse_device_;
    detail::HeldState held_;
    bool shut_down_ {false};
};

std::unique_ptr<InterceptionInput> InterceptionInput::create(
    const Options& options,
    Status* status
) noexcept
{
    const auto report = [status](ErrorCode code) noexcept {
        if (status != nullptr) {
            status->code = code;
        }
    };

    try {
        auto impl = std::make_unique<Impl>();

        const ErrorCode init_result = impl->initialize(options);
        if (init_result != ErrorCode::Ok) {
            report(init_result);
            return nullptr;
        }

        std::unique_ptr<InterceptionInput> instance {
            new InterceptionInput(std::move(impl))
        };
        report(ErrorCode::Ok);
        return instance;
    } catch (...) {
        report(ErrorCode::InternalError);
        return nullptr;
    }
}

InterceptionInput::InterceptionInput(std::unique_ptr<Impl> impl) noexcept
    : impl_ {std::move(impl)}
{
}

InterceptionInput::~InterceptionInput() noexcept
{
    if (impl_ != nullptr) {
        impl_->best_effort_teardown();
    }
}

bool InterceptionInput::keyboard_available() const noexcept
{
    try {
        return impl_->keyboard_available();
    } catch (...) {
        return false;
    }
}

bool InterceptionInput::mouse_available() const noexcept
{
    try {
        return impl_->mouse_available();
    } catch (...) {
        return false;
    }
}

Status InterceptionInput::send_key(const KeyStroke& stroke) noexcept
{
    try {
        return impl_->send_key(stroke);
    } catch (...) {
        return {ErrorCode::InternalError};
    }
}

Status InterceptionInput::move_mouse_relative(
    std::int32_t delta_x,
    std::int32_t delta_y
) noexcept
{
    try {
        return impl_->move_mouse_relative(delta_x, delta_y);
    } catch (...) {
        return {ErrorCode::InternalError};
    }
}

Status InterceptionInput::set_mouse_button(
    MouseButton button,
    bool pressed
) noexcept
{
    try {
        return impl_->set_mouse_button(button, pressed);
    } catch (...) {
        return {ErrorCode::InternalError};
    }
}

Status InterceptionInput::scroll_vertical(std::int32_t distance) noexcept
{
    try {
        return impl_->scroll(distance, false);
    } catch (...) {
        return {ErrorCode::InternalError};
    }
}

Status InterceptionInput::scroll_horizontal(std::int32_t distance) noexcept
{
    try {
        return impl_->scroll(distance, true);
    } catch (...) {
        return {ErrorCode::InternalError};
    }
}

Status InterceptionInput::release_all() noexcept
{
    try {
        return impl_->release_all();
    } catch (...) {
        return {ErrorCode::InternalError};
    }
}

Status InterceptionInput::shutdown() noexcept
{
    try {
        return impl_->shutdown();
    } catch (...) {
        return {ErrorCode::InternalError};
    }
}

} // namespace interception_input
