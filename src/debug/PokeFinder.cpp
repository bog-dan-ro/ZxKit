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

#include "zxkit/debug/PokeFinder.h"

#include <cstdio>

namespace zxkit {

std::vector<PokTrainer> PokeFinder::parsePokFile(std::string_view content)
{
    std::vector<PokTrainer> trainers;
    std::size_t pos = 0;
    while (pos < content.size()) {
        const auto nl = content.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos)
            ? content.substr(pos)
            : content.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? content.size() : (nl + 1);

        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty())
            continue;

        const char tag = line[0];
        if (tag == 'Y' || tag == 'y')
            break;

        if (tag == 'N' || tag == 'n') {
            std::string_view title = line.substr(1);
            while (!title.empty() && title.front() == ' ')
                title.remove_prefix(1);
            trainers.push_back(PokTrainer{std::string(title), {}});
        } else if ((tag == 'M' || tag == 'Z' || tag == 'm' || tag == 'z') && !trainers.empty()) {
            unsigned bank = 8, addr = 0, val = 0, orig = 0;
            std::string s(line.substr(1));
            if (std::sscanf(s.c_str(), "%u %u %u %u", &bank, &addr, &val, &orig) >= 3) {
                trainers.back().patches.push_back(PokPatch{
                    .bank = static_cast<uint8_t>(bank),
                    .address = static_cast<uint16_t>(addr),
                    .value = static_cast<uint16_t>(val),
                    .original = static_cast<uint8_t>(orig)
                });
            }
        }
    }
    return trainers;
}

void PokeFinder::reset(const Mmu &mmu, BankId ramBankId)
{
    m_ramBankId = ramBankId;
    const auto &ram = mmu.bank(ramBankId);
    m_snapshot.assign(ram.bytes().begin(), ram.bytes().end());
    m_valid.assign(m_snapshot.size(), true);
    m_candidateCount = m_snapshot.size();
}

void PokeFinder::search(const Mmu &mmu, uint8_t targetValue)
{
    if (m_ramBankId == 0)
        return;
    const auto ramBytes = mmu.bank(m_ramBankId).bytes();
    std::size_t count = 0;
    for (std::size_t i = 0; i < m_snapshot.size() && i < ramBytes.size(); ++i) {
        if (m_valid[i]) {
            if (ramBytes[i] == targetValue) {
                m_snapshot[i] = ramBytes[i];
                ++count;
            } else {
                m_valid[i] = false;
            }
        }
    }
    m_candidateCount = count;
}

void PokeFinder::searchIncremented(const Mmu &mmu)
{
    filterComparison(mmu, [](uint8_t cur, uint8_t prev) { return cur > prev; });
}

void PokeFinder::searchDecremented(const Mmu &mmu)
{
    filterComparison(mmu, [](uint8_t cur, uint8_t prev) { return cur < prev; });
}

std::vector<PokeCandidate> PokeFinder::results(std::size_t maxResults) const
{
    std::vector<PokeCandidate> res;
    for (std::size_t i = 0; i < m_snapshot.size() && res.size() < maxResults; ++i) {
        if (m_valid[i]) {
            res.push_back(PokeCandidate{
                .page16k = static_cast<uint16_t>(i / 16384),
                .offset = static_cast<uint16_t>(i % 16384),
                .currentValue = m_snapshot[i]
            });
        }
    }
    return res;
}

} // namespace zxkit
