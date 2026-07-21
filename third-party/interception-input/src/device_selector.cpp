#include "device_selector.hpp"

namespace interception_input::detail
{

SlotResult resolve_device_slot(
    const std::optional<std::uint8_t>& explicit_index,
    const SlotProbe& probe
)
{
    if (explicit_index.has_value()) {
        const std::uint8_t slot = *explicit_index;

        if (slot > max_device_slot) {
            return {SlotResolution::InvalidIndex, std::nullopt};
        }

        if (probe(slot)) {
            return {SlotResolution::Selected, slot};
        }

        return {SlotResolution::ExplicitSlotEmpty, std::nullopt};
    }

    for (std::uint8_t slot = 0; slot <= max_device_slot; ++slot) {
        if (probe(slot)) {
            return {SlotResolution::Selected, slot};
        }
    }

    return {SlotResolution::NoDeviceFound, std::nullopt};
}

} // namespace interception_input::detail
