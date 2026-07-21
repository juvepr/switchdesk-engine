# Interception Input Wrapper — Implementation Contract v0.1

## 1. Purpose

Implement a standalone modern C++23 static library named `interception-input`.

The library provides a clean, thread-safe C++ wrapper around the vendored oblitum Interception API.

The long-term intention is for this wrapper to eventually serve as an alternative Windows input backend for Sunshine, but this repository must contain no Sunshine-specific or Moonlight-specific code.

The public API is already defined and frozen in:

    include/interception_input/input.hpp

Do not redesign or modify that public API unless a required constraint is technically impossible. If such a problem is discovered, stop and explain it before changing the public interface.

---

## 2. Non-goals

Version 0.1 must not implement:

- Qt
- GUI code
- global hotkeys
- Sunshine-specific types
- Moonlight-specific types
- Windows INPUT structures in the public API
- virtual-key translation
- absolute mouse movement
- touch input
- pen input
- gamepad input
- physical keyboard interception/filtering
- physical mouse interception/filtering

This library is an input-sending backend only.

---

## 3. Public API boundary

The only public header is:

    include/interception_input/input.hpp

It must not expose or include:

- windows.h
- interception.h
- InterceptionContext
- InterceptionDevice
- InterceptionStroke
- InterceptionKeyStroke
- InterceptionMouseStroke
- Windows HANDLE types
- Windows INPUT structures

All platform-specific and Interception-specific implementation details must remain private.

Use the existing PImpl boundary:

    class InterceptionInput::Impl

---

## 4. Persistent context ownership

Each InterceptionInput instance must own exactly one persistent:

    InterceptionContext

Create it once during:

    InterceptionInput::create()

Keep it alive for the lifetime of the wrapper instance.

Do not create and destroy an Interception context for each input event.

Lifecycle:

    create()
        -> interception_create_context()
        -> select keyboard and/or mouse devices
        -> send potentially many input events
        -> shutdown()
        -> best-effort release_all()
        -> interception_destroy_context()

The destructor must perform best-effort shutdown.

No exception may escape the destructor.

---

## 5. Device selection

Interception exposes ten keyboard slots and ten mouse slots.

Automatic keyboard selection:

    for index 0 through 9:
        device = INTERCEPTION_KEYBOARD(index)
        call interception_get_hardware_id()
        if a non-empty hardware ID is returned:
            select this keyboard
            stop searching

Automatic mouse selection:

    for index 0 through 9:
        device = INTERCEPTION_MOUSE(index)
        call interception_get_hardware_id()
        if a non-empty hardware ID is returned:
            select this mouse
            stop searching

Explicit selection:

    Options::keyboard_index
    Options::mouse_index

Valid explicit indexes are 0 through 9 inclusive.

Invalid explicit keyboard indexes return:

    ErrorCode::InvalidKeyboardIndex

Invalid explicit mouse indexes return:

    ErrorCode::InvalidMouseIndex

An explicitly selected device must be verified to exist.

Do not silently fall back to another device when an explicit index is invalid or unavailable.

---

## 6. Availability semantics

Options contains:

    require_keyboard
    require_mouse

Example:

    require_keyboard = true
    require_mouse = false

If a keyboard exists but no mouse exists:

    create() succeeds
    keyboard_available() == true
    mouse_available() == false

Calling a keyboard operation without an available keyboard returns:

    ErrorCode::KeyboardUnavailable

Calling a mouse operation without an available mouse returns:

    ErrorCode::MouseUnavailable

If a required device is unavailable, creation must fail.

---

## 7. Keyboard behavior

Public input type:

    KeyStroke

Fields:

    scan_code
    action
    prefix
    information

Map:

    KeyAction::Down
        -> INTERCEPTION_KEY_DOWN

    KeyAction::Up
        -> INTERCEPTION_KEY_UP

Map prefixes:

    KeyPrefix::None
        -> no prefix flag

    KeyPrefix::E0
        -> INTERCEPTION_KEY_E0

    KeyPrefix::E1
        -> INTERCEPTION_KEY_E1

