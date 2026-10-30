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

#include "zxkit/machines/CloneMachines.h"
#include "zxkit/machines/MachineProfile.h"
#include "zxkit/machines/RomanianMachines.h"
#include "zxkit/machines/SinclairMachines.h"
#include "zxkit/machines/ZxNextMachine.h"

namespace zxkit {

MachineRegistry::MachineRegistry()
{
    // Classic Sinclair machines
    registerMachine("16k",      [] { return std::make_unique<Spectrum48kMachine>(Spectrum48kMachine::Variant::Spec16k); });
    registerMachine("48k",      [] { return std::make_unique<Spectrum48kMachine>(Spectrum48kMachine::Variant::Spec48k); });
    registerMachine("48k_ntsc", [] { return std::make_unique<Spectrum48kMachine>(Spectrum48kMachine::Variant::Spec48kNtsc); });
    registerMachine("128k",     [] { return std::make_unique<Spectrum128kMachine>(false); });
    registerMachine("plus2",    [] { return std::make_unique<Spectrum128kMachine>(true); });
    registerMachine("plus2a",   [] { return std::make_unique<SpectrumPlus3Machine>(SpectrumPlus3Machine::Variant::Plus2A); });
    registerMachine("plus3",    [] { return std::make_unique<SpectrumPlus3Machine>(SpectrumPlus3Machine::Variant::Plus3); });
    registerMachine("plus3e",   [] { return std::make_unique<SpectrumPlus3Machine>(SpectrumPlus3Machine::Variant::Plus3e); });

    // Timex & Russian Clones
    registerMachine("tc2048",       [] { return std::make_unique<TimexMachine>(TimexMachine::Variant::Tc2048); });
    registerMachine("tc2068",       [] { return std::make_unique<TimexMachine>(TimexMachine::Variant::Tc2068); });
    registerMachine("ts2068",       [] { return std::make_unique<TimexMachine>(TimexMachine::Variant::Ts2068); });
    registerMachine("se",           [] { return std::make_unique<TimexMachine>(TimexMachine::Variant::SpecSe); });
    registerMachine("pentagon",     [] { return std::make_unique<PentagonMachine>(PentagonMachine::Variant::Pentagon128); });
    registerMachine("pentagon512",  [] { return std::make_unique<PentagonMachine>(PentagonMachine::Variant::Pentagon512); });
    registerMachine("pentagon1024", [] { return std::make_unique<PentagonMachine>(PentagonMachine::Variant::Pentagon1024); });
    registerMachine("scorpion",     [] { return std::make_unique<ScorpionMachine>(); });

    // Romanian Clones (ICE Felix HC & Brasov CoBra)
    registerMachine("hc2000",     [] { return std::make_unique<HcMachine>(); });
    registerMachine("cobra1_64k", [] { return std::make_unique<CobraMachine>(false); });
    registerMachine("cobra1_80k", [] { return std::make_unique<CobraMachine>(true); });
    registerMachine("cobra2",     [] { return std::make_unique<Cobra2Machine>(); });

    // ZX Spectrum Next
    registerMachine("next", [] { return std::make_unique<ZxNextMachine>(); });
}

} // namespace zxkit
