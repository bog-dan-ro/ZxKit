// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "Tools.h"

#include <zxkit/input/Joystick.h>
#include <zxkit/input/Mouse.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace zxkit::mcp_tool {
using namespace std::literals;
namespace {
struct MouseInput
{
    int dx;
    int dy;
    int buttons;
};

struct InputPhase
{
    int frames;
    std::vector<ZxKey> keys;
    std::vector<NextKey> nextKeys;
    std::optional<uint8_t> joystick;
    std::optional<MouseInput> mouse;
    bool endsAction = true;
};

int inputInteger(const Json &value, int minimum, int maximum, std::string_view name)
{
    if (!value.is_int64() || value.as_int64() < minimum || value.as_int64() > maximum)
        throw std::invalid_argument(std::string(name) + " must be an integer in ["s
                                    + std::to_string(minimum) + ", "s + std::to_string(maximum)
                                    + "]"s);
    return int(value.as_int64());
}

std::string textChord(char character)
{
    if (character == ' ')
        return "SPACE"s;
    if (character == '\n' || character == '\r')
        return "ENTER"s;
    if ((character >= '0' && character <= '9') || (character >= 'a' && character <= 'z'))
        return std::string(1, character);
    if (character >= 'A' && character <= 'Z')
        return "CS+"s + character;
    constexpr auto symbols = "!@#$%&'()_<>;\"^-+=:?/*,."sv;
    // Keep the Spectrum SYMBOL SHIFT legend aligned with the key letters.
    constexpr auto shifted = "1234567890RTOPHJKLZCVBNM"sv;
    static_assert(symbols.size() == shifted.size());
    if (auto index = symbols.find(character); index != symbols.npos)
        return "SS+"s + shifted[index];
    throw std::invalid_argument("Text contains a character with no Spectrum key chord"s);
}

std::vector<NextKey> nextChord(std::string_view chord)
{
    constexpr std::array names{ "RIGHT"sv,    "LEFT"sv,     "DOWN"sv,  "UP"sv,
                                "PERIOD"sv,   "COMMA"sv,    "QUOTE"sv, "SEMICOLON"sv,
                                "EXTEND"sv,   "CAPSLOCK"sv, "GRAPH"sv, "TRUEVIDEO"sv,
                                "INVVIDEO"sv, "BREAK"sv,    "EDIT"sv,  "DELETE"sv };
    std::vector<NextKey> keys;
    for (;;) {
        const auto separator = chord.find('+');
        const auto name = upper(std::string(chord.substr(0, separator)));
        const auto found = std::ranges::find(names, name);
        if (found == names.end())
            throw std::invalid_argument("Unknown Next extended key: "s + name);
        keys.push_back(NextKey(found - names.begin()));
        if (separator == chord.npos)
            return keys;
        chord.remove_prefix(separator + 1);
    }
}

template <typename Device> Device *inputDevice(Emulator &emulator, std::string_view id, bool attach)
{
    auto found = emulator.findPeripheral(id);
    if (!found && attach) {
        if (emulator.rzx().isRecording() || emulator.rzx().isReviewing()
            || emulator.rzx().isPlaying())
            throw std::invalid_argument(
                    "Attach input devices before starting RZX playback or recording"s);
        emulator.attachPeripheral(std::make_unique<Device>());
        found = emulator.findPeripheral(id);
    }
    return found ? &static_cast<Device &>(found->get()) : nullptr;
}

struct HeldInput
{
    Emulator &emulator;
    const InputPhase &phase;
    KempstonJoystickPeripheral *joystick;
    KempstonMousePeripheral *mouse;
    std::vector<ZxKey> newlyPressed;
    uint8_t oldButtons = 0;

    void press()
    {
        newlyPressed.reserve(phase.keys.size());
        for (const auto key : phase.keys) {
            const auto code = uint8_t(key);
            const auto select = uint8_t(~(1u << (code >> 4)));
            if (emulator.keyboard().readRows(select) & (1u << (code & 7))) {
                newlyPressed.push_back(key);
                emulator.setKeyState(key, true);
            }
        }
        for (const auto key : phase.nextKeys)
            emulator.setNextKeyState(key, true);
        if (phase.joystick) {
            oldButtons = joystick->buttons();
            joystick->setButtons(*phase.joystick);
        }
        if (phase.mouse) {
            mouse->move(phase.mouse->dx, phase.mouse->dy);
            for (uint8_t bit : { 1, 2, 4 })
                mouse->setButton(bit, (phase.mouse->buttons & bit) != 0);
        }
    }

    ~HeldInput()
    {
        for (const auto key : newlyPressed)
            emulator.setKeyState(key, false);
        for (const auto key : phase.nextKeys)
            emulator.setNextKeyState(key, false);
        if (phase.joystick)
            joystick->setButtons(oldButtons);
        if (phase.mouse)
            for (uint8_t bit : { 1, 2, 4 })
                mouse->setButton(bit, false);
    }
};
} // namespace