Construct an InterceptionKeyStroke and send exactly one stroke.

Success means:

    interception_send(...) == 1

Any other result returns:

    ErrorCode::SendFailed

Do not perform virtual-key translation.

The wrapper accepts scan codes only.

---

## 8. Keyboard state tracking

Track currently held keys internally.

A key identity consists of:

    scan_code
    prefix

The `information` field is not part of held-key identity.

After a successfully transmitted key-down:

    add the key identity to held state

After a successfully transmitted key-up:

    remove the key identity from held state

Only successfully transmitted events may change tracked state.

Do not suppress duplicate events.

This sequence must send all four events:

    W down
    W down
    W down
    W up

State tracking is for cleanup purposes only.

It must not alter or deduplicate caller-requested event delivery.

---

## 9. Relative mouse movement

Implement:

    move_mouse_relative(
        std::int32_t delta_x,
        std::int32_t delta_y
    )

Construct an InterceptionMouseStroke using:

    INTERCEPTION_MOUSE_MOVE_RELATIVE

Set:

    x = delta_x
    y = delta_y

Send exactly one stroke.

Success means:

    interception_send(...) == 1

Otherwise return:

    ErrorCode::SendFailed

Do not implement absolute mouse positioning in v0.1.

---

## 10. Mouse buttons

Support:

    MouseButton::Left
    MouseButton::Right
    MouseButton::Middle
    MouseButton::X1
    MouseButton::X2

Map exactly:

    Left pressed
        -> INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN

    Left released
        -> INTERCEPTION_MOUSE_LEFT_BUTTON_UP

    Right pressed
        -> INTERCEPTION_MOUSE_RIGHT_BUTTON_DOWN

    Right released
        -> INTERCEPTION_MOUSE_RIGHT_BUTTON_UP

    Middle pressed
        -> INTERCEPTION_MOUSE_MIDDLE_BUTTON_DOWN

    Middle released
        -> INTERCEPTION_MOUSE_MIDDLE_BUTTON_UP

    X1 pressed
        -> INTERCEPTION_MOUSE_BUTTON_4_DOWN

    X1 released
        -> INTERCEPTION_MOUSE_BUTTON_4_UP

    X2 pressed
        -> INTERCEPTION_MOUSE_BUTTON_5_DOWN

    X2 released
        -> INTERCEPTION_MOUSE_BUTTON_5_UP

Track successfully held mouse buttons.

Only successful transmissions update held-button state.

Do not suppress duplicate requested button events.

---

## 11. Vertical and horizontal scrolling

Vertical wheel:

    INTERCEPTION_MOUSE_WHEEL

Horizontal wheel:

    INTERCEPTION_MOUSE_HWHEEL

The public API accepts:

    std::int32_t distance

InterceptionMouseStroke stores wheel distance in a signed 16-bit rolling field.

Never silently truncate or overflow.

Large values must be split into multiple valid strokes.

Example:

    100000

may become:

    32767
    32767
    32767
    1699

The total emitted distance must equal the requested distance.

The same requirement applies to positive and negative values.

If any individual send fails, return:

    ErrorCode::SendFailed

Do not claim success when the complete requested distance was not transmitted.

---

## 12. release_all()

release_all() must best-effort release every tracked held key and every tracked held mouse button.

Example held state:

    Left Shift
    W
    Right Ctrl
    Right mouse button

release_all() must attempt:

    Left Shift up
    W up
    Right Ctrl up
    Right mouse button up

Recommended order:

    keys first
    mouse buttons second

The exact release order within each category is not part of the public contract.

If one release fails:

    continue attempting all remaining releases

Return the first encountered failure after best-effort cleanup has completed.

Successfully released inputs must be removed from tracked state.

Inputs whose release failed must remain tracked so another cleanup attempt may retry them.

release_all() must not recursively acquire the public mutex.

Use an internal unlocked helper where necessary.

---

## 13. Thread safety

Every public operation must be thread-safe:

    keyboard_available()
    mouse_available()
    send_key()
    move_mouse_relative()
    set_mouse_button()
    scroll_vertical()
    scroll_horizontal()
    release_all()
    shutdown()

