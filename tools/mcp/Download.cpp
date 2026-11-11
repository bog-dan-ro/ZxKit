// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "Download.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <regex>
#include <stdexcept>

namespace zxkit::mcp_tool {
using namespace std::literals;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using Tcp = asio::ip::tcp;
namespace {
struct Url
{
    std::string scheme;
    std::string authority;
    std::string host;
    std::string port;
    std::string target;
};

Url parseUrl(std::string_view text)
{
    if (text.size() > 8192 || text.find_first_of("\r\n\t #") != text.npos)
        throw std::invalid_argument("URL contains unsupported characters"s);
    auto colon = text.find("://"sv);
    if (colon == text.npos)
        throw std::invalid_argument(
                "URL must use https:// (or loopback http:// for local sources)"s);
    Url url;
    url.scheme = text.substr(0, colon);
    text.remove_prefix(colon + 3);
    auto slash = text.find_first_of("/?");
    url.authority = text.substr(0, slash);
    url.target = slash == text.npos ? "/"s : std::string(text.substr(slash));
    if (url.target.starts_with('?'))
        url.target.insert(0, "/"sv);
    if (url.authority.empty() || url.authority.find('@') != std::string::npos)
        throw std::invalid_argument("URL host is missing or contains credentials"s);
    url.port = url.scheme == "https"sv ? "443"s : "80"s;
    if (url.authority.starts_with('[')) {
        auto close = url.authority.find(']');
        if (close == std::string::npos)
            throw std::invalid_argument("Invalid IPv6 URL"s);
        url.host = url.authority.substr(1, close - 1);
        if (close + 1 < url.authority.size()) {
            if (url.authority[close + 1] != ':')
                throw std::invalid_argument("Invalid URL port"s);
            url.port = url.authority.substr(close + 2);
        }
    } else {
        auto port = url.authority.find(':');
        url.host = url.authority.substr(0, port);
        if (port != std::string::npos)
            url.port = url.authority.substr(port + 1);
    }
    if (url.host.empty() || url.port.empty())
        throw std::invalid_argument("Invalid URL host/port"s);
    if (url.scheme != "https"sv
        && !(url.scheme == "http"sv
             && (url.host == "127.0.0.1"sv || url.host == "localhost"sv || url.host == "::1"sv)))
        throw std::invalid_argument("Downloads require HTTPS; HTTP is allowed only on loopback"s);
    return url;
}

std::string digestFile(const std::filesystem::path &path)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Cannot initialize SHA-256"s);
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot hash downloaded file"s);
    std::array<char, 65536> buffer;
    while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
        if (EVP_DigestUpdate(context.get(), buffer.data(), std::size_t(input.gcount())) != 1)
            throw std::runtime_error("Cannot compute SHA-256"s);
    }
    if (!input.eof())
        throw std::runtime_error("Cannot finish reading downloaded file"s);
    std::array<uint8_t, EVP_MAX_MD_SIZE> hash;
    unsigned length = 0;
    if (EVP_DigestFinal_ex(context.get(), hash.data(), &length) != 1)
        throw std::runtime_error("Cannot finish SHA-256"s);
    return hex(std::span(hash).first(length));
}

struct Response
{
    unsigned status = 0;
    std::string location;
    uint64_t bytes = 0;
};

