#include "core/cache/CacheManager.hpp"

#include "core/archive/ArchiveEngine.hpp"
#include "core/archive/PdfDocumentReader.hpp"
#include "core/imaging/ImageDecoder.hpp"
#include "data/fs/FileManager.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mavira::core::cache {

namespace {

using mavira::core::ComicFormat;
using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::Result;
using mavira::core::archive::ArchiveEngine;
using mavira::core::archive::PdfDocumentReader;
using mavira::core::imaging::ImageDecoder;
using mavira::data::db::ComicRecord;
using mavira::data::db::CoverPackLocationUpdate;
using mavira::data::db::DatabaseManager;
using mavira::data::fs::FileManager;

/// Extracts and decodes page 0 of `comic` into a `180x255` fit RGBA8888 thumbnail.
[[nodiscard]] Result<ImageFrame> extractCoverThumbnail(
    const ComicRecord& comic,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "Cover extraction cancelled");
    }

    const ComicFormat fmt = (comic.format != ComicFormat::Unknown)
        ? comic.format
        : ArchiveEngine::detectFormat(comic.filePath);

    if (fmt == ComicFormat::PDF) {
        PdfDocumentReader pdfReader;
        auto openRes = pdfReader.open(comic.filePath);
        if (!openRes.isOk()) {
            return Result<ImageFrame>::err(openRes.error());
        }
        if (pdfReader.pageCount() == 0) {
            return Result<ImageFrame>::err(ErrorCode::NotFound, "PDF has zero pages");
        }
        auto frameRes = pdfReader.renderPage(
            0, ICoverStorage::kCoverMaxWidth, ICoverStorage::kCoverMaxHeight, stopToken);
        if (!frameRes.isOk()) {
            return frameRes;
        }
        return Result<ImageFrame>::ok(ImageDecoder::resizeFit(
            frameRes.value(), ICoverStorage::kCoverMaxWidth, ICoverStorage::kCoverMaxHeight));
    }

    auto readerRes = ArchiveEngine::openArchive(comic.filePath);
    if (!readerRes.isOk()) {
        return Result<ImageFrame>::err(readerRes.error());
    }
    auto& reader = *readerRes.value();
    if (reader.pageCount() == 0) {
        return Result<ImageFrame>::err(ErrorCode::NotFound, "Archive contains no image pages");
    }

    if (stopToken.stop_requested()) {
        return Result<ImageFrame>::err(ErrorCode::Cancelled, "Cover extraction cancelled");
    }

    auto pageBytesRes = reader.readPageBytes(0, stopToken);
    if (!pageBytesRes.isOk()) {
        return Result<ImageFrame>::err(pageBytesRes.error());
    }

    auto decodedRes = ImageDecoder::decode(
        pageBytesRes.value(),
        ICoverStorage::kCoverMaxWidth,
        ICoverStorage::kCoverMaxHeight,
        stopToken);
    if (!decodedRes.isOk()) {
        return decodedRes;
    }

    return Result<ImageFrame>::ok(ImageDecoder::resizeFit(
        decodedRes.value(), ICoverStorage::kCoverMaxWidth, ICoverStorage::kCoverMaxHeight));
}

} // namespace

// =============================================================================
// PageRamCache (L1 Bounded Byte-Budget RAM LRU Cache)
// =============================================================================

PageRamCache::PageRamCache(std::size_t maxBytes) : maxBytes_(maxBytes) {}

void PageRamCache::setMemoryLimitBytes(std::size_t maxBytes) {
    std::scoped_lock lock(mutex_);
    maxBytes_ = maxBytes;
    evictIfNeededLocked();
}

std::size_t PageRamCache::memoryLimitBytes() const noexcept {
    std::scoped_lock lock(mutex_);
    return maxBytes_;
}

std::size_t PageRamCache::memoryUsageBytes() const noexcept {
    std::scoped_lock lock(mutex_);
    return usedBytes_;
}

std::size_t PageRamCache::entryCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

