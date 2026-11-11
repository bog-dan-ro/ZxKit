// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "Tools.h"
#include <mcp++/mcp.hpp>
#include <mcp++/json/boost.hpp>
#include <iostream>
#include <optional>

using namespace zxkit::mcp_tool;
using namespace std::literals;

namespace {
Json error(Json id, int code, std::string_view message)
{
    return Object{ { "jsonrpc", "2.0" },
                   { "id", std::move(id) },
                   { "error", Object{ { "code", code }, { "message", message } } } };
}

std::string base64(std::span<const uint8_t> bytes)
{
    constexpr auto alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"sv;
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        uint32_t value = uint32_t(bytes[offset]) << 16;
        if (offset + 1 < bytes.size())
            value |= uint32_t(bytes[offset + 1]) << 8;
        if (offset + 2 < bytes.size())
            value |= bytes[offset + 2];
        encoded += alphabet[(value >> 18) & 63];
        encoded += alphabet[(value >> 12) & 63];
        encoded += offset + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
        encoded += offset + 2 < bytes.size() ? alphabet[value & 63] : '=';
    }
    return encoded;
}

// MCP++ currently exposes scalar returns. Tool implementations return JSON text;
// adapt it once here to ordinary MCP structured results and optional image content.
void normalizeResult(Json &response, const Json &request, bool legacy)
{
    auto result = response.as_object().if_contains("result");
    if (!result || !result->is_object())
        return;
    auto &object = result->as_object();
    if (auto structured = object.if_contains("structuredContent");
        structured && structured->is_object()) {
        auto returned = structured->as_object().if_contains("result");
        if (returned && returned->is_string()) {
            auto decoded = boost::json::parse(returned->as_string());
            if (decoded.is_object()) {
                object["isError"] = decoded.as_object().contains("error");
                auto serialized = boost::json::serialize(decoded);
                object["structuredContent"] = decoded;
                Array content{ Object{ { "type", "text" }, { "text", serialized } } };
                auto &params = request.at("params").as_object();
                auto tool = params.if_contains("name");
                if (!decoded.as_object().contains("error") && tool && tool->is_string()
                    && (tool->as_string() == "captureScreen"sv
                        || tool->as_string() == "renderScreen"sv
                        || tool->as_string() == "renderGraphics"sv)) {
                    auto bytes = readFile(std::string(decoded.at("path").as_string()));
                    content.push_back(Object{ { "type", "image" },
                                              { "mimeType", "image/png" },
                                              { "data", base64(bytes) } });
                }
                object["content"] = content;
            }
        }
    }
    if (legacy) {
        object.erase("resultType");
        object.erase("ttlMs");
        object.erase("cacheScope");
        object.erase("_meta");
    }
}

class Protocol
{
public:
    explicit Protocol(Tools &tools) : m_server("zx-mcp"s, "1.0.0"s)
    {
        m_server.expose(tools);
    }

    Json catalog() const
    {
        return m_server.list_tools();
    }

