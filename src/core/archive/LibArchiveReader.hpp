#pragma once

#include "core/archive/IArchiveReader.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct archive;

namespace mavira::core::archive {

/// Streaming in-memory archive reader backed by `libarchive` for `.cbr` (RAR4/RAR5),
/// `.cb7` (7-Zip), and `.cbt` (TAR) containers.
/// Uses `archive_read_open_filename_w` with wide UTF-16 Win32 paths and streams entry
/// bytes directly into memory without extracting temporary files to disk.
class LibArchiveReader final : public IArchiveReader {
public:
    LibArchiveReader() = default;
    ~LibArchiveReader() override;

    LibArchiveReader(const LibArchiveReader&) = delete;
    LibArchiveReader& operator=(const LibArchiveReader&) = delete;
    LibArchiveReader(LibArchiveReader&&) = delete;
    LibArchiveReader& operator=(LibArchiveReader&&) = delete;

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
        std::size_t archiveIndex{0};
        std::string name{};
    };

    void closeStreamLocked() noexcept;
    [[nodiscard]] mavira::core::Result<void> reopenStreamLocked();
    [[nodiscard]] mavira::core::Result<std::vector<std::uint8_t>> readEntryByArchiveIndexLocked(
        std::size_t targetArchiveIndex,
        std::stop_token stopToken);

    mutable std::mutex mutex_{};
    bool isOpen_{false};
    std::wstring wideLongPath_{};
    struct ::archive* stream_{nullptr};
    std::size_t nextArchiveIndex_{0};
    std::vector<PageEntry> pages_{};
    std::optional<std::size_t> metadataArchiveIndex_{};
};

} // namespace mavira::core::archive
