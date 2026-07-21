#pragma once

#include "interception.h"

namespace interception_input::detail
{

// Sole owner of the persistent InterceptionContext. Exactly one context exists
// per wrapper instance for its whole lifetime (contract §4); destruction goes
// only through reset()/the destructor, never through any other path.
class ContextOwner
{
public:
    ContextOwner() noexcept;
    ~ContextOwner();

    ContextOwner(const ContextOwner&) = delete;
    ContextOwner& operator=(const ContextOwner&) = delete;
    ContextOwner(ContextOwner&&) = delete;
    ContextOwner& operator=(ContextOwner&&) = delete;

    // False when the driver did not produce a usable context.
    [[nodiscard]]
    bool valid() const noexcept;

    // Idempotent destruction: destroys the context if present and nulls the
    // handle, so a later destructor call cannot destroy it a second time.
    void reset() noexcept;

    // True when the device slot reports a non-empty hardware ID.
    [[nodiscard]]
    bool device_exists(InterceptionDevice device) const noexcept;

    // Each sends exactly one stroke; true iff the driver reports one stroke
    // written (interception_send(...) == 1).
    [[nodiscard]]
    bool send_key_stroke(
        InterceptionDevice device,
        const InterceptionKeyStroke& stroke
    ) noexcept;

    [[nodiscard]]
    bool send_mouse_stroke(
        InterceptionDevice device,
        const InterceptionMouseStroke& stroke
    ) noexcept;

private:
    InterceptionContext context_ {nullptr};
};

} // namespace interception_input::detail