template <typename Stream>
asio::awaitable<Response>
transfer(Stream &stream, const Url &url, const std::filesystem::path &partial,
         const std::filesystem::path &metadataPath, std::string originalUrl, uint64_t offset,
         std::string validator, uint64_t maximumBytes)
{
    http::request<http::empty_body> request{ http::verb::get, url.target, 11 };
    request.set(http::field::host, url.authority);
    request.set(http::field::user_agent, "zx-mcp/1.0");
    request.set(http::field::accept_encoding, "identity");
    request.keep_alive(false);
    if (offset) {
        request.set(http::field::range, "bytes="s + std::to_string(offset) + "-"s);
        if (!validator.empty())
            request.set(http::field::if_range, validator);
    }
    co_await http::async_write(stream, request, asio::use_awaitable);
    beast::flat_buffer input;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(maximumBytes);
    parser.header_limit(32768);
    co_await http::async_read_header(stream, input, parser, asio::use_awaitable);
    Response response{ parser.get().result_int(), std::string(parser.get()[http::field::location]),
                       0 };
    if (response.status >= 300 && response.status < 400)
        co_return response;
    if (response.status != 200 && response.status != 206)
        throw std::runtime_error("Download returned HTTP "s + std::to_string(response.status));
    auto encoding = parser.get()[http::field::content_encoding];
    if (!encoding.empty() && encoding != "identity")
        throw std::runtime_error("Unexpected HTTP content encoding"s);
    uint64_t expectedTotal = 0;
    if (response.status == 206) {
        const std::regex range(R"(^bytes ([0-9]+)-([0-9]+)/([0-9]+)$)");
        std::smatch match;
        auto header = std::string(parser.get()[http::field::content_range]);
        if (!offset || !std::regex_match(header, match, range))
            throw std::runtime_error("Invalid partial response Content-Range"s);
        auto first = std::stoull(match[1]);
        auto last = std::stoull(match[2]);
        expectedTotal = std::stoull(match[3]);
        if (first != offset || last < first || last + 1 != expectedTotal
            || expectedTotal > maximumBytes
            || (parser.content_length() && *parser.content_length() != last - first + 1))
            throw std::runtime_error("Partial response does not match the saved offset/size"s);
    } else {
        offset = 0; // A server ignoring Range must replace, never append, the full body.
        expectedTotal = parser.content_length().value_or(0);
        if (expectedTotal > maximumBytes)
            throw std::runtime_error("Download exceeds size limit"s);
    }
    auto token = parser.get()[http::field::etag];
    if (token.empty())
        token = parser.get()[http::field::last_modified];
    auto metadata = boost::json::serialize(
            Object{ { "url", originalUrl }, { "validator", std::string(token) } });
    writeFile(metadataPath,
              std::span(reinterpret_cast<const uint8_t *>(metadata.data()), metadata.size()), true);
    std::ofstream output(partial, std::ios::binary | (offset ? std::ios::app : std::ios::trunc));
    if (!output)
        throw std::runtime_error("Cannot open partial download"s);
    std::array<char, 65536> buffer;
    uint64_t written = offset;
    while (!parser.is_done()) {
        parser.get().body().data = buffer.data();
        parser.get().body().size = buffer.size();
        boost::system::error_code error;
        co_await http::async_read(stream, input, parser,
                                  asio::redirect_error(asio::use_awaitable, error));
        auto count = buffer.size() - parser.get().body().size;
        written += count;
        if (written > maximumBytes)
            throw std::runtime_error("Download exceeds size limit"s);
        if (!output.write(buffer.data(), std::streamsize(count)))
            throw std::runtime_error("Cannot write partial download"s);
        if (error && error != http::error::need_buffer)
            throw boost::system::system_error(error);
    }
    if (!output.flush())
        throw std::runtime_error("Cannot flush partial download"s);
    if (expectedTotal && written != expectedTotal)
        throw std::runtime_error("Incomplete download; the next call can resume it"s);
    response.bytes = written;
    co_return response;
}

Response fetch(const Url &url, const std::filesystem::path &partial,
               const std::filesystem::path &metadata, const std::string &originalUrl,
               uint64_t offset, const std::string &validator, uint64_t maximumBytes,
               std::chrono::steady_clock::time_point deadline)
{
    asio::io_context context;
    Tcp::resolver resolver(context);
    asio::steady_timer timer(context, deadline);
    timer.async_wait([&](boost::system::error_code error) {
        if (!error)
            resolver.cancel();
    });
    Response response;
    std::exception_ptr failure;
    auto task = [&]() -> asio::awaitable<void> {
        auto endpoints = co_await resolver.async_resolve(url.host, url.port, asio::use_awaitable);
        if (url.scheme == "https"sv) {
            ssl::context tls(ssl::context::tls_client);
            tls.set_default_verify_paths();
            tls.set_verify_mode(ssl::verify_peer);
            ssl::stream<beast::tcp_stream> stream(context, tls);
            stream.set_verify_callback(ssl::host_name_verification(url.host));
            if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str()))
                throw std::runtime_error("Cannot configure TLS server name"s);
            beast::get_lowest_layer(stream).expires_at(deadline);
            co_await beast::get_lowest_layer(stream).async_connect(endpoints, asio::use_awaitable);
            co_await stream.async_handshake(ssl::stream_base::client, asio::use_awaitable);
            response = co_await transfer(stream, url, partial, metadata, originalUrl, offset,
                                         validator, maximumBytes);
        } else {
            beast::tcp_stream stream(context);
            stream.expires_at(deadline);
            co_await stream.async_connect(endpoints, asio::use_awaitable);
            response = co_await transfer(stream, url, partial, metadata, originalUrl, offset,
                                         validator, maximumBytes);
        }
    };
    asio::co_spawn(context, task(), [&](std::exception_ptr error) {
        failure = error;
        timer.cancel();
    });
    context.run();
    if (failure)
        std::rethrow_exception(failure);
    return response;
}
} // namespace

