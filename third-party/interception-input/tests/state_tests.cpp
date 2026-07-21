// Driver-free unit tests for the internal state logic (contract §20). These
// exercise pure helpers only: no Interception context is created and no input
// is injected into the desktop.

#include "device_selector.hpp"
#include "input_state.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace
{

using namespace interception_input;
using namespace interception_input::detail;

int checks_run = 0;
int checks_failed = 0;

#define CHECK(condition)                                                   \
    do {                                                                   \
        ++checks_run;                                                      \
        if (!(condition)) {                                                \
            ++checks_failed;                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
        }                                                                  \
    } while (false)

[[nodiscard]]
std::int64_t chunk_sum(const std::vector<std::int16_t>& chunks)
{
    std::int64_t sum = 0;
    for (const std::int16_t chunk : chunks) {
        sum += chunk;
    }
    return sum;
}

void test_key_identity_semantics()
{
    constexpr KeyIdentity w {0x11, KeyPrefix::None};
    constexpr KeyIdentity w_again {0x11, KeyPrefix::None};
    constexpr KeyIdentity w_e0 {0x11, KeyPrefix::E0};
    constexpr KeyIdentity a {0x1E, KeyPrefix::None};

    CHECK(w == w_again);
    CHECK(w != w_e0);
    CHECK(w != a);

    // `information` is not part of held-key identity: KeyIdentity carries no
    // such field, so strokes differing only in information share an identity.
    HeldState state;
    apply_key_transmission(
        state, KeyIdentity {0x1E, KeyPrefix::None}, KeyAction::Down, true);
    apply_key_transmission(
        state, KeyIdentity {0x1E, KeyPrefix::None}, KeyAction::Up, true);
    CHECK(state.held_key_count() == 0);
}

void test_key_state_insert_and_remove()
{
    HeldState state;
    const KeyIdentity shift {0x2A, KeyPrefix::None};

    apply_key_transmission(state, shift, KeyAction::Down, true);
    CHECK(state.is_key_held(shift));
    CHECK(state.held_key_count() == 1);

    apply_key_transmission(state, shift, KeyAction::Up, true);
    CHECK(!state.is_key_held(shift));
    CHECK(state.held_key_count() == 0);
}

void test_failed_transmissions_do_not_change_state()
{
    HeldState state;
    const KeyIdentity w {0x11, KeyPrefix::None};

    apply_key_transmission(state, w, KeyAction::Down, false);
    CHECK(!state.is_key_held(w));

    apply_key_transmission(state, w, KeyAction::Down, true);
    apply_key_transmission(state, w, KeyAction::Up, false);
    CHECK(state.is_key_held(w));

    apply_button_transmission(state, MouseButton::Left, true, false);
    CHECK(!state.is_button_held(MouseButton::Left));
}

void test_duplicate_downs_accumulate_no_extra_state()
{
    HeldState state;
    const KeyIdentity w {0x11, KeyPrefix::None};

    // The wrapper transmits every requested event (tracking never gates
    // delivery); duplicate successful downs must still map to one tracked
    // identity so a single up fully clears it.
    apply_key_transmission(state, w, KeyAction::Down, true);
    apply_key_transmission(state, w, KeyAction::Down, true);
    apply_key_transmission(state, w, KeyAction::Down, true);
    CHECK(state.held_key_count() == 1);

    apply_key_transmission(state, w, KeyAction::Up, true);
    CHECK(state.held_key_count() == 0);
}

void test_prefix_distinguishes_identities()
{
    HeldState state;

    apply_key_transmission(
        state, KeyIdentity {0x11, KeyPrefix::None}, KeyAction::Down, true);
    apply_key_transmission(
        state, KeyIdentity {0x11, KeyPrefix::E0}, KeyAction::Down, true);
    CHECK(state.held_key_count() == 2);

    apply_key_transmission(
        state, KeyIdentity {0x11, KeyPrefix::E0}, KeyAction::Up, true);
    CHECK(state.held_key_count() == 1);
    CHECK(state.is_key_held(KeyIdentity {0x11, KeyPrefix::None}));
}

void test_mouse_button_state()
{
    HeldState state;

    apply_button_transmission(state, MouseButton::Left, true, true);
    apply_button_transmission(state, MouseButton::X2, true, true);
    CHECK(state.is_button_held(MouseButton::Left));
    CHECK(state.is_button_held(MouseButton::X2));
    CHECK(!state.is_button_held(MouseButton::Right));
    CHECK(state.held_button_count() == 2);

    apply_button_transmission(state, MouseButton::Left, false, true);
    CHECK(!state.is_button_held(MouseButton::Left));
    CHECK(state.held_button_count() == 1);
}

void test_scroll_chunking_positive()
{
    const auto chunks = chunk_scroll(100000);
    CHECK(chunks.size() == 4);
    CHECK(chunks[0] == 32767);
    CHECK(chunks[1] == 32767);
    CHECK(chunks[2] == 32767);
    CHECK(chunks[3] == 1699);
    CHECK(chunk_sum(chunks) == 100000);

    const auto small = chunk_scroll(120);
    CHECK(small.size() == 1);
    CHECK(small[0] == 120);

    const auto exact_max = chunk_scroll(32767);
    CHECK(exact_max.size() == 1);
    CHECK(exact_max[0] == 32767);

    const auto just_over = chunk_scroll(32768);
    CHECK(just_over.size() == 2);
    CHECK(chunk_sum(just_over) == 32768);
}

void test_scroll_chunking_negative()
{
    const auto chunks = chunk_scroll(-100000);
    CHECK(chunks.size() == 4);
    CHECK(chunks[0] == -32768);
    CHECK(chunks[1] == -32768);
    CHECK(chunks[2] == -32768);
    CHECK(chunks[3] == -1696);
    CHECK(chunk_sum(chunks) == -100000);

    const auto exact_min = chunk_scroll(-32768);
    CHECK(exact_min.size() == 1);
    CHECK(exact_min[0] == -32768);

    const auto just_over = chunk_scroll(-32769);
    CHECK(just_over.size() == 2);
    CHECK(chunk_sum(just_over) == -32769);
}

void test_scroll_chunking_extremes()
{
    constexpr std::int32_t max_distance =
        std::numeric_limits<std::int32_t>::max();
    const auto max_chunks = chunk_scroll(max_distance);
    CHECK(chunk_sum(max_chunks) == max_distance);
    CHECK(max_chunks.size() == 65539);

    constexpr std::int32_t min_distance =
        std::numeric_limits<std::int32_t>::min();
    const auto min_chunks = chunk_scroll(min_distance);
    CHECK(chunk_sum(min_chunks) == min_distance);
    CHECK(min_chunks.size() == 65536);

    CHECK(chunk_scroll(0).empty());
}

void test_release_all_success_and_order()
{
    HeldState state;
    apply_key_transmission(
        state, KeyIdentity {0x2A, KeyPrefix::None}, KeyAction::Down, true);
    apply_key_transmission(
        state, KeyIdentity {0x11, KeyPrefix::None}, KeyAction::Down, true);
    apply_button_transmission(state, MouseButton::Right, true, true);

    std::vector<std::string> order;
    const ErrorCode result = release_all_held(
        state,
        [&order](const KeyIdentity& key) {
            order.push_back("key:" + std::to_string(key.scan_code));
            return true;
        },
        [&order](MouseButton button) {
            order.push_back(
                "button:" + std::to_string(static_cast<int>(button)));
            return true;
        }
    );

    CHECK(result == ErrorCode::Ok);
    CHECK(state.held_key_count() == 0);
    CHECK(state.held_button_count() == 0);
    CHECK(order.size() == 3);
    CHECK(order[0].starts_with("key:"));
    CHECK(order[1].starts_with("key:"));
    CHECK(order[2].starts_with("button:"));
}

void test_release_all_best_effort()
{
    HeldState state;
    const KeyIdentity shift {0x2A, KeyPrefix::None};
    const KeyIdentity w {0x11, KeyPrefix::None};
    const KeyIdentity right_ctrl {0x1D, KeyPrefix::E0};

    apply_key_transmission(state, shift, KeyAction::Down, true);
    apply_key_transmission(state, w, KeyAction::Down, true);
    apply_key_transmission(state, right_ctrl, KeyAction::Down, true);
    apply_button_transmission(state, MouseButton::Right, true, true);

    int key_attempts = 0;
    int button_attempts = 0;

    const ErrorCode result = release_all_held(
        state,
        [&](const KeyIdentity& key) {
            ++key_attempts;
            return key != w; // only the W release fails
        },
        [&](MouseButton) {
            ++button_attempts;
            return true;
        }
    );

    CHECK(result == ErrorCode::SendFailed);
    CHECK(key_attempts == 3);    // one failure did not stop the remaining attempts
    CHECK(button_attempts == 1);
    CHECK(state.is_key_held(w)); // failed release remains tracked
    CHECK(!state.is_key_held(shift));
    CHECK(!state.is_key_held(right_ctrl));
    CHECK(!state.is_button_held(MouseButton::Right));
    CHECK(state.held_key_count() == 1);

    // A later attempt can retry the remaining tracked input.
    const ErrorCode retry = release_all_held(
        state,
        [](const KeyIdentity&) { return true; },
        [](MouseButton) { return true; }
    );
    CHECK(retry == ErrorCode::Ok);
    CHECK(state.held_key_count() == 0);
}

void test_release_all_button_failure()
{
    HeldState state;
    apply_button_transmission(state, MouseButton::Left, true, true);
    apply_button_transmission(state, MouseButton::Middle, true, true);

    const ErrorCode result = release_all_held(
        state,
        [](const KeyIdentity&) { return true; },
        [](MouseButton button) { return button != MouseButton::Left; }
    );

    CHECK(result == ErrorCode::SendFailed);
    CHECK(state.is_button_held(MouseButton::Left));
    CHECK(!state.is_button_held(MouseButton::Middle));
    CHECK(state.held_button_count() == 1);
}

void test_explicit_device_index_validation()
{
    const auto probe_none = [](std::uint8_t) { return false; };
    const auto probe_all = [](std::uint8_t) { return true; };
    const auto probe_2_and_5 = [](std::uint8_t slot) {
        return slot == 2 || slot == 5;
    };

    // Explicit indexes outside 0-9 are invalid.
    CHECK(resolve_device_slot(std::uint8_t {10}, probe_all).resolution
          == SlotResolution::InvalidIndex);
    CHECK(resolve_device_slot(std::uint8_t {255}, probe_all).resolution
          == SlotResolution::InvalidIndex);

    // Boundary indexes 0 and 9 are valid.
    const auto slot0 = resolve_device_slot(std::uint8_t {0}, probe_all);
    CHECK(slot0.resolution == SlotResolution::Selected);
    CHECK(slot0.slot == std::uint8_t {0});

    const auto slot9 = resolve_device_slot(std::uint8_t {9}, probe_all);
    CHECK(slot9.resolution == SlotResolution::Selected);
    CHECK(slot9.slot == std::uint8_t {9});

    // Explicit in-range index with no device: no silent fallback.
    const auto empty_slot = resolve_device_slot(std::uint8_t {3}, probe_2_and_5);
    CHECK(empty_slot.resolution == SlotResolution::ExplicitSlotEmpty);
    CHECK(!empty_slot.slot.has_value());

    // Automatic selection picks the first occupied slot.
    const auto auto_pick = resolve_device_slot(std::nullopt, probe_2_and_5);
    CHECK(auto_pick.resolution == SlotResolution::Selected);
    CHECK(auto_pick.slot == std::uint8_t {2});

    // Automatic selection with no devices at all.
    CHECK(resolve_device_slot(std::nullopt, probe_none).resolution
          == SlotResolution::NoDeviceFound);
}

} // namespace

int main()
{
    test_key_identity_semantics();
    test_key_state_insert_and_remove();
    test_failed_transmissions_do_not_change_state();
    test_duplicate_downs_accumulate_no_extra_state();
    test_prefix_distinguishes_identities();
    test_mouse_button_state();
    test_scroll_chunking_positive();
    test_scroll_chunking_negative();
    test_scroll_chunking_extremes();
    test_release_all_success_and_order();
    test_release_all_best_effort();
    test_release_all_button_failure();
    test_explicit_device_index_validation();

    std::printf(
        "state_tests: %d checks, %d failures\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
