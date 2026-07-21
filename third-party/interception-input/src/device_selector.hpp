#pragma once

#include <cstdint>
#include <functional>
#include <optional>

namespace interception_input::detail
{

// Interception exposes ten keyboard slots and ten mouse slots (0-9).
inline constexpr std::uint8_t max_device_slot = 9;

enum class SlotResolution : std::uint8_t
{
    // A device slot was selected.
    Selected,

    // Automatic scan found no occupied slot.
    NoDeviceFound,

    // Explicit index outside 0-9.
    InvalidIndex,

    // Explicit in-range index whose slot has no device. Explicit selection
    // never silently falls back to another slot (contract §5).
    ExplicitSlotEmpty
};

struct SlotResult
{
    SlotResolution resolution {SlotResolution::NoDeviceFound};
    std::optional<std::uint8_t> slot;
};

// Returns true when the slot holds a device with a non-empty hardware ID.
using SlotProbe = std::function<bool(std::uint8_t slot)>;

// Resolves which device slot to use: an explicit caller-requested slot, or
// std::nullopt for an automatic 0-through-9 scan selecting the first hit.
[[nodiscard]]
SlotResult resolve_device_slot(
    const std::optional<std::uint8_t>& explicit_index,
    const SlotProbe& probe
);

} // namespace interception_input::detail
