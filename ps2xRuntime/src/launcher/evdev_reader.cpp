#include "evdev_reader.h"

#if defined(__linux__)
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>

namespace evin
{
    void Reader::close() { if (m_fd >= 0) { ::close(m_fd); m_fd = -1; } }
    namespace
    {
        bool bitIs(const unsigned long *bm, int code)
        {
            return (bm[code / (8 * sizeof(unsigned long))] >> (code % (8 * sizeof(unsigned long))) & 1UL) != 0;
        }

        // Standard gamepad button keycodes (linux/input-event-codes.h).
        int kcodeToBtn(int code)
        {
            switch (code)
            {
            case BTN_A: return BtnA;
            case BTN_B: return BtnB;
            case BTN_X: return BtnX;
            case BTN_Y: return BtnY;
            case BTN_TL: return BtnLB;
            case BTN_TR: return BtnRB;
            case BTN_TL2: return BtnLT;
            case BTN_TR2: return BtnRT;
            case BTN_SELECT: return BtnSelect;
            case BTN_START: return BtnStart;
            case BTN_THUMBL: return BtnLS;
            case BTN_THUMBR: return BtnRS;
            case BTN_DPAD_UP: return BtnDpadUp;
            case BTN_DPAD_DOWN: return BtnDpadDown;
            case BTN_DPAD_LEFT: return BtnDpadLeft;
            case BTN_DPAD_RIGHT: return BtnDpadRight;
            default: return -1;
            }
        }
    } // namespace

    int abscodeToAxis(int code)
    {
        switch (code)
        {
        case ABS_X: return AxisLX;
        case ABS_Y: return AxisLY;
        case ABS_RX: return AxisRX;
        case ABS_RY: return AxisRY;
        case ABS_Z: return AxisLT;
        case ABS_RZ: return AxisRT;
        default: return -1;
        }
    }