std::string Tools::attachInputDevices(std::string sessionId, bool joystick, bool mouse)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        return Object{ { "joystick",
                         inputDevice<KempstonJoystickPeripheral>(emulator, "kempston"sv, joystick)
                                 != nullptr },
                       { "mouse",
                         inputDevice<KempstonMousePeripheral>(emulator, "kempston_mouse"sv, mouse)
                                 != nullptr } };
    });
}

std::string Tools::runInputSequence(std::string sessionId, std::string actionsJson, int maxFrames,
                                    int maxMilliseconds)
{
    return result([&] {
        checked(maxFrames, 1, 10000, "maxFrames"sv);
        checked(maxMilliseconds, 1, 30000, "maxMilliseconds"sv);
        if (actionsJson.size() > 65536)
            throw std::invalid_argument("Input sequence JSON cannot exceed 64 KiB"s);
        const auto parsed = boost::json::parse(actionsJson);
        const auto &actions = parsed.as_array();
        if (actions.empty() || actions.size() > 512)
            throw std::invalid_argument("Supply 1 to 512 input actions"s);

        auto &current = session(sessionId);
        auto &emulator = *current.emulator;
        if (emulator.isRzxSeeking() || emulator.rzx().isPlaying() || emulator.rzx().isReviewing())
            throw std::invalid_argument(
                    "Stop replay or resume recording before sending live inputs"s);
        bool needsJoystick = false;
        bool needsMouse = false;
        std::vector<InputPhase> phases;
        for (const auto &value : actions) {
            const auto &action = value.as_object();
            const int frames = inputInteger(action.at("frames"), 1, 10000, "frames"sv);
            InputPhase phase{ .frames = frames };
            if (action.size() == 1) {
                phases.push_back(std::move(phase));
                continue;
            }
            if (action.size() != 2)
                throw std::invalid_argument(
                        "An input action uses frames and at most one input field"s);
            if (const auto *keys = action.if_contains("keys"))
                phase.keys = parseKeyChord(keys->as_string());
            else if (const auto *keys = action.if_contains("nextKeys")) {
                if (!emulator.machine().supportsZ80n())
                    throw std::invalid_argument("Extended keys require a ZX Spectrum Next"s);
                phase.nextKeys = nextChord(keys->as_string());
            } else if (const auto *text = action.if_contains("text")) {
                const auto &characters = text->as_string();
                if (characters.empty() || phases.size() + 2 * characters.size() > 4096)
                    throw std::invalid_argument(
                            "Supply nonempty text and at most 4096 input phases"s);
                for (char character : characters) {
                    phases.push_back({ .frames = frames,
                                       .keys = parseKeyChord(textChord(character)),
                                       .endsAction = false });
                    phases.push_back({ .frames = frames, .endsAction = false });
                }
                phases.back().endsAction = true;
                continue;
            } else if (const auto *buttons = action.if_contains("joystick")) {
                phase.joystick = inputInteger(*buttons, 0, 255, "joystick"sv);
                needsJoystick = true;
            } else if (const auto *mouse = action.if_contains("mouse")) {
                const auto &motion = mouse->as_object();
                if (motion.size() != 3)
                    throw std::invalid_argument("Mouse input requires dx, dy and buttons"s);
                phase.mouse = MouseInput{ inputInteger(motion.at("dx"), -255, 255, "dx"sv),
                                          inputInteger(motion.at("dy"), -255, 255, "dy"sv),
                                          inputInteger(motion.at("buttons"), 0, 7, "buttons"sv) };
                needsMouse = true;
            } else
                throw std::invalid_argument("Unknown input action"s);
            phases.push_back(std::move(phase));
        }

        const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMilliseconds);
        auto *joystick =
                inputDevice<KempstonJoystickPeripheral>(emulator, "kempston"sv, needsJoystick);
        auto *mouse =
                inputDevice<KempstonMousePeripheral>(emulator, "kempston_mouse"sv, needsMouse);
        int completedFrames = 0;
        int completedActions = 0;
        std::string reason = "sequenceComplete"s;
        for (const auto &phase : phases) {
            if (completedFrames == maxFrames) {
                reason = "frameBudget"s;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                reason = "timeBudget"s;
                break;
            }
            const auto frames = std::min(maxFrames - completedFrames, phase.frames);
            HeldInput held{ emulator, phase, joystick, mouse, {}, 0 };
            held.press();
            auto progress = executeUntil(current, frames, deadline).as_object();
            const int ran = int(progress.at("completedFrames").as_int64());
            const auto &stop = progress.at("stopReason").as_string();
            completedFrames += ran;
            if (ran == phase.frames && phase.endsAction)
                ++completedActions;
            if (stop != "frameBudget"sv || frames != phase.frames) {
                reason = stop;
                break;
            }
        }
        auto output = status(current).as_object();
        output["completedFrames"] = completedFrames;
        output["completedActions"] = completedActions;
        output["stopReason"] = reason;
        return output;
    });
}
} // namespace zxkit::mcp_tool