void PageRamCache::setCurrentReadingPosition(std::int64_t comicId, std::size_t pageIndex) noexcept {
    std::scoped_lock lock(mutex_);
    currentComicId_ = comicId;
    currentPageIndex_ = pageIndex;
}

void PageRamCache::put(const PageCacheKey& key, std::shared_ptr<const ImageFrame> frame) {
    if (!frame) {
        return;
    }

    const std::size_t frameBytes = frame->memoryBytes();
    std::scoped_lock lock(mutex_);

    if (auto it = entries_.find(key); it != entries_.end()) {
        usedBytes_ -= it->second.bytes;
        entries_.erase(it);
    }

    if (maxBytes_ == 0 || frameBytes > maxBytes_) {
        return;
    }

    entries_.emplace(key, Entry{std::move(frame), frameBytes, ++accessTick_});
    usedBytes_ += frameBytes;
    evictIfNeededLocked();
}

std::shared_ptr<const ImageFrame> PageRamCache::get(const PageCacheKey& key) {
    std::scoped_lock lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return nullptr;
    }
    it->second.lastAccessTick = ++accessTick_;
    return it->second.frame;
}

bool PageRamCache::contains(const PageCacheKey& key) const {
    std::scoped_lock lock(mutex_);
    return entries_.contains(key);
}

void PageRamCache::erase(const PageCacheKey& key) {
    std::scoped_lock lock(mutex_);
    if (auto it = entries_.find(key); it != entries_.end()) {
        usedBytes_ -= it->second.bytes;
        entries_.erase(it);
    }
}

void PageRamCache::eraseComic(std::int64_t comicId) {
    std::scoped_lock lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->first.comicId == comicId) {
            usedBytes_ -= it->second.bytes;
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    if (currentComicId_ == comicId) {
        currentComicId_ = 0;
        currentPageIndex_ = 0;
    }
}

void PageRamCache::clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    usedBytes_ = 0;
}

void PageRamCache::evictIfNeededLocked() {
    while (usedBytes_ > maxBytes_ && !entries_.empty()) {
        auto victim = entries_.end();
        bool victimOtherComic = false;
        std::size_t victimDist = 0;
        std::uint64_t victimTick = (std::numeric_limits<std::uint64_t>::max)();

        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            const bool otherComic = (it->first.comicId != currentComicId_);
            const std::size_t dist = otherComic
                ? (std::numeric_limits<std::size_t>::max)()
                : (it->first.pageIndex >= currentPageIndex_
                       ? it->first.pageIndex - currentPageIndex_
                       : currentPageIndex_ - it->first.pageIndex);
            const std::uint64_t tick = it->second.lastAccessTick;

            bool betterVictim = false;
            if (victim == entries_.end()) {
                betterVictim = true;
            } else if (otherComic != victimOtherComic) {
                betterVictim = otherComic; // Always evict pages from inactive comics first
            } else if (dist != victimDist) {
                betterVictim = dist > victimDist; // Evict page furthest from current index first
            } else if (tick < victimTick) {
                betterVictim = true; // Tie-break by LRU (oldest access first)
            }

            if (betterVictim) {
                victim = it;
                victimOtherComic = otherComic;
                victimDist = dist;
                victimTick = tick;
            }
        }

        if (victim == entries_.end()) {
            break;
        }
        usedBytes_ -= victim->second.bytes;
        entries_.erase(victim);
    }
}

// =============================================================================
// CoverRamLru
// =============================================================================

CoverRamLru::CoverRamLru(std::size_t maxBytes) : maxBytes_(maxBytes) {}

void CoverRamLru::put(std::int64_t comicId, std::shared_ptr<const ImageFrame> frame) {
    if (!frame) {
        return;
    }
    const std::size_t bytes = frame->memoryBytes();
    std::scoped_lock lock(mutex_);

    if (auto it = index_.find(comicId); it != index_.end()) {
        usedBytes_ -= it->second->bytes;
        lruList_.erase(it->second);
        index_.erase(it);
    }

    if (maxBytes_ == 0 || bytes > maxBytes_) {
        return;
    }

    lruList_.push_front(Node{comicId, std::move(frame), bytes});
    index_[comicId] = lruList_.begin();
    usedBytes_ += bytes;

    while (usedBytes_ > maxBytes_ && !lruList_.empty()) {
        const auto& tail = lruList_.back();
        usedBytes_ -= tail.bytes;
        index_.erase(tail.comicId);
        lruList_.pop_back();
    }
}

