#include "interception_context.hpp"

namespace interception_input::detail
{

ContextOwner::ContextOwner() noexcept
    : context_ {interception_create_context()}
{
}

ContextOwner::~ContextOwner()
{
    reset();
}

bool ContextOwner::valid() const noexcept
{
    return context_ != nullptr;
}

void ContextOwner::reset() noexcept
{
    if (context_ != nullptr) {
        interception_destroy_context(context_);
        context_ = nullptr;
    }
}

bool ContextOwner::device_exists(InterceptionDevice device) const noexcept
{
    if (context_ == nullptr) {
        return false;
    }

    wchar_t hardware_id[256] {};
    const unsigned int bytes = interception_get_hardware_id(
        context_,
        device,
        hardware_id,
        static_cast<unsigned int>(sizeof(hardware_id))
    );

    return bytes > 0;
}

bool ContextOwner::send_key_stroke(
    InterceptionDevice device,
    const InterceptionKeyStroke& stroke
) noexcept
{
    if (context_ == nullptr) {
        return false;
    }

    return interception_send(
        context_,
        device,
        reinterpret_cast<const InterceptionStroke*>(&stroke),
        1
    ) == 1;
}

bool ContextOwner::send_mouse_stroke(
    InterceptionDevice device,
    const InterceptionMouseStroke& stroke
) noexcept
{
    if (context_ == nullptr) {
        return false;
    }

    return interception_send(
        context_,
        device,
        reinterpret_cast<const InterceptionStroke*>(&stroke),
        1
    ) == 1;
}

} // namespace interception_input::detail
