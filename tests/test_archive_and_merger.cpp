#include "core/archive/ArchiveEngine.hpp"
#include "core/archive/ComicMerger.hpp"
#include "core/imaging/ImageDecoder.hpp"
#include "data/fs/FileManager.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>
#include <zip.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using mavira::core::ComicFormat;
using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::archive::ArchiveEngine;
using mavira::core::archive::ComicMerger;
using mavira::core::archive::MergeOptions;
using mavira::core::archive::MergeProgress;
using mavira::core::imaging::ImageDecoder;
using mavira::data::fs::FileManager;

struct TempDirGuard {
    std::filesystem::path path;

    explicit TempDirGuard(const std::wstring& name) {
        path = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::vector<std::uint8_t> makeJpegBytes(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    ImageFrame frame;
    frame.width = 48;
    frame.height = 64;
    frame.stride = frame.width * 4u;
    frame.pixels.resize(static_cast<std::size_t>(frame.stride) * frame.height);
    for (std::size_t i = 0; i < static_cast<std::size_t>(frame.width) * frame.height; ++i) {
        frame.pixels[i * 4 + 0] = r;
        frame.pixels[i * 4 + 1] = g;
        frame.pixels[i * 4 + 2] = b;
        frame.pixels[i * 4 + 3] = 255;
    }
    auto res = ImageDecoder::encodeJpeg(frame, 85);
    EXPECT_TRUE(res.isOk());
    return res.isOk() ? res.value() : std::vector<std::uint8_t>{};
}

void writeCbz(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries) {
    std::filesystem::create_directories(path.parent_path());
    const std::wstring wide = FileManager::toWin32LongPath(path);
    zip_error_t err;
    ::zip_error_init(&err);
    zip_source_t* src = ::zip_source_win32w_create(wide.c_str(), 0, -1, &err);
    ASSERT_NE(src, nullptr) << ::zip_error_strerror(&err);
    zip_t* za = ::zip_open_from_source(src, ZIP_CREATE | ZIP_TRUNCATE, &err);
    ASSERT_NE(za, nullptr) << ::zip_error_strerror(&err);
    ::zip_error_fini(&err);

    for (const auto& [name, data] : entries) {
        zip_source_t* buf = ::zip_source_buffer(za, data.data(), data.size(), 0);
        ASSERT_NE(buf, nullptr);
        ASSERT_GE(::zip_file_add(za, name.c_str(), buf, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE), 0);
    }
    ASSERT_EQ(::zip_close(za), 0);
}

void writeTarCbt(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries) {
    std::filesystem::create_directories(path.parent_path());
    const std::wstring wide = FileManager::toWin32LongPath(path);
    archive* a = ::archive_write_new();
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(::archive_write_set_format_ustar(a), ARCHIVE_OK);
    ASSERT_EQ(::archive_write_open_filename_w(a, wide.c_str()), ARCHIVE_OK);

    for (const auto& [name, data] : entries) {
        archive_entry* entry = ::archive_entry_new();
        ASSERT_NE(entry, nullptr);
        ::archive_entry_set_pathname(entry, name.c_str());
        ::archive_entry_set_size(entry, static_cast<la_int64_t>(data.size()));
        ::archive_entry_set_filetype(entry, AE_IFREG);
        ::archive_entry_set_perm(entry, 0644);
        ASSERT_EQ(::archive_write_header(a, entry), ARCHIVE_OK);
        if (!data.empty()) {
            ASSERT_EQ(::archive_write_data(a, data.data(), data.size()), static_cast<la_ssize_t>(data.size()));
        }
        ::archive_entry_free(entry);
    }
    ASSERT_EQ(::archive_write_close(a), ARCHIVE_OK);
    ASSERT_EQ(::archive_write_free(a), ARCHIVE_OK);
}

TEST(ArchiveAndMergerTest, ArchiveEngineOpensCbzCbtAndFolderInNaturalOrder) {
    TempDirGuard tmp(L"mavira_test_archive_merger_open");
    const auto p1 = makeJpegBytes(200, 40, 40);
    const auto p2 = makeJpegBytes(40, 200, 40);
    const auto p10 = makeJpegBytes(40, 40, 200);

    const auto cbzPath = tmp.path / L"vol1.cbz";
    writeCbz(cbzPath, {{"page10.jpg", p10}, {"page2.jpg", p2}, {"page1.jpg", p1}});

    const auto cbtPath = tmp.path / L"vol2.cbt";
    writeTarCbt(cbtPath, {{"010.jpg", p10}, {"002.jpg", p2}, {"001.jpg", p1}});

    EXPECT_EQ(ArchiveEngine::detectFormat(cbzPath), ComicFormat::CBZ);
    auto rCbz = ArchiveEngine::openArchive(cbzPath);
    ASSERT_TRUE(rCbz.isOk()) << rCbz.error().message;
    ASSERT_EQ(rCbz.value()->pageCount(), 3u);
    EXPECT_EQ(rCbz.value()->pageName(0), "page1.jpg");
    EXPECT_EQ(rCbz.value()->pageName(1), "page2.jpg");
    EXPECT_EQ(rCbz.value()->pageName(2), "page10.jpg");

    EXPECT_EQ(ArchiveEngine::detectFormat(cbtPath), ComicFormat::CBT);
    auto rCbt = ArchiveEngine::openArchive(cbtPath);
    ASSERT_TRUE(rCbt.isOk()) << rCbt.error().message;
    ASSERT_EQ(rCbt.value()->pageCount(), 3u);
    auto pageBytesRes = rCbt.value()->readPageBytes(1);
    ASSERT_TRUE(pageBytesRes.isOk());
    EXPECT_EQ(pageBytesRes.value(), p2);
}

TEST(ArchiveAndMergerTest, StreamingMergerCombinesCbzAndCbtIntoCbzAndPdfAndExtracts) {
    TempDirGuard tmp(L"mavira_test_archive_merger_merge");
    const auto p1 = makeJpegBytes(220, 30, 30);
    const auto p2 = makeJpegBytes(30, 220, 30);
    const auto p3 = makeJpegBytes(30, 30, 220);
    const auto p4 = makeJpegBytes(220, 220, 30);

    const auto vol1 = tmp.path / L"Vol_01.cbz";
    const auto vol2 = tmp.path / L"Vol_02.cbt";
    writeCbz(vol1, {{"001.jpg", p1}, {"002.jpg", p2}});
    writeTarCbt(vol2, {{"001.jpg", p3}, {"002.jpg", p4}});

    // 1. Merge into CBZ
    const auto mergedCbz = tmp.path / L"Merged.cbz";
    MergeOptions cbzOpts;
    cbzOpts.inputPaths = {vol1, vol2};
    cbzOpts.outputPath = mergedCbz;
    cbzOpts.outputFormat = ComicFormat::CBZ;

    std::size_t progressCalls = 0;
    auto cbzRes = ComicMerger::merge(cbzOpts, [&](const MergeProgress&) { ++progressCalls; });
    ASSERT_TRUE(cbzRes.isOk()) << cbzRes.error().message;
    EXPECT_EQ(cbzRes.value().totalPages, 4u);
    EXPECT_EQ(cbzRes.value().totalVolumes, 2u);
    ASSERT_EQ(cbzRes.value().chapterBookmarks.size(), 2u);
    EXPECT_EQ(cbzRes.value().chapterBookmarks[0].startPageIndex, 0u);
    EXPECT_EQ(cbzRes.value().chapterBookmarks[1].startPageIndex, 2u);
    EXPECT_GE(progressCalls, 4u);

    auto openedMerged = ArchiveEngine::openArchive(mergedCbz);
    ASSERT_TRUE(openedMerged.isOk());
    EXPECT_EQ(openedMerged.value()->pageCount(), 4u);

    // 2. Merge into PDF
    const auto mergedPdf = tmp.path / L"Merged.pdf";
    MergeOptions pdfOpts = cbzOpts;
    pdfOpts.outputPath = mergedPdf;
    pdfOpts.outputFormat = ComicFormat::PDF;
    auto pdfRes = ComicMerger::merge(pdfOpts);
    ASSERT_TRUE(pdfRes.isOk()) << pdfRes.error().message;
    EXPECT_EQ(pdfRes.value().totalPages, 4u);

    EXPECT_EQ(ArchiveEngine::detectFormat(mergedPdf), ComicFormat::PDF);
    auto openedPdf = ArchiveEngine::openArchive(mergedPdf);
    ASSERT_TRUE(openedPdf.isOk()) << openedPdf.error().message;
    EXPECT_EQ(openedPdf.value()->pageCount(), 4u);

    // 3. Extract pages to folder and repackage as CBZ
    const auto extractDir = tmp.path / L"extracted";
    auto extractRes = ComicMerger::extractPagesToFolder(mergedCbz, extractDir);
    ASSERT_TRUE(extractRes.isOk()) << extractRes.error().message;
    EXPECT_EQ(extractRes.value(), 4u);

    const auto repackedCbz = tmp.path / L"Repacked.cbz";
    auto packRes = ComicMerger::createCbzFromFolder(extractDir, repackedCbz);
    ASSERT_TRUE(packRes.isOk()) << packRes.error().message;
    EXPECT_EQ(packRes.value(), 4u);

    // 4. Cooperative cancellation
    std::stop_source stopSrc;
    stopSrc.request_stop();
    MergeOptions cancelOpts = cbzOpts;
    cancelOpts.outputPath = tmp.path / L"Cancelled.cbz";
    auto cancelRes = ComicMerger::merge(cancelOpts, {}, stopSrc.get_token());
    EXPECT_FALSE(cancelRes.isOk());
    EXPECT_EQ(cancelRes.errorCode(), ErrorCode::Cancelled);
}

} // namespace
