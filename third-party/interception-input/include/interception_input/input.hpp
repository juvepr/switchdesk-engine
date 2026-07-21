#pragma once

#include <cstdint>
#include <memory>
#include <optional>

namespace interception_input
{

enum class ErrorCode : std::uint8_t
{
    Ok = 0,

    DriverUnavailable,
    KeyboardUnavailable,
    MouseUnavailable,

    InvalidKeyboardIndex,
    InvalidMouseIndex,
    InvalidArgument,

    SendFailed,
    AlreadyShutdown,
    InternalError
};

struct Status
{
    ErrorCode code {ErrorCode::Ok};

    [[nodiscard]]
    constexpr bool ok() const noexcept
    {
        return code == ErrorCode::Ok;
    }

    [[nodiscard]]
    constexpr explicit operator bool() const noexcept
    {
        return ok();
    }

    [[nodiscard]]
    static constexpr Status success() noexcept
    {
        return {};
    }
};

enum class KeyAction : std::uint8_t
{
    Down,
    Up
};

enum class KeyPrefix : std::uint8_t
{
    None,
    E0,
    E1
};

struct KeyStroke
{
    std::uint16_t scan_code {};
    KeyAction action {KeyAction::Down};
    KeyPrefix prefix {KeyPrefix::None};
    std::uint32_t information {};
};

enum class MouseButton : std::uint8_t
{
    Left,
    Right,
    Middle,
    X1,
    X2
};

struct Options
{
    // Interception keyboard slot 0-9.
    // std::nullopt means automatically select the first available keyboard.
    std::optional<std::uint8_t> keyboard_index;

    // Interception mouse slot 0-9.
    // std::nullopt means automatically select the first available mouse.
    std::optional<std::uint8_t> mouse_index;

    // Creation fails if no keyboard is available.
    bool require_keyboard {true};

    // Creation fails if no mouse is available.
    bool require_mouse {false};
};

class InterceptionInput final
{
public:
    [[nodiscard]]
    static std::unique_ptr<InterceptionInput> create(
        const Options& options = {},
        Status* status = nullptr
    ) noexcept;

    ~InterceptionInput() noexcept;

    InterceptionInput(const InterceptionInput&) = delete;
    InterceptionInput& operator=(const InterceptionInput&) = delete;

    InterceptionInput(InterceptionInput&&) = delete;
    InterceptionInput& operator=(InterceptionInput&&) = delete;

    [[nodiscard]]
    bool keyboard_available() const noexcept;

    [[nodiscard]]
    bool mouse_available() const noexcept;

    [[nodiscard]]
    Status send_key(const KeyStroke& stroke) noexcept;

    [[nodiscard]]
    Status move_mouse_relative(
        std::int32_t delta_x,
        std::int32_t delta_y
    ) noexcept;

    [[nodiscard]]
    Status set_mouse_button(
        MouseButton button,
        bool pressed
    ) noexcept;

    [[nodiscard]]
    Status scroll_vertical(
        std::int32_t distance
    ) noexcept;

    [[nodiscard]]
    Status scroll_horizontal(
        std::int32_t distance
    ) noexcept;

    [[nodiscard]]
    Status release_all() noexcept;

    [[nodiscard]]
    Status shutdown() noexcept;

private:
    class Impl;

    explicit InterceptionInput(
        std::unique_ptr<Impl> impl
    ) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace interception_input
