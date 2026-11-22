#pragma once

#include "core/archive/IArchiveReader.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct zip;

namespace mavira::core::archive {

/// O(1) indexed ZIP/CBZ/EPUB archive reader backed by `libzip`.
/// Opens wide UTF-16 Win32 paths via `zip_source_win32w_create` + `zip_open_from_source`
/// and reads page bytes directly into memory without extracting temp files.
class ZipArchiveReader final : public IArchiveReader {
public:
    ZipArchiveReader() = default;
    ~ZipArchiveReader() override;

    ZipArchiveReader(const ZipArchiveReader&) = delete;
    ZipArchiveReader& operator=(const ZipArchiveReader&) = delete;
    ZipArchiveReader(ZipArchiveReader&&) = delete;
    ZipArchiveReader& operator=(ZipArchiveReader&&) = delete;

    [[nodiscard]] mavira::core::Result<void> open(const std::filesystem::path& path) override;
    void close() noexcept override;
    [[nodiscard]] bool isOpen() const noexcept override;

    [[nodiscard]] std::size_t pageCount() const noexcept override;
    [[nodiscard]] std::string pageName(std::size_t index) const override;

    [[nodiscard]] mavira::core::Result<std::vector<std::uint8_t>> readPageBytes(
        std::size_t index,
        std::stop_token stopToken = {}) override;

    [[nodiscard]] mavira::core::Result<std::optional<std::string>> readMetadataXml() override;

private:
    struct PageEntry {
        std::uint64_t zipIndex{0};
        std::string name{};
    };

    [[nodiscard]] mavira::core::Result<std::vector<std::uint8_t>> readEntryBytesLocked(
        std::uint64_t zipIndex,
        std::stop_token stopToken);

    mutable std::mutex mutex_{};
    zip* archive_{nullptr};
    std::vector<PageEntry> pages_{};
    std::optional<std::uint64_t> metadataIndex_{};
};

} // namespace mavira::core::archive
