/*
    Copyright (c) 2026, BogDan Vatra <bogdan@kde.org>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "zxkit/bus/Peripheral.h"
#include "zxkit/cpu/Z80Cpu.h"

#include <cstdint>

namespace zxkit {

/// Romantic Robot Multiface 1 / 128 / 3 Peripheral
/// Supports NMI button activation, 8KB ROM + 8KB RAM overlay at 0x0000..0x3FFF,
/// M1 page-in at 0x0066, RETN unmap, and model-specific I/O ports.
class MultifacePeripheral : public Peripheral
{
public:
    enum class Model { Multiface1, Multiface128, Multiface3 };

    explicit MultifacePeripheral(Model model = Model::Multiface128) noexcept;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    void pressRedButton(Z80Cpu &cpu) noexcept;

    BankId romBankId() const noexcept;
    BankId ramBankId() const noexcept;
    bool isPagedIn() const noexcept;

private:
    Model m_model = Model::Multiface128;
    BankId m_romBank = 0;
    BankId m_ramBank = 0;
    bool m_nmiButtonPressed = false;
    bool m_pagedIn = false;
};

/// Currah uSource Peripheral (2KB ROM toggled by M1 fetch at 0x2BAE)
class UsourcePeripheral : public Peripheral
{
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    BankId romBankId() const noexcept;

private:
    BankId m_romBank = 0;
    bool m_active = false;
};

/// Sinclair ZX Printer Peripheral (Port 0xFB)
class ZxPrinterPeripheral : public Peripheral
{
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

private:
    uint8_t m_lastCommand = 0;
};

/// Sinclair ZX Interface 2 ROM Cartridge Peripheral (16KB ROM at 0x0000..0x3FFF)
class Interface2Peripheral : public Peripheral
{
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    void insertCartridge(MachineContext &ctx, std::span<const uint8_t> rom16k);
    void ejectCartridge(MachineContext &ctx);

    BankId romBankId() const noexcept;

private:
    BankId m_romBank = 0;
};

/// Cheetah SpecDrum 8-bit DAC Peripheral (Port 0xDF)
class SpecDrumPeripheral : public Peripheral
{
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;
};

/// Covox 8-bit Mono DAC Peripheral (Port 0xFB or 0xDD)
class CovoxPeripheral : public Peripheral
{
public:
    explicit CovoxPeripheral(uint8_t port = 0xFB) noexcept;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

private:
    uint8_t m_port = 0xFB;
};

/// Fuller Box AY + Joystick Peripheral (AY on 0x3F/0x5F, Joystick on 0x7F)
class FullerBoxPeripheral : public Peripheral
{
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    /// The joystick, as `JoystickButtons` bits (Fire1 is its fire button).
    void setButtons(uint8_t buttons) noexcept;
    uint8_t buttons() const noexcept
    {
        return m_buttons;
    }

private:
    uint8_t m_buttons = 0;
    uint8_t m_joyState =
            0xFF; // port 0x7F, active low: up 0x01, down 0x02, left 0x04, right 0x08, fire 0x80
};

} // namespace zxkit