    std::vector<DeviceInfo> listDevices()
    {
        std::vector<DeviceInfo> out;
        const std::string dir = "/dev/input";
        for (const auto &entry : std::filesystem::directory_iterator(dir))
        {
            std::string base = entry.path().filename().string();
            if (base.rfind("event", 0) != 0)
                continue;
            const std::string node = entry.path().string();
            int fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0)
                continue;
            DeviceInfo d;
            d.node = node;
            char nm[256] = {};
            if (ioctl(fd, EVIOCGNAME(sizeof(nm)), nm) >= 0)
                d.name = nm;

            unsigned long absBits[KEY_CNT / (8 * sizeof(unsigned long)) + 1] = {};
            unsigned long relBits[KEY_CNT / (8 * sizeof(unsigned long)) + 1] = {};
            unsigned long keyBits[KEY_CNT / (8 * sizeof(unsigned long)) + 1] = {};
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits);
            ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits);
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);

            // Absolute axes: count + check the gamepad stick set.
            for (int c = 0; c < (int)(sizeof(absBits) * 8); ++c)
                if (bitIs(absBits, c))
                    ++d.axes;
            // isGamepad: has a stick (ABS_X/ABS_Y) or hat.
            if (bitIs(absBits, ABS_X) && bitIs(absBits, ABS_Y))
                d.isGamepad = true;
            if (bitIs(absBits, ABS_HAT0X) && bitIs(absBits, ABS_HAT0Y))
                d.isGamepad = true;
            // Plus a physical button cluster beyond D-pad (gamepads have BTN_* codes).
            if (d.isGamepad)
            {
                const int padBtns[] = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_START, BTN_SELECT};
                int n = 0;
                for (int c : padBtns)
                    if (bitIs(keyBits, c))
                        ++n;
                if (n < 2)
                    d.isGamepad = false; // abs axes but almost no pad buttons -> touchpad etc.
            }

            // Relative axes + button count.
            for (int c = 0; c < (int)(sizeof(relBits) * 8); ++c)
                if (bitIs(relBits, c))
                    ++d.relAxes;
            for (int c = 0; c < (int)(sizeof(keyBits) * 8); ++c)
                if (bitIs(keyBits, c))
                    ++d.buttons;

            // isKeyboard: any of KEY_A..KEY_Z present, matching Reader::open().
            // A full threshold (~16 letters) is too strict for remappers like
            // keyd, which virtualize the keyboard and expose a subset of keys;
            // the letter range is unique to real keyboards, so >=1 is safe.
            {
                for (int k = KEY_A; k <= KEY_Z; ++k)
                    if (bitIs(keyBits, k))
                    {
                        d.isKeyboard = true;
                        break;
                    }
            }
            // isMouse: relative X axis + primary button.
            if (bitIs(relBits, REL_X) && bitIs(keyBits, BTN_LEFT))
                d.isMouse = true;

            ::close(fd);
            out.push_back(std::move(d));
        }
        return out;
    }

    std::string pickKeyboardNode(const std::vector<DeviceInfo> &devices)
    {
        std::string fallback;
        for (const auto &d : devices)
        {
            if (!d.isKeyboard)
                continue;
            if (fallback.empty())
                fallback = d.node;
            // Prefer remap/virtual keyboards (keyd etc.): they deliver the
            // keys after remapping and without the physical grab contention.
            const std::string lower = d.name;
            if (lower.find("virtual") != std::string::npos ||
                lower.find("keyd") != std::string::npos)
                return d.node;
        }
        return fallback;
    }

    bool Reader::open(const std::string &node)
    {
        close();
        m_fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK);
        if (m_fd < 0)
            return false;
        m_btns.fill(0);
        m_axes.fill(0.0f);
        m_axisIdx.fill(-1);
        m_axisMin.fill(0);
        m_axisMax.fill(0);
        m_isKeyboard = false;

        // Query axis ranges for the standard axes.
        struct input_absinfo ai;
        static const int absMap[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ};
        for (int ra = 0; ra < 6; ++ra)
        {
            if (ioctl(m_fd, EVIOCGABS(absMap[ra]), &ai) >= 0)
            {
                m_axisIdx[ra] = absMap[ra];
                m_axisMin[ra] = ai.minimum;
                m_axisMax[ra] = ai.maximum;
            }
        }

        // A keyboard device exposes the KEY_A..KEY_Z (30..56) letter range.
        unsigned long keyBits[KEY_CNT / (8 * sizeof(unsigned long)) + 1] = {};
        if (ioctl(m_fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) >= 0)
        {
            for (int c = KEY_A; c <= KEY_Z && !m_isKeyboard; ++c)
                if (bitIs(keyBits, c))
                    m_isKeyboard = true;
        }
        return true;
    }

    bool Reader::update()
    {
        if (m_fd < 0)
            return false;
        input_event ev;
        for (;;)
        {
            ssize_t n = ::read(m_fd, &ev, sizeof(ev));
            if (n < (ssize_t)sizeof(ev))
                break;
            if (ev.type == EV_KEY)
            {
                // Which "button index" does this key map to? -1 -> keyboard key.
                const int rb = kcodeToBtn(ev.code);
                if (rb >= 0 && rb < 32)
                {
                    m_btns[rb] = ev.value != 0 ? 1 : 0;
                    if (ev.value != 0)
                        m_lastKey = ev.code;
                }
                else if (ev.value != 0)
                {
                    m_lastKey = ev.code;
                }
            }
            else if (ev.type == EV_ABS)
                noteAbs(ev.code, ev.value);
        }
        return false;
    }

    int Reader::takeLastKey()
    {
        const int k = m_lastKey;
        m_lastKey = -1;
        return k;
    }

    void Reader::noteAbs(int code, int value)
    {
        // D-pad hats: fold into buttons 12-15.
        if (code == ABS_HAT0X)
        {
            m_btns[BtnDpadLeft] = value < 0 ? 1 : 0;
            m_btns[BtnDpadRight] = value > 0 ? 1 : 0;
        }
        else if (code == ABS_HAT0Y)
        {
            m_btns[BtnDpadUp] = value < 0 ? 1 : 0;
            m_btns[BtnDpadDown] = value > 0 ? 1 : 0;
        }
        for (int ra = 0; ra < 8; ++ra)
        {
            if (m_axisIdx[ra] == code)
            {
                m_axes[ra] = normalize(value, ra);
                return;
            }
        }
        // Bind a fresh unbounded axis slot if one is open.
        for (int i = 0; i < 6; ++i)
        {
            if (m_axisIdx[i] < 0)
            {
                m_axisIdx[i] = code;
                m_axes[i] = normalize(value, i);
                break;
            }
        }
    }

    float Reader::normalize(int raw, int idx) const
    {
        if (idx < 0 || idx >= 8)
            return 0.0f;
        const int mn = m_axisMin[idx], mx = m_axisMax[idx];
        if (mx == mn)
            return 0.0f;
        if (idx == AxisLT || idx == AxisRT)
            return (float)(raw - mn) / (float)(mx - mn);   // triggers 0..1
        return (float)(raw - mn) / (float)(mx - mn) * 2.0f - 1.0f;
    }
} // namespace evin

#else
namespace evin
{
    std::vector<DeviceInfo> listDevices() { return {}; }
    std::string pickKeyboardNode(const std::vector<DeviceInfo> &) { return {}; }
    int abscodeToAxis(int) { return -1; }
    bool Reader::open(const std::string &) { return false; }
    void Reader::close() {}
    bool Reader::update() { return false; }
    int Reader::takeLastKey() { return -1; }
}
#endif
