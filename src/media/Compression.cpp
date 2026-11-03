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

#include "zxkit/media/Compression.h"

#include "zxkit/core/Exception.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <limits>
#include <zlib.h>

namespace zxkit {

std::vector<uint8_t> zlibCompress(std::span<const uint8_t> input, int level)
{
    uLongf bound = compressBound(static_cast<uLong>(input.size()));
    std::vector<uint8_t> out(bound);
    const int res = compress2(out.data(), &bound, input.data(), static_cast<uLong>(input.size()), level);
    if (res != Z_OK)
        throw FormatException("zlib compression failed");
    out.resize(bound);
    return out;
}

std::vector<uint8_t> zlibDecompress(std::span<const uint8_t> input, std::size_t expectedSize)
{
    if (input.empty())
        return {};

    z_stream strm{};
    strm.next_in = const_cast<Bytef *>(input.data());
    strm.avail_in = static_cast<uInt>(input.size());

    // 15 + 32 enables automatic zlib or gzip header detection
    if (inflateInit2(&strm, 15 + 32) != Z_OK)
        throw FormatException("zlib inflateInit2 failed");

    std::vector<uint8_t> out(std::max<std::size_t>(expectedSize, 4096));
    int ret = Z_OK;
    while (ret == Z_OK) {
        if (strm.total_out >= out.size())
            out.resize(out.size() * 2);
        strm.next_out = out.data() + strm.total_out;
        strm.avail_out = static_cast<uInt>(out.size() - strm.total_out);
        ret = inflate(&strm, Z_NO_FLUSH);
    }

    const std::size_t totalOut = strm.total_out;
    inflateEnd(&strm);

    if (ret != Z_STREAM_END)
        throw FormatException("zlib decompression failed: corrupt compressed stream");

    out.resize(totalOut);
    return out;
}

std::vector<uint8_t> zlibDecompressBounded(std::span<const uint8_t> input, std::size_t maxSize)
{
    if (input.size() > std::numeric_limits<uInt>::max())
        throw FormatException("Compressed media is too large");
    z_stream stream{};
    stream.next_in = const_cast<Bytef *>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    if (inflateInit(&stream) != Z_OK)
        throw FormatException("Cannot initialize media decompression");
    struct End { z_stream &stream; ~End() { inflateEnd(&stream); } } end{stream};
    std::vector<uint8_t> result;
    std::array<uint8_t, 16384> buffer;
    int status;
    do {
        stream.next_out = buffer.data();
        stream.avail_out = buffer.size();
        status = inflate(&stream, Z_NO_FLUSH);
        const auto count = buffer.size() - stream.avail_out;
        if (count > maxSize - result.size())
            throw FormatException("Decompressed media exceeds the size limit");
        result.insert(result.end(), buffer.begin(), buffer.begin() + count);
    } while (status == Z_OK);
    if (status != Z_STREAM_END || stream.avail_in != 0)
        throw FormatException("Invalid compressed media stream");
    return result;
}

uint32_t computeCrc32(std::span<const uint8_t> data) noexcept
{
    return static_cast<uint32_t>(crc32(0L, data.data(), static_cast<uInt>(data.size())));
}

ZipExtractedEntry extractFirstFromZip(std::span<const uint8_t> zipBytes)
{
    if (zipBytes.size() < 30 || zipBytes[0] != 'P' || zipBytes[1] != 'K' || zipBytes[2] != 0x03 || zipBytes[3] != 0x04)
        throw FormatException("Invalid ZIP local file header signature");

    const uint16_t method = static_cast<uint16_t>(zipBytes[8] | (zipBytes[9] << 8));
    const uint32_t compSize = static_cast<uint32_t>(zipBytes[18])
                            | (static_cast<uint32_t>(zipBytes[19]) << 8)
                            | (static_cast<uint32_t>(zipBytes[20]) << 16)
                            | (static_cast<uint32_t>(zipBytes[21]) << 24);
    const uint32_t uncompSize = static_cast<uint32_t>(zipBytes[22])
                              | (static_cast<uint32_t>(zipBytes[23]) << 8)
                              | (static_cast<uint32_t>(zipBytes[24]) << 16)
                              | (static_cast<uint32_t>(zipBytes[25]) << 24);
    const uint16_t nameLen = static_cast<uint16_t>(zipBytes[26] | (zipBytes[27] << 8));
    const uint16_t extraLen = static_cast<uint16_t>(zipBytes[28] | (zipBytes[29] << 8));

    const std::size_t payloadOffset = 30u + nameLen + extraLen;
    if (payloadOffset + compSize > zipBytes.size())
        throw FormatException("Truncated ZIP entry payload");

    ZipExtractedEntry entry;
    entry.filename.assign(zipBytes.begin() + 30, zipBytes.begin() + 30 + nameLen);
    const auto payload = zipBytes.subspan(payloadOffset, compSize);

    if (method == 0) {
        entry.data.assign(payload.begin(), payload.end());
    } else if (method == 8) {
        z_stream strm{};
        strm.next_in = const_cast<Bytef *>(payload.data());
        strm.avail_in = static_cast<uInt>(payload.size());
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK)
            throw FormatException("zlib raw inflateInit2 failed for ZIP");
        entry.data.resize(std::max<std::size_t>(uncompSize, 1));
        strm.next_out = entry.data.data();
        strm.avail_out = static_cast<uInt>(entry.data.size());
        const int ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret != Z_STREAM_END)
            throw FormatException("ZIP deflate decompression failed");
        entry.data.resize(uncompSize);
    } else {
        throw FormatException("Unsupported ZIP compression method");
    }
    return entry;
}