    std::optional<Json> dispatch(Json request)
    {
        if (!request.is_object())
            return error(nullptr, -32600, "Expected a JSON-RPC object"sv);
        auto &object = request.as_object();
        Json id = object.contains("id") ? object.at("id") : Json(nullptr);
        bool validId = id.is_string() || id.is_int64() || id.is_uint64();
        auto method = object.if_contains("method");
        auto version = object.if_contains("jsonrpc");
        if (!method || !method->is_string() || !version || !version->is_string()
            || version->as_string() != "2.0"sv || (object.contains("id") && !validId))
            return error(validId ? id : Json(nullptr), -32600, "Invalid JSON-RPC request"sv);
        if (!object.contains("id")) {
            // Notifications never invoke tools or mutate emulator state.
            return std::nullopt;
        }
        const std::string methodName(method->as_string());
        if (!object.contains("params"))
            object["params"] = Object{};
        if (!object.at("params").is_object())
            return error(id, -32602, "params must be an object"sv);
        auto &params = object.at("params").as_object();
        if (methodName == "initialize"sv) {
            if (m_initialized)
                return error(id, -32600, "Already initialized"sv);
            auto requested = params.if_contains("protocolVersion");
            auto capabilities = params.if_contains("capabilities");
            auto client = params.if_contains("clientInfo");
            if (!requested || !requested->is_string() || !capabilities || !capabilities->is_object()
                || !client || !client->is_object() || !client->as_object().contains("name")
                || !client->at("name").is_string() || !client->as_object().contains("version")
                || !client->at("version").is_string())
                return error(id, -32602,
                             "initialize requires protocolVersion, capabilities and clientInfo"sv);
            std::string negotiated = "2025-11-25"s;
            for (auto supported :
                 { "2024-11-05"sv, "2025-03-26"sv, "2025-06-18"sv, "2025-11-25"sv })
                if (requested->as_string() == supported)
                    negotiated = supported;
            m_initialized = true;
            return Object{
                { "jsonrpc", "2.0" },
                { "id", id },
                { "result",
                  Object{ { "protocolVersion", negotiated },
                          { "capabilities", Object{ { "tools", Object{} } } },
                          { "serverInfo", Object{ { "name", "zx-mcp" }, { "version", "1.0.0" } } },
                          { "instructions",
                            "Create sessions with createSession. Use explicit sessionId values "
                            "when comparing machines; empty sessionId uses useSession's default. "
                            "Emulators advance only during bounded execution tools. All listed "
                            "input properties are required. Hex strings contain byte pairs; "
                            "addresses are JSON integers. Relative output paths use --output-dir. "
                            "Embedded 48K/128K ROMs are ready without external files." } } }
            };
        }
        if (methodName == "ping"sv)
            return Object{ { "jsonrpc", "2.0" }, { "id", id }, { "result", Object{} } };
        if (params.contains("_meta") && !params.at("_meta").is_object())
            return error(id, -32602, "_meta must be an object"sv);
        bool legacy = !params.contains("_meta")
                || !params.at("_meta").as_object().contains(
                        "io.modelcontextprotocol/protocolVersion");
        if (legacy) {
            if (!m_initialized)
                return error(id, -32000,
                             "Call initialize first, or supply native MCP++ metadata"sv);
            if (!params.contains("_meta"))
                params["_meta"] = Object{};
            auto &metadata = params.at("_meta").as_object();
            metadata["io.modelcontextprotocol/protocolVersion"] = "2026-07-28";
            metadata["io.modelcontextprotocol/clientCapabilities"] = Object{};
        }
        auto response = m_server.dispatch(request);
        if (response)
            normalizeResult(*response, request, legacy);
        return response;
    }

private:
    mcp::server<mcp::boost_backend> m_server;
    bool m_initialized = false;
};
} // namespace

int main(int argc, char **argv)
{
    try {
        std::filesystem::path outputDirectory = "build/zx-mcp";
        std::filesystem::path romDirectory;
        bool list = false;
        for (int index = 1; index < argc; ++index) {
            std::string_view option(argv[index]);
            if (option == "--help"sv || option == "-h"sv) {
                std::cout << "zx-mcp [--output-dir DIR] [--roms DIR] [--list-tools]\n"
                             "MCP stdio server. stdout is reserved for JSON-RPC.\n"
                             "48K and 128K ROMs are embedded. Additional ROMs: --roms DIR.\n";
                return 0;
            }
            if (option == "--list-tools"sv)
                list = true;
            else if (option == "--output-dir"sv || option == "--roms"sv) {
                if (++index == argc)
                    throw std::invalid_argument("Missing option value"s);
                if (option == "--output-dir"sv)
                    outputDirectory = argv[index];
                else
                    romDirectory = argv[index];
            } else
                throw std::invalid_argument("Unknown option: "s + std::string(option));
        }
        Tools tools(outputDirectory, romDirectory);
        Protocol protocol(tools);
        if (list) {
            std::cout << boost::json::serialize(protocol.catalog()) << '\n';
            return 0;
        }
        // Bound incoming frames before allocating arbitrary input. Oversized lines
        // are drained so the next request can still be served.
        std::string line;
        bool oversized = false;
        char character;
        while (std::cin.get(character)) {
            if (character != '\n') {
                if (line.size() < 4 * 1024 * 1024)
                    line += character;
                else
                    oversized = true;
                continue;
            }
            std::optional<Json> response;
            if (oversized)
                response = error(nullptr, -32600, "Request exceeds 4 MiB"sv);
            else {
                boost::system::error_code parseError;
                auto request = boost::json::parse(line, parseError);
                if (parseError)
                    response = error(nullptr, -32700, "Parse error"sv);
                else {
                    try {
                        response = protocol.dispatch(std::move(request));
                    } catch (const std::exception &) {
                        response = error(nullptr, -32603, "Internal error"sv);
                    }
                }
            }
            if (response)
                std::cout << boost::json::serialize(*response) << '\n' << std::flush;
            line.clear();
            oversized = false;
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "zx-mcp: "sv << error.what() << '\n';
        return 1;
    }
}
