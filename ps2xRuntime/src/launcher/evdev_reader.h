#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Minimal evdev reader for the launcher's gamepad test + bind capture. Mirrors
// the runtime's pad_evdev_linux.cpp behaviour but without raylib: enumerates
// /dev/input/event* nodes, maps the standard Linux input codes onto the same
// logical layout raylib uses for gamepads (buttons 0..31, axes 0..5).
namespace evin
{
#if defined(__linux__)
    inline constexpr bool available = true;
#else
    inline constexpr bool available = false;
#endif
    // raylib-style button indices for the standard controller layout.
    enum : int
    {
        BtnY     = 0, // Y/Triangle
        BtnB     = 1, // B/Circle
        BtnA     = 2, // A/Cross
        BtnX     = 3, // X/Square
        BtnLT    = 4,
        BtnRT    = 5,
        BtnLB    = 6,
        BtnRB    = 7,
        BtnSelect= 8,
        BtnStart = 9,
        BtnLS    = 10,
        BtnRS    = 11,
        BtnDpadUp= 12,
        BtnDpadDown = 13,
        BtnDpadLeft = 14,
        BtnDpadRight = 15,
    };

    enum : int
    {
        AxisLX = 0,
        AxisLY = 1,
        AxisRX = 2,
        AxisRY = 3,
        AxisLT = 4,
        AxisRT = 5,
    };

    struct DeviceInfo
    {
        std::string node;   // /dev/input/eventN
        std::string name;   // sysfs name
        int axes = 0;
        int buttons = 0;
        int relAxes = 0;    // relative axes (mice)
        bool isGamepad = false; // gamepad-like: ABS_X/Y + BTN_GAMEPAD/triggers
        bool isKeyboard = false;// has KEY_A..KEY_Z range
        bool isMouse = false;   // REL_X + BTN_LEFT
    };

    std::vector<DeviceInfo> listDevices();
    // Pick the node to use for keyboard input from a listDevices() result.
    // Prefers the remap/virtual keyboard (keyd) when present, since keyd grabs
    // the physical device and delivers remapped keys on its virtual node.
    std::string pickKeyboardNode(const std::vector<DeviceInfo> &devices);
    // Linux input code (EV_ABS) -> raylib-like axis index, or -1.
    int abscodeToAxis(int code);

    class Reader
    {
    public:
        bool open(const std::string &node);
        void close();
        bool isOpen() const { return m_fd >= 0; }
        bool update(); // drain pending events; true if a key was pressed

        bool buttonDown(int rb) const { return rb >= 0 && rb < 32 && (m_btns[rb] != 0); }
        float axis(int ra) const { return ra >= 0 && ra < 8 ? m_axes[ra] : 0.0f; }
        // Most recent EV_KEY that is not a gamepad BTN_* code (i.e. a keyboard
        // key). Cleared by takeLastKey(). -1 when none.
        int peekLastKey() const { return m_lastKey; }
        int takeLastKey();
        void clearLastKey() { m_lastKey = -1; }
        // True when the opened node looks like a keyboard (has KEY_A..KEY_Z).
        bool isKeyboardDevice() const { return m_isKeyboard; }

    private:
        int m_fd = -1;
        bool m_isKeyboard = false;
        std::array<float, 8> m_axes{};
        std::array<int, 8> m_axisIdx{};   // evdev ABS code -> raylib axis
        std::array<int, 8> m_axisMin{};
        std::array<int, 8> m_axisMax{};
        std::array<uint8_t, 32> m_btns{};
        int m_lastKey = -1;
        void noteAbs(int code, int value);
        float normalize(int raw, int idx) const;
    };
} // namespace evin