std::shared_ptr<const ImageFrame> CoverRamLru::get(std::int64_t comicId) {
    std::scoped_lock lock(mutex_);
    auto it = index_.find(comicId);
    if (it == index_.end()) {
        return nullptr;
    }
    lruList_.splice(lruList_.begin(), lruList_, it->second);
    return it->second->frame;
}

void CoverRamLru::erase(std::int64_t comicId) {
    std::scoped_lock lock(mutex_);
    if (auto it = index_.find(comicId); it != index_.end()) {
        usedBytes_ -= it->second->bytes;
        lruList_.erase(it->second);
        index_.erase(it);
    }
}

void CoverRamLru::clear() {
    std::scoped_lock lock(mutex_);
    lruList_.clear();
    index_.clear();
    usedBytes_ = 0;
}

std::size_t CoverRamLru::memoryUsageBytes() const noexcept {
    std::scoped_lock lock(mutex_);
    return usedBytes_;
}

std::size_t CoverRamLru::entryCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return index_.size();
}

// =============================================================================
// Mode 1: RamOnlyCoverProvider (0 MB Disk, 64 MB RAM LRU)
// =============================================================================

RamOnlyCoverProvider::RamOnlyCoverProvider(std::size_t maxRamBytes) : ramLru_(maxRamBytes) {}

Result<std::shared_ptr<const ImageFrame>> RamOnlyCoverProvider::getCover(
    const ComicRecord& comic,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<std::shared_ptr<const ImageFrame>>::err(
            ErrorCode::Cancelled, "Cover request cancelled");
    }

    if (auto cached = ramLru_.get(comic.id); cached != nullptr) {
        return Result<std::shared_ptr<const ImageFrame>>::ok(std::move(cached));
    }

    auto frameRes = extractCoverThumbnail(comic, stopToken);
    if (!frameRes.isOk()) {
        return Result<std::shared_ptr<const ImageFrame>>::err(frameRes.error());
    }

    auto sharedFrame = std::make_shared<const ImageFrame>(std::move(frameRes).value());
    ramLru_.put(comic.id, sharedFrame);
    return Result<std::shared_ptr<const ImageFrame>>::ok(std::move(sharedFrame));
}

void RamOnlyCoverProvider::invalidate(std::int64_t comicId) {
    ramLru_.erase(comicId);
}

void RamOnlyCoverProvider::clearMemoryCache() {
    ramLru_.clear();
}

std::size_t RamOnlyCoverProvider::memoryUsageBytes() const noexcept {
    return ramLru_.memoryUsageBytes();
}

// =============================================================================
// Mode 2: PackFileCoverProvider (Single-File covers.pack)
// =============================================================================

PackFileCoverProvider::PackFileCoverProvider(
    std::filesystem::path packFilePath,
    DatabaseManager* db,
    std::size_t maxRamBytes)
    : packFilePath_(std::move(packFilePath)),
      db_(db),
      ramLru_(maxRamBytes) {}