Use one internal std::mutex for v0.1.

Do not introduce complicated lock hierarchies.

The mutex protects:

    Interception context
    selected device IDs
    held keyboard state
    held mouse-button state
    shutdown state

Avoid recursive locking.

Public functions may call private `_unlocked` helpers after acquiring the mutex.

---

## 14. Shutdown semantics

shutdown() must:

    acquire the mutex
    detect whether already shut down
    best-effort release all held input
    destroy the Interception context
    clear selected device IDs
    mark the object shut down

A second explicit shutdown() call returns:

    ErrorCode::AlreadyShutdown

After shutdown, input-sending methods must not access a destroyed context.

Use:

    ErrorCode::AlreadyShutdown

for calls made on an explicitly shut-down instance unless a more precise existing contract clearly applies.

The destructor must perform best-effort cleanup without throwing.

---

## 15. Error handling

Use only the existing public ErrorCode values:

    Ok
    DriverUnavailable
    KeyboardUnavailable
    MouseUnavailable
    InvalidKeyboardIndex
    InvalidMouseIndex
    InvalidArgument
    SendFailed
    AlreadyShutdown
    InternalError

Do not add public error codes without first explaining why the existing contract is insufficient.

All public functions marked noexcept must actually prevent exceptions from escaping.

Catch unexpected internal exceptions and convert them to:

    ErrorCode::InternalError

Do not allow exceptions to cross the public API boundary.

---

## 16. Input validation

Reject clearly invalid public input using:

    ErrorCode::InvalidArgument

Do not reject valid scan code 0 solely because it is zero unless Interception itself makes such usage invalid.

Do not add arbitrary policy restrictions not required by the public contract.

---

## 17. Internal file responsibilities

### src/input.cpp

Implement:

    InterceptionInput::create()
    InterceptionInput destructor
    public method forwarding
    InterceptionInput::Impl ownership and/or orchestration

Keep public API handling here.

### src/interception_context.hpp
### src/interception_context.cpp

Own and manage the lifetime of:

    InterceptionContext

Responsibilities may include:

    context creation
    context destruction
    native send helpers
    shutdown-safe ownership

Do not expose Interception types publicly.

### src/device_selector.hpp
### src/device_selector.cpp

Implement:

    keyboard discovery
    mouse discovery
    explicit device index validation
    hardware ID existence checks

Keep device-discovery policy isolated here.

Reasonable internal restructuring is allowed if it improves correctness, but do not change the public API.

---

## 18. Build requirements

Use:

    C++23
    CMake
    Ninja-compatible configuration

The vendored C implementation is located at:

    external/interception/interception.c

The vendored header is located at:

    external/interception/interception.h

Compile interception.c directly into the static library.

Define:

    INTERCEPTION_STATIC

No Qt dependency.

No GUI dependency.

No Sunshine dependency.

No Moonlight dependency.

---

## 19. Examples to implement

Create:

    examples/keyboard_smoke.cpp
    examples/mouse_smoke.cpp
    examples/release_all_smoke.cpp

### keyboard_smoke

Behavior:

    wait 3 seconds
    tell the user to focus Notepad
    send A
    send B
    send C
    send an E0-prefixed Right Arrow
    send Shift+A with explicit down/up events
    print clear status/error information

Use known scan codes directly.

Do not add virtual-key translation.

### mouse_smoke

Behavior:

    wait 3 seconds
    move relative +100 X
    move relative +100 Y
    move relative -100 X
    move relative -100 Y
    perform vertical scrolling
    perform horizontal scrolling
    print clear status/error information

Do not automatically click.

### release_all_smoke

Behavior:

    wait 3 seconds
    send W down
    send Left Shift down
    wait 2 seconds
    call release_all()
    print clear cleanup status

---

## 20. Tests

Create:

    tests/state_tests.cpp

Tests must not require a physical keyboard event to be injected into the active desktop merely to run normal unit tests.

Prefer testing pure/internal state behavior and helper logic independently from live driver integration.

