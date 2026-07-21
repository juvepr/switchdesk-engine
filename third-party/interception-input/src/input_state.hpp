#pragma once

#include <interception_input/input.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace interception_input::detail
{

// Identity of a held key: scan code + prefix only. The `information` field is
// intentionally not part of held-key identity (contract §8).
struct KeyIdentity
{
    std::uint16_t scan_code {};
    KeyPrefix prefix {KeyPrefix::None};

    friend constexpr bool operator==(
        const KeyIdentity&,
        const KeyIdentity&
    ) noexcept = default;
};

inline constexpr std::size_t mouse_button_count = 5;

[[nodiscard]]
constexpr std::size_t mouse_button_slot(MouseButton button) noexcept
{
    return static_cast<std::size_t>(button);
}

// Tracks currently held inputs for cleanup purposes only. It never gates or
// deduplicates caller-requested event delivery (contract §8).
class HeldState
{
public:
    // Callers must guarantee the event was successfully transmitted before
    // mutating state; use apply_*_transmission below.
    void note_key_down(const KeyIdentity& key)
    {
        if (!is_key_held(key)) {
            held_keys_.push_back(key);
        }
    }

    void note_key_up(const KeyIdentity& key)
    {
        std::erase(held_keys_, key);
    }

    [[nodiscard]]
    bool is_key_held(const KeyIdentity& key) const
    {
        return std::ranges::find(held_keys_, key) != held_keys_.end();
    }

    [[nodiscard]]
    const std::vector<KeyIdentity>& held_keys() const
    {
        return held_keys_;
    }

    [[nodiscard]]
    std::size_t held_key_count() const
    {
        return held_keys_.size();
    }

    // Guarantees the next note_key_down cannot allocate. Called before
    // transmitting a key-down so that a tracking allocation failure cannot
    // occur after the event already reached the driver.
    void reserve_key_slot()
    {
        if (held_keys_.size() == held_keys_.capacity()) {
            held_keys_.reserve(
                held_keys_.capacity() == 0 ? 8 : held_keys_.capacity() * 2
            );
        }
    }

    void note_button(MouseButton button, bool pressed)
    {
        const std::size_t slot = mouse_button_slot(button);
        if (slot < mouse_button_count) {
            held_buttons_[slot] = pressed;
        }
    }

    [[nodiscard]]
    bool is_button_held(MouseButton button) const
    {
        const std::size_t slot = mouse_button_slot(button);
        return slot < mouse_button_count && held_buttons_[slot];
    }

    [[nodiscard]]
    std::size_t held_button_count() const
    {
        return static_cast<std::size_t>(
            std::ranges::count(held_buttons_, true)
        );
    }

    void clear()
    {
        held_keys_.clear();
        held_buttons_.fill(false);
    }

private:
    std::vector<KeyIdentity> held_keys_;
    std::array<bool, mouse_button_count> held_buttons_ {};
};

// Only successfully transmitted events may change tracked state (contract §8).
inline void apply_key_transmission(
    HeldState& state,
    const KeyIdentity& key,
    KeyAction action,
    bool transmitted
)
{
    if (!transmitted) {
        return;
    }

    if (action == KeyAction::Down) {
        state.note_key_down(key);
    } else {
        state.note_key_up(key);
    }
}

inline void apply_button_transmission(
    HeldState& state,
    MouseButton button,
    bool pressed,
    bool transmitted
)
{
    if (!transmitted) {
        return;
    }

    state.note_button(button, pressed);
}

// Splits a requested scroll distance into strokes that fit the driver's signed
// 16-bit rolling field. The chunk sum always equals `distance` exactly, for
// positive and negative values; distance 0 produces no chunks (contract §11).
[[nodiscard]]
inline std::vector<std::int16_t> chunk_scroll(std::int32_t distance)
{
    constexpr std::int64_t max_chunk = std::numeric_limits<std::int16_t>::max();
    constexpr std::int64_t min_chunk = std::numeric_limits<std::int16_t>::min();

    std::vector<std::int16_t> chunks;
    // 64-bit remainder so INT32_MIN cannot overflow during chunking.
    std::int64_t remaining = distance;

    while (remaining != 0) {
        const std::int64_t chunk = std::clamp(remaining, min_chunk, max_chunk);
        chunks.push_back(static_cast<std::int16_t>(chunk));
        remaining -= chunk;
    }

    return chunks;
}

// Best-effort release of everything held: keys first, then buttons
// (contract §12). A failed release keeps the input tracked so a later attempt
// can retry it; the first failure is returned after all attempts complete.
template <typename SendKeyUp, typename SendButtonUp>
[[nodiscard]]
ErrorCode release_all_held(
    HeldState& state,
    const SendKeyUp& send_key_up,
    const SendButtonUp& send_button_up
)
{
    ErrorCode first_error {ErrorCode::Ok};

    const std::vector<KeyIdentity> keys = state.held_keys();
    for (const KeyIdentity& key : keys) {
        if (send_key_up(key)) {
            state.note_key_up(key);
        } else if (first_error == ErrorCode::Ok) {
            first_error = ErrorCode::SendFailed;
        }
    }

    for (std::size_t slot = 0; slot < mouse_button_count; ++slot) {
        const auto button = static_cast<MouseButton>(slot);
        if (!state.is_button_held(button)) {
            continue;
        }

        if (send_button_up(button)) {
            state.note_button(button, false);
        } else if (first_error == ErrorCode::Ok) {
            first_error = ErrorCode::SendFailed;
        }
    }

    return first_error;
}

} // namespace interception_input::detail
