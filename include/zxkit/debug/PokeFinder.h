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

#include "zxkit/memory/Mmu.h"

#include <cstdint>
#include <vector>

namespace zxkit {

struct PokeCandidate
{
    uint16_t page16k = 0;
    uint16_t offset = 0;
    uint8_t currentValue = 0;
};

struct PokPatch
{
    uint8_t bank = 8; // 8 = ignore bank (16-bit address)
    uint16_t address = 0;
    uint16_t value = 0; // 256 = ask user
    uint8_t original = 0;
};

struct PokTrainer
{
    std::string name;
    std::vector<PokPatch> patches;
};

/// Cheat / Trainer RAM search engine & .pok file parser (ported & modernized from FUSE/Spectacol).
class PokeFinder
{
public:
    /// Parses a standard Spectrum .pok trainer file (N = trainer name, M/Z = patch lines, Y = EOF).
    static std::vector<PokTrainer> parsePokFile(std::string_view content);

    void reset(const Mmu &mmu, BankId ramBankId);
    void search(const Mmu &mmu, uint8_t targetValue);
    void searchIncremented(const Mmu &mmu);
    void searchDecremented(const Mmu &mmu);

    std::size_t count() const noexcept
    {
        return m_candidateCount;
    }

    std::vector<PokeCandidate> results(std::size_t maxResults = 256) const;

private:
    template <typename Pred> void filterComparison(const Mmu &mmu, Pred pred)
    {
        if (m_ramBankId == 0)
            return;
        const auto ramBytes = mmu.bank(m_ramBankId).bytes();
        std::size_t count = 0;
        for (std::size_t i = 0; i < m_snapshot.size() && i < ramBytes.size(); ++i) {
            if (m_valid[i]) {
                if (pred(ramBytes[i], m_snapshot[i])) {
                    m_snapshot[i] = ramBytes[i];
                    ++count;
                } else {
                    m_valid[i] = false;
                }
            }
        }
        m_candidateCount = count;
    }

    BankId m_ramBankId = 0;
    std::vector<uint8_t> m_snapshot;
    std::vector<bool> m_valid;
    std::size_t m_candidateCount = 0;
};

} // namespace zxkit