Result<std::vector<std::uint8_t>> PackFileCoverProvider::readRawWebpFromPack(
    std::int64_t offset,
    std::uint32_t size,
    std::stop_token stopToken) const {
    if (stopToken.stop_requested()) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::Cancelled, "Pack cover read cancelled");
    }
    if (offset < 0 || size == 0) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::InvalidArgument, "Invalid covers.pack offset or size");
    }

    std::scoped_lock lock(packIoMutex_);
    const std::wstring wideLongPath = FileManager::toWin32LongPath(packFilePath_);
    HANDLE hFile = ::CreateFileW(
        wideLongPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::NotFound,
            "Failed to open covers.pack: " + FileManager::wideToUtf8(packFilePath_.native()));
    }

    LARGE_INTEGER seekPos{};
    seekPos.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN)) {
        ::CloseHandle(hFile);
        return Result<std::vector<std::uint8_t>>::err(
            ErrorCode::IoError, "Failed to seek inside covers.pack");
    }

    std::vector<std::uint8_t> buffer(size);
    std::uint32_t totalRead = 0;
    while (totalRead < size) {
        if (stopToken.stop_requested()) {
            ::CloseHandle(hFile);
            return Result<std::vector<std::uint8_t>>::err(
                ErrorCode::Cancelled, "Pack cover read cancelled");
        }
        DWORD chunkRead = 0;
        if (!::ReadFile(hFile, buffer.data() + totalRead, size - totalRead, &chunkRead, nullptr) ||
            chunkRead == 0) {
            ::CloseHandle(hFile);
            return Result<std::vector<std::uint8_t>>::err(
                ErrorCode::IoError, "Failed to read WebP payload from covers.pack");
        }
        totalRead += static_cast<std::uint32_t>(chunkRead);
    }

    ::CloseHandle(hFile);
    return Result<std::vector<std::uint8_t>>::ok(std::move(buffer));
}

Result<std::shared_ptr<const ImageFrame>> PackFileCoverProvider::getCover(
    const ComicRecord& comic,
    std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return Result<std::shared_ptr<const ImageFrame>>::err(
            ErrorCode::Cancelled, "Cover request cancelled");
    }

    if (auto cached = ramLru_.get(comic.id); cached != nullptr) {
        return Result<std::shared_ptr<const ImageFrame>>::ok(std::move(cached));
    }

    // Fast path: read pre-encoded WebP thumbnail directly from covers.pack by offset & size.
    if (comic.coverPackOffset >= 0 && comic.coverPackSize > 0) {
        auto webpRes = readRawWebpFromPack(comic.coverPackOffset, comic.coverPackSize, stopToken);
        if (webpRes.isOk()) {
            auto decodedRes = ImageDecoder::decode(webpRes.value(), 0, 0, stopToken);
            if (decodedRes.isOk()) {
                auto sharedFrame = std::make_shared<const ImageFrame>(std::move(decodedRes).value());
                ramLru_.put(comic.id, sharedFrame);
                return Result<std::shared_ptr<const ImageFrame>>::ok(std::move(sharedFrame));
            }
        }
    }

    // Fallback: extract on demand if not yet indexed in covers.pack.
    auto frameRes = extractCoverThumbnail(comic, stopToken);
    if (!frameRes.isOk()) {
        return Result<std::shared_ptr<const ImageFrame>>::err(frameRes.error());
    }

    auto sharedFrame = std::make_shared<const ImageFrame>(std::move(frameRes).value());
    ramLru_.put(comic.id, sharedFrame);
    return Result<std::shared_ptr<const ImageFrame>>::ok(std::move(sharedFrame));
}

void PackFileCoverProvider::invalidate(std::int64_t comicId) {
    ramLru_.erase(comicId);
}

void PackFileCoverProvider::clearMemoryCache() {
    ramLru_.clear();
}

std::size_t PackFileCoverProvider::memoryUsageBytes() const noexcept {
    return ramLru_.memoryUsageBytes();
}