namespace {

uint16_t rd16(std::span<const uint8_t> bytes, std::size_t offset = 0) noexcept
{
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t rd32(std::span<const uint8_t> bytes, std::size_t offset = 0) noexcept
{
    return static_cast<uint32_t>(rd16(bytes, offset)) | (static_cast<uint32_t>(rd16(bytes, offset + 2)) << 16);
}

uint64_t rd64(std::span<const uint8_t> bytes, std::size_t offset = 0) noexcept
{
    return static_cast<uint64_t>(rd32(bytes, offset)) | (static_cast<uint64_t>(rd32(bytes, offset + 4)) << 32);
}

bool readAt(std::ifstream &f, uint64_t offset, std::span<uint8_t> out)
{
    f.clear();
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<std::size_t>(f.gcount()) == out.size();
}

} // namespace

void extractZipEntryToFile(const std::filesystem::path &zipPath,
                           std::string_view entryName,
                           const std::filesystem::path &outPath,
                           const ZipProgressFn &progress)
{
    std::ifstream zip(zipPath, std::ios::binary);
    if (!zip)
        throw IoException(std::format("Cannot open '{}'", zipPath.string()));
    zip.seekg(0, std::ios::end);
    const uint64_t zipSize = static_cast<uint64_t>(zip.tellg());

    // End Of Central Directory record, possibly followed by a comment
    const std::size_t tailSize = static_cast<std::size_t>(std::min<uint64_t>(zipSize, 22 + 65535));
    std::vector<uint8_t> tail(tailSize);
    if (!readAt(zip, zipSize - tailSize, tail))
        throw FormatException("Truncated ZIP file");
    std::size_t eocd = std::string::npos;
    for (std::size_t i = tailSize >= 22 ? tailSize - 22 : 0;; --i) {
        if (rd32(tail, i) == 0x06054B50) {
            eocd = i;
            break;
        }
        if (i == 0)
            break;
    }
    if (eocd == std::string::npos)
        throw FormatException("Not a ZIP file (no end of central directory)");
    uint64_t cdOffset = rd32(tail, eocd + 16);
    uint64_t cdSize = rd32(tail, eocd + 12);
    if (cdOffset == 0xFFFFFFFF && eocd >= 20 && rd32(tail, eocd - 20) == 0x07064B50) {
        // ZIP64 end of central directory
        std::array<uint8_t, 56> z64;
        if (!readAt(zip, rd64(tail, eocd - 20 + 8), z64) || rd32(z64) != 0x06064B50)
            throw FormatException("Corrupt ZIP64 end of central directory");
        cdSize = rd64(z64, 40);
        cdOffset = rd64(z64, 48);
    }
    if (cdOffset + cdSize > zipSize)
        throw FormatException("Corrupt ZIP central directory");

    std::vector<uint8_t> cd(static_cast<std::size_t>(cdSize));
    if (!readAt(zip, cdOffset, cd))
        throw FormatException("Truncated ZIP central directory");

    const auto sameName = [entryName](std::string_view path) {
        if (const auto slash = path.find_last_of("/\\"); slash != std::string_view::npos)
            path.remove_prefix(slash + 1);
        return std::ranges::equal(path, entryName, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    };

    for (std::size_t pos = 0; pos + 46 <= cd.size();) {
        const auto h = std::span<const uint8_t>(cd).subspan(pos);
        if (rd32(h) != 0x02014B50)
            break;
        const uint16_t method = rd16(h, 10);
        const uint32_t crc = rd32(h, 16);
        uint64_t compSize = rd32(h, 20);
        uint64_t size = rd32(h, 24);
        const uint16_t nameLen = rd16(h, 28);
        const uint16_t extraLen = rd16(h, 30);
        const uint16_t commentLen = rd16(h, 32);
        uint64_t localOffset = rd32(h, 42);
        if (pos + 46 + nameLen + extraLen > cd.size())
            break;
        const std::string name(h.begin() + 46, h.begin() + 46 + nameLen);
        pos += 46u + nameLen + extraLen + commentLen;
        if (!sameName(name))
            continue;

        // ZIP64 extended information: only the fields saturated above are present
        const auto extra = h.subspan(46 + nameLen, extraLen);
        for (std::size_t x = 0; x + 4 <= extra.size();) {
            const uint16_t id = rd16(extra, x);
            const uint16_t len = rd16(extra, x + 2);
            std::size_t v = x + 4;
            if (id == 0x0001) {
                if (size == 0xFFFFFFFF && v + 8 <= extra.size()) { size = rd64(extra, v); v += 8; }
                if (compSize == 0xFFFFFFFF && v + 8 <= extra.size()) { compSize = rd64(extra, v); v += 8; }
                if (localOffset == 0xFFFFFFFF && v + 8 <= extra.size()) { localOffset = rd64(extra, v); }
            }
            x += 4u + len;
        }

        std::array<uint8_t, 30> local;
        if (!readAt(zip, localOffset, local) || rd32(local) != 0x04034B50)
            throw FormatException("Corrupt ZIP local header");
        uint64_t dataOffset = localOffset + 30 + rd16(local, 26) + rd16(local, 28);
        if (method != 0 && method != 8)
            throw FormatException("Unsupported ZIP compression method");

        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out)
            throw IoException(std::format("Cannot create '{}'", outPath.string()));

        z_stream strm{};
        if (method == 8 && inflateInit2(&strm, -MAX_WBITS) != Z_OK)
            throw FormatException("zlib raw inflateInit2 failed for ZIP");
        std::vector<uint8_t> in(256 * 1024);
        std::vector<uint8_t> buf(1024 * 1024);
        uint64_t remaining = compSize;
        uint64_t written = 0;
        uLong outCrc = crc32(0L, Z_NULL, 0);
        bool ok = true;
        bool finished = false;
        bool cancelled = false;
        zip.clear();
        zip.seekg(static_cast<std::streamoff>(dataOffset));
        while (ok && !finished) {
            const std::size_t chunk = static_cast<std::size_t>(std::min<uint64_t>(remaining, in.size()));
            zip.read(reinterpret_cast<char *>(in.data()), static_cast<std::streamsize>(chunk));
            if (static_cast<std::size_t>(zip.gcount()) != chunk) {
                ok = false;
                break;
            }
            remaining -= chunk;
            const auto emit = [&](std::span<const uint8_t> data) {
                out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
                outCrc = crc32(outCrc, data.data(), static_cast<uInt>(data.size()));
                written += data.size();
                if (progress && !progress(written, size))
                    cancelled = true;
                return static_cast<bool>(out) && !cancelled;
            };
            if (method == 0) {
                ok = emit(std::span(in).first(chunk));
                finished = (remaining == 0);
                continue;
            }
            strm.next_in = in.data();
            strm.avail_in = static_cast<uInt>(chunk);
            do {
                strm.next_out = buf.data();
                strm.avail_out = static_cast<uInt>(buf.size());
                const int ret = inflate(&strm, Z_NO_FLUSH);
                if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                    ok = false;
                    break;
                }
                ok = emit(std::span(buf).first(buf.size() - strm.avail_out));
                if (ret == Z_STREAM_END)
                    finished = true;
            } while (ok && !finished && strm.avail_out == 0);
            if (!finished && remaining == 0)
                ok = false;
        }
        if (method == 8)
            inflateEnd(&strm);
        out.close();
        if (!ok || written != size || outCrc != crc) {
            std::error_code ec;
            std::filesystem::remove(outPath, ec);
            if (cancelled)
                throw IoException(std::format("Extracting '{}' was cancelled", name));
            throw FormatException(std::format("'{}' in '{}' is corrupt", name, zipPath.string()));
        }
        return;
    }
    throw FormatException(std::format("'{}' not found in '{}'", entryName, zipPath.string()));
}

} // namespace zxkit