At minimum test:

    held-key identity semantics
    key state insertion after successful down
    key state removal after successful up
    duplicate down events do not change delivery requirements
    mouse-button held state
    scroll chunking for positive values
    scroll chunking for negative values
    maximum int32_t scroll distance
    minimum int32_t scroll distance
    best-effort release behavior
    failed releases remain tracked
    successful releases are removed
    explicit device index validation

Do not modify the public API merely to facilitate tests.

Internal test seams are acceptable.

---

## 21. Required CMake additions

The root CMake project must build:

    interception_input

as a static library.

Also add buildable example targets:

    keyboard_smoke
    mouse_smoke
    release_all_smoke

Enable testing with CTest.

Add a test executable for:

    state_tests

All targets must build with the existing MSYS2 UCRT64 GCC toolchain.

---

## 22. Important design constraints

Do not:

- recreate InterceptionContext per event
- hardcode INTERCEPTION_KEYBOARD(0) as the only supported keyboard
- hardcode INTERCEPTION_MOUSE(0) as the only supported mouse
- expose interception.h publicly
- expose windows.h publicly
- silently overflow wheel distance
- suppress duplicate input events
- stop cleanup after the first failed release
- modify held state after failed transmissions
- leak exceptions through noexcept functions
- introduce Qt
- introduce a GUI
- add global hotkeys
- add Sunshine code
- add Moonlight code
- add virtual-key translation
- implement absolute mouse input in v0.1

---

## 23. Acceptance criteria

The implementation is not considered complete unless all of the following are true:

- [ ] Builds as a standalone C++23 static library.
- [ ] Public API remains unchanged.
- [ ] Public header exposes no Windows or Interception implementation types.
- [ ] One persistent Interception context exists per wrapper instance.
- [ ] Context is not recreated per event.
- [ ] Keyboard can be automatically discovered.
- [ ] Mouse can be automatically discovered.
- [ ] Explicit keyboard and mouse indexes are supported.
- [ ] Explicit indexes outside 0-9 fail clearly.
- [ ] Explicit unavailable devices fail clearly.
- [ ] Normal keyboard scan codes work.
- [ ] E0-prefixed keys work.
- [ ] E1 representation is supported.
- [ ] Keyboard down/up state is tracked.
- [ ] Duplicate requested keyboard events are still transmitted.
- [ ] Relative mouse movement works.
- [ ] Left, right, middle, X1, and X2 buttons are supported.
- [ ] Mouse-button state is tracked.
- [ ] Vertical scrolling works.
- [ ] Horizontal scrolling works.
- [ ] Large scroll distances cannot silently overflow.
- [ ] release_all() performs best-effort cleanup.
- [ ] One failed release does not stop remaining cleanup attempts.
- [ ] Failed releases remain tracked.
- [ ] Successfully released inputs are removed from tracked state.
- [ ] Only successful transmissions alter held state.
- [ ] Every public operation is thread-safe.
- [ ] shutdown() safely destroys the context.
- [ ] Destructor performs best-effort cleanup.
- [ ] No exception escapes a public noexcept function.
- [ ] No Qt dependency exists.
- [ ] No GUI exists.
- [ ] No global hotkeys exist.
- [ ] No Sunshine dependency exists.
- [ ] No Moonlight dependency exists.
- [ ] No virtual-key translation exists.
- [ ] No absolute mouse implementation exists in v0.1.
- [ ] keyboard_smoke builds.
- [ ] mouse_smoke builds.
- [ ] release_all_smoke builds.
- [ ] state_tests builds and runs through CTest.

---

## 24. Implementation process for Claude

Before writing code:

1. Read this entire contract.
2. Read `include/interception_input/input.hpp`.
3. Read the vendored `interception.h`.
4. Inspect the existing CMakeLists.txt.
5. Summarize the implementation plan.
6. Identify any real ambiguity or technical conflict.
7. Do not redesign the public API without explicit approval.

Then implement the project.

After implementation:

1. Build from a clean build directory.
2. Run CTest.
3. Report every changed file.
4. Report any warning.
5. Report any requirement not fully implemented.
6. Do not claim completion if any acceptance criterion is knowingly unmet.