std::string resolveUrl(std::string_view base, std::string_view location)
{
    auto origin = parseUrl(base);
    std::string resolved;
    if (location.starts_with("//"sv))
        resolved = origin.scheme + ":"s + std::string(location);
    else if (location.find("://"sv) != location.npos)
        resolved = location;
    else if (location.starts_with('/'))
        resolved = origin.scheme + "://"s + origin.authority + std::string(location);
    else
        resolved = origin.scheme + "://"s + origin.authority
                + origin.target.substr(0, origin.target.rfind('/') + 1) + std::string(location);
    auto next = parseUrl(resolved);
    if (origin.scheme == "https"sv && next.scheme != "https"sv)
        throw std::runtime_error("Refusing an HTTPS downgrade redirect"s);
    return resolved;
}

std::filesystem::path publishFile(const std::filesystem::path &temporary,
                                  const std::filesystem::path &output, bool overwrite)
{
    std::filesystem::path backup;
    if (std::filesystem::exists(output)) {
        if (!overwrite)
            throw std::runtime_error("Output exists; set overwrite=true"s);
        backup = output;
        backup += ".backup-"s
                + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::filesystem::rename(output, backup);
    }
    try {
        std::filesystem::rename(temporary, output);
    } catch (...) {
        if (!backup.empty())
            std::filesystem::rename(backup, output);
        throw;
    }
    return backup;
}

Object download(std::string url, const std::filesystem::path &output,
                std::string_view expectedSha256, int milliseconds, bool overwrite,
                uint64_t maximumBytes)
{
    checked(milliseconds, 1, 300000, "maxMilliseconds"sv);
    if (!expectedSha256.empty()
        && (expectedSha256.size() != 64 || unhex(expectedSha256).size() != 32))
        throw std::invalid_argument("Expected SHA-256 must be empty or 64 hexadecimal digits"s);
    if (!overwrite && std::filesystem::exists(output))
        throw std::runtime_error("Output exists; set overwrite=true"s);
    if (!output.parent_path().empty())
        std::filesystem::create_directories(output.parent_path());
    auto partial = output;
    partial += ".part";
    auto metadata = partial;
    metadata += ".json";
    uint64_t offset = 0;
    std::string validator;
    if (std::filesystem::exists(partial)) {
        if (overwrite) {
            std::filesystem::remove(partial);
            std::filesystem::remove(metadata);
        } else {
            auto bytes = readFile(metadata);
            auto saved = boost::json::parse(
                    std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
            if (saved.at("url").as_string() != url)
                throw std::runtime_error("Partial download belongs to another URL"s);
            validator = saved.at("validator").as_string();
            offset = std::filesystem::file_size(partial);
            if (offset >= maximumBytes)
                throw std::runtime_error("Partial download exceeds size limit"s);
        }
    }
    auto originalUrl = url;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    Response response;
    for (int redirects = 0;; ++redirects) {
        auto parsed = parseUrl(url);
        response = fetch(parsed, partial, metadata, originalUrl, offset, validator, maximumBytes,
                         deadline);
        if (response.status < 300 || response.status >= 400)
            break;
        if (redirects >= 5 || response.location.empty())
            throw std::runtime_error("Too many or invalid redirects"s);
        url = resolveUrl(url, response.location);
    }
    auto digest = digestFile(partial);
    if (!expectedSha256.empty() && upper(digest) != upper(std::string(expectedSha256)))
        throw std::runtime_error(
                "Downloaded SHA-256 does not match; existing output was preserved"s);
    auto backup = publishFile(partial, output, overwrite);
    std::filesystem::remove(metadata);
    return { { "path", output.string() }, { "url", url },
             { "bytes", response.bytes }, { "resumedFrom", response.status == 206 ? offset : 0 },
             { "sha256", digest },        { "backup", backup.string() } };
}
} // namespace zxkit::mcp_tool