Result<std::size_t> PackFileCoverProvider::indexUncachedCovers(
    std::stop_token stopToken,
    std::function<void(std::size_t, std::size_t)> progressCb) {
    if (db_ == nullptr || !db_->isOpen()) {
        return Result<std::size_t>::err(
            ErrorCode::DatabaseError, "DatabaseManager is not open for cover pack indexing");
    }

    auto uncachedRes = db_->getUncachedComicsOrderedByPath();
    if (!uncachedRes.isOk()) {
        return Result<std::size_t>::err(uncachedRes.error());
    }

    const auto& comics = uncachedRes.value();
    if (comics.empty()) {
        return Result<std::size_t>::ok(0);
    }

    std::error_code ec;
    if (packFilePath_.has_parent_path()) {
        std::filesystem::create_directories(packFilePath_.parent_path(), ec);
    }

    const std::filesystem::path longPackPath(FileManager::toWin32LongPath(packFilePath_));
    std::ofstream ofs(longPackPath, std::ios::binary | std::ios::app | std::ios::out);
    if (!ofs.is_open()) {
        return Result<std::size_t>::err(
            ErrorCode::IoError,
            "Failed to open covers.pack for append: " +
                FileManager::wideToUtf8(packFilePath_.native()));
    }

    ofs.seekp(0, std::ios::end);
    std::int64_t currentOffset = static_cast<std::int64_t>(ofs.tellp());
    if (currentOffset < 0) {
        currentOffset = 0;
    }

    std::vector<CoverPackLocationUpdate> pendingBatch;
    pendingBatch.reserve(kBatchFlushInterval);

    std::size_t indexedCount = 0;
    const std::size_t totalCount = comics.size();

    auto flushBatch = [&]() -> Result<void> {
        if (pendingBatch.empty()) {
            return Result<void>::ok();
        }
        {
            std::scoped_lock lock(packIoMutex_);
            ofs.flush();
            if (!ofs.good()) {
                return Result<void>::err(ErrorCode::IoError, "Failed to flush covers.pack stream");
            }
        }
        auto dbRes = db_->updateCoverPackLocationsBatched(pendingBatch);
        if (!dbRes.isOk()) {
            return dbRes;
        }
        pendingBatch.clear();
        return Result<void>::ok();
    };

    for (const auto& comic : comics) {
        if (stopToken.stop_requested()) {
            break;
        }

        auto thumbRes = extractCoverThumbnail(comic, stopToken);
        if (!thumbRes.isOk()) {
            if (thumbRes.errorCode() == ErrorCode::Cancelled) {
                break;
            }
            continue; // Skip unreadable/offline comic and continue batch
        }

        auto webpRes = ImageDecoder::encodeWebP(thumbRes.value(), kCoverWebpQuality);
        if (!webpRes.isOk()) {
            continue;
        }

        const auto& webpBytes = webpRes.value();
        const std::int64_t writeOffset = currentOffset;
        const auto writeSize = static_cast<std::uint32_t>(webpBytes.size());

        {
            std::scoped_lock lock(packIoMutex_);
            ofs.write(
                reinterpret_cast<const char*>(webpBytes.data()),
                static_cast<std::streamsize>(webpBytes.size()));
            if (!ofs.good()) {
                return Result<std::size_t>::err(
                    ErrorCode::IoError, "Failed writing WebP thumbnail to covers.pack");
            }
            currentOffset += static_cast<std::int64_t>(webpBytes.size());
        }

        pendingBatch.push_back(CoverPackLocationUpdate{comic.id, writeOffset, writeSize});
        ++indexedCount;

        if (pendingBatch.size() >= kBatchFlushInterval) {
            auto flushRes = flushBatch();
            if (!flushRes.isOk()) {
                return Result<std::size_t>::err(flushRes.error());
            }
        }

        if (progressCb) {
            progressCb(indexedCount, totalCount);
        }
    }

    // Flush any remaining updates (< 50).
    auto finalFlush = flushBatch();
    if (!finalFlush.isOk()) {
        return Result<std::size_t>::err(finalFlush.error());
    }

    return Result<std::size_t>::ok(indexedCount);
}

// =============================================================================
// CacheManager Facade
// =============================================================================

CacheManager::CacheManager(
    std::size_t pageRamLimitBytes,
    std::unique_ptr<ICoverStorage> coverProvider)
    : pageCache_(pageRamLimitBytes),
      coverProvider_(std::move(coverProvider)) {}

void CacheManager::setCoverStorage(std::unique_ptr<ICoverStorage> provider) {
    coverProvider_ = std::move(provider);
}

} // namespace mavira::core::cache
