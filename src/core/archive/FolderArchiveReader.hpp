#pragma once

#include "core/archive/IArchiveReader.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mavira::core::archive {

/// Direct reader for folders containing loose image files, sorted using natural page order.
class FolderArchiveReader final : public IArchiveReader {
public:
    FolderArchiveReader() = default;
    ~FolderArchiveReader() override = default;

    FolderArchiveReader(const FolderArchiveReader&) = delete;
    FolderArchiveReader& operator=(const FolderArchiveReader&) = delete;
    FolderArchiveReader(FolderArchiveReader&&) = delete;
    FolderArchiveReader& operator=(FolderArchiveReader&&) = delete;

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
        std::filesystem::path filePath{};
        std::string name{};
    };

    mutable std::mutex mutex_{};
    bool isOpen_{false};
    std::vector<PageEntry> pages_{};
    std::optional<std::filesystem::path> metadataPath_{};
};

} // namespace mavira::core::archive
