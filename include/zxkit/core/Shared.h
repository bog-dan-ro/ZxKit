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

#include "zxkit/core/SpinSharedMutex.h"

#include <SafeCpp.h>

#include <utility>

namespace zxkit {

/// An object shared between threads: its spin lock lives next to it and the
/// only way in is a lock guard, `writeLock()` or `readLock()` (see SafeCpp).
template <typename T> using Shared = safe::SharedPtr<T, SpinSharedMutex>;

/// Observes a `Shared` object without keeping it alive; `lock()` it to use it.
template <typename T> using Weak = safe::WeakPtr<T, SpinSharedMutex>;

template <typename T, typename... Args> Shared<T> makeShared(Args &&...args)
{
    return safe::makeShared<T, SpinSharedMutex>(std::forward<Args>(args)...);
}

} // namespace zxkit
