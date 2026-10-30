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

#include <format>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zxkit {

class Exception : public std::runtime_error {
public:
    explicit Exception(std::string_view message,
                       std::source_location location = std::source_location::current())
        : std::runtime_error(std::format("{} [{}:{}:{}]",
                                         message,
                                         location.file_name(),
                                         location.line(),
                                         location.function_name()))
        , m_message(message)
        , m_location(location)
    {
    }

    std::string_view rawMessage() const noexcept { return m_message; }
    const std::source_location &where() const noexcept { return m_location; }

private:
    std::string m_message;
    std::source_location m_location;
};

class IoException : public Exception {
public:
    using Exception::Exception;
};

class FormatException : public Exception {
public:
    using Exception::Exception;
};

class RomMissingException : public Exception {
public:
    using Exception::Exception;
};

class PeripheralException : public Exception {
public:
    using Exception::Exception;
};

class DebuggerException : public Exception {
public:
    using Exception::Exception;
};

class CancelledException : public Exception {
public:
    explicit CancelledException(std::string_view message = "Operation cancelled",
                                std::source_location location = std::source_location::current())
        : Exception(message, location)
    {
    }
};

class InvalidArgumentException : public Exception {
public:
    using Exception::Exception;
};

} // namespace zxkit
