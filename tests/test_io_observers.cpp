// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include <zxkit/bus/IoBus.h>
#include <cassert>

int main()
{
    zxkit::IoBus bus;
    int permanent = 0;
    int temporary = 0;
    bus.addObserver([&](uint16_t, uint8_t, bool) { ++permanent; });
    auto subscription = bus.subscribe([&](uint16_t port, uint8_t value, bool write) {
        assert(port == 0x1234);
        assert(value == (write ? 0x42 : 0xFF));
        ++temporary;
    });
    bus.write(0x1234, 0x42);
    bus.peekPort(0x1234);
    assert(permanent == 1 && temporary == 1);
    bus.read(0x1234);
    assert(permanent == 2 && temporary == 2);

    // Copies own their subscription registry and can detach independently.
    zxkit::IoBus copy = bus;
    assert(copy.unsubscribe(subscription));
    copy.write(0x1234, 0x42);
    assert(permanent == 3 && temporary == 2);
    bus.write(0x1234, 0x42);
    assert(permanent == 4 && temporary == 3);
    assert(bus.unsubscribe(subscription));
    assert(!bus.unsubscribe(subscription));
    bus.write(0x5678, 0x12);
    assert(permanent == 5 && temporary == 3);

    auto stale = bus.subscribe([&](uint16_t, uint8_t, bool) { ++temporary; });
    bus.clear();
    auto fresh = bus.subscribe([&](uint16_t, uint8_t, bool) { ++temporary; });
    assert(stale != fresh && !bus.unsubscribe(stale));
    bus.write(0x1234, 0x42);
    assert(permanent == 5 && temporary == 4);
    assert(bus.unsubscribe(fresh));
    bus.write(0x1234, 0x42);
    assert(temporary == 4);
}
