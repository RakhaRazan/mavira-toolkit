#include "app/ReaderController.hpp"
#include "core/archive/ArchiveEngine.hpp"
#include "core/archive/ComicMerger.hpp"
#include "core/archive/PdfDocumentReader.hpp"
#include "core/imaging/ImageDecoder.hpp"
#include "core/imaging/NativeOcrEngine.hpp"
#include "data/db/DatabaseManager.hpp"
#include "data/fs/FileManager.hpp"
#include "ui/MergeComicsDialog.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <fpdf_doc.h>
#include <fpdfview.h>
#include <gtest/gtest.h>
#include <zip.h>

#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using mavira::app::ReaderController;
using mavira::core::ComicFormat;
using mavira::core::ErrorCode;
using mavira::core::ImageFrame;
using mavira::core::Result;
using mavira::core::archive::ArchiveEngine;
using mavira::core::archive::ComicMerger;
using mavira::core::archive::MergeOptions;
using mavira::core::archive::MergeProgress;
using mavira::core::archive::PdfDocumentReader;
using mavira::core::imaging::ImageDecoder;
using mavira::data::db::DatabaseManager;
using mavira::data::fs::FileManager;
using mavira::ui::MergeComicsDialog;

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

struct PageFixture {
    std::string name;
    std::vector<std::uint8_t> bytes;
};

std::uint32_t crc32Ieee(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            const std::uint32_t mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void appendLe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void appendLe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

/// Generates a real JPEG image with a distinct solid+gradient pattern per (volIdx, pageIdx).
std::vector<std::uint8_t> makeTestJpeg(std::uint32_t width, std::uint32_t height, std::uint8_t rSeed, std::uint8_t gSeed) {
    ImageFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 4;
    frame.pixels.resize(static_cast<std::size_t>(frame.stride) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * width + x) * 4u;
            frame.pixels[off + 0] = static_cast<std::uint8_t>((rSeed + x) & 0xFFu);
            frame.pixels[off + 1] = static_cast<std::uint8_t>((gSeed + y) & 0xFFu);
            frame.pixels[off + 2] = static_cast<std::uint8_t>(((x ^ y) + rSeed) & 0xFFu);
            frame.pixels[off + 3] = 0xFFu;
        }
    }
    auto res = ImageDecoder::encodeJpeg(frame, 88);
    EXPECT_TRUE(res.isOk()) << res.error().message;
    return res ? res.value() : std::vector<std::uint8_t>{};
}

void createZipCbz(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries) {
    std::filesystem::create_directories(path.parent_path());
    int err = 0;
    const std::string utf8 = FileManager::wideToUtf8(path.native());
    zip_t* za = ::zip_open(utf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    ASSERT_NE(za, nullptr);
    for (const auto& [name, data] : entries) {
        zip_source_t* src = ::zip_source_buffer(za, data.data(), data.size(), 0);
        ASSERT_NE(src, nullptr);
        ASSERT_GE(::zip_file_add(za, name.c_str(), src, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE), 0);
    }
    ASSERT_EQ(::zip_close(za), 0);
}

/// Writes a spec-compliant RAR 4.x stored-mode (0x30) `.cbr` archive readable by libarchive.
void createRar4Cbr(const std::filesystem::path& path, const std::vector<PageFixture>& entries) {
    std::filesystem::create_directories(path.parent_path());
    std::vector<std::uint8_t> blob;

    // 1) RAR4 Marker Block: "Rar!\x1A\x07\x00"
    static constexpr std::uint8_t kMarker[7] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
    blob.insert(blob.end(), kMarker, kMarker + sizeof(kMarker));

    // 2) Main Archive Header (type 0x73, size 13)
    {
        std::vector<std::uint8_t> hdr;
        hdr.push_back(0x73);       // HEAD_TYPE
        appendLe16(hdr, 0x0000);   // HEAD_FLAGS
        appendLe16(hdr, 13);       // HEAD_SIZE
        appendLe16(hdr, 0);        // RESERVED1
        appendLe32(hdr, 0);        // RESERVED2
        const std::uint16_t crc = static_cast<std::uint16_t>(crc32Ieee(hdr.data(), hdr.size()) & 0xFFFFu);
        appendLe16(blob, crc);
        blob.insert(blob.end(), hdr.begin(), hdr.end());
    }

    // 3) File Headers (type 0x74) + Stored Data
    for (const auto& entry : entries) {
        const std::uint16_t nameLen = static_cast<std::uint16_t>(entry.name.size());
        const std::uint16_t headSize = static_cast<std::uint16_t>(32u + nameLen);
        const std::uint32_t dataSize = static_cast<std::uint32_t>(entry.bytes.size());
        const std::uint32_t fileCrc = crc32Ieee(entry.bytes.data(), entry.bytes.size());

        std::vector<std::uint8_t> hdr;
        hdr.push_back(0x74);       // HEAD_TYPE = FILE_HEAD
        appendLe16(hdr, 0x8000);   // HEAD_FLAGS (ADD_SIZE present)
        appendLe16(hdr, headSize); // HEAD_SIZE
        appendLe32(hdr, dataSize); // PACK_SIZE
        appendLe32(hdr, dataSize); // UNP_SIZE
        hdr.push_back(2);          // HOST_OS = Win32
        appendLe32(hdr, fileCrc);  // FILE_CRC
        appendLe32(hdr, 0);        // FTIME
        hdr.push_back(20);         // UNP_VER = 2.0
        hdr.push_back(0x30);       // METHOD = Store
        appendLe16(hdr, nameLen);  // NAME_SIZE
        appendLe32(hdr, 0x20);     // ATTR
        hdr.insert(hdr.end(), entry.name.begin(), entry.name.end());

        const std::uint16_t hdrCrc =
            static_cast<std::uint16_t>(crc32Ieee(hdr.data(), hdr.size()) & 0xFFFFu);
        appendLe16(blob, hdrCrc);
        blob.insert(blob.end(), hdr.begin(), hdr.end());
        blob.insert(blob.end(), entry.bytes.begin(), entry.bytes.end());
    }

    // 4) End of Archive Block (type 0x7B)
    static constexpr std::uint8_t kEndArc[7] = {0xC4, 0x3D, 0x7B, 0x00, 0x40, 0x07, 0x00};
    blob.insert(blob.end(), kEndArc, kEndArc + sizeof(kEndArc));

    std::ofstream ofs(FileManager::toWin32LongPath(path), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    ASSERT_TRUE(ofs.good());
}

std::size_t currentWorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (::GetProcessMemoryInfo(
            ::GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc))) {
        return static_cast<std::size_t>(pmc.WorkingSetSize);
    }
    return 0;
}

TEST(ComicMergerTest, MergeFiveCbrToCbzAndPdfWithRamCheckAndChapterBookmarks) {
    TempDirGuard tmp(L"mavira_test_comic_merger_5cbr");

    // Build 5 real .cbr (RAR4) volumes with 4 JPEG pages each (20 pages total).
    std::vector<std::filesystem::path> cbrPaths;
    std::vector<std::vector<std::uint8_t>> expectedAllPages;
    constexpr std::size_t kVolCount = 5;
    constexpr std::size_t kPagesPerVol = 4;

    for (std::size_t v = 0; v < kVolCount; ++v) {
        std::vector<PageFixture> pages;
        for (std::size_t p = 0; p < kPagesPerVol; ++p) {
            auto jpg = makeTestJpeg(
                320, 480,
                static_cast<std::uint8_t>(20 + v * 40),
                static_cast<std::uint8_t>(10 + p * 50));
            ASSERT_GT(jpg.size(), 100u);
            expectedAllPages.push_back(jpg);
            // Intentionally reuse identical filenames ("001.jpg".."004.jpg") across all 5 volumes
            // to verify V001_0001.jpg collision-avoidance prefixing.
            pages.push_back(PageFixture{"00" + std::to_string(p + 1) + ".jpg", std::move(jpg)});
        }
        const auto cbrPath = tmp.path / (L"Saga_Vol_" + std::to_wstring(v + 1) + L".cbr");
        createRar4Cbr(cbrPath, pages);
        ASSERT_EQ(ArchiveEngine::detectFormat(cbrPath), ComicFormat::CBR);
        cbrPaths.push_back(cbrPath);
    }

    // Open SQLite database to verify persisted chapter bookmarks.
    DatabaseManager db;
    ASSERT_TRUE(db.open(tmp.path / L"library.db").isOk());

    // =========================================================================
    // 1) Merge 5 .cbr -> 1 .cbz (Zero-Recompression ZIP_CM_STORE + Bookmarks)
    // =========================================================================
    const auto outCbz = tmp.path / L"Saga_Omnibus.cbz";
    const std::size_t baseRamCbz = currentWorkingSetBytes();
    std::size_t maxDeltaCbz = 0;
    std::size_t cbzProgressTicks = 0;

    MergeOptions cbzOpts;
    cbzOpts.inputPaths = cbrPaths;
    cbzOpts.outputPath = outCbz;
    cbzOpts.outputFormat = ComicFormat::CBZ;
    cbzOpts.saveChapterBookmarks = true;
    cbzOpts.database = &db;

    auto cbzRes = ComicMerger::merge(
        cbzOpts,
        [&](const MergeProgress&) {
            ++cbzProgressTicks;
            const std::size_t now = currentWorkingSetBytes();
            if (now > baseRamCbz) {
                maxDeltaCbz = (std::max)(maxDeltaCbz, now - baseRamCbz);
            }
        });
    ASSERT_TRUE(cbzRes.isOk()) << cbzRes.error().message;
    EXPECT_EQ(cbzRes.value().totalVolumes, kVolCount);
    EXPECT_EQ(cbzRes.value().totalPages, kVolCount * kPagesPerVol);
    EXPECT_EQ(cbzRes.value().chapterBookmarks.size(), kVolCount);
    EXPECT_EQ(cbzProgressTicks, kVolCount * kPagesPerVol);
    EXPECT_GT(cbzRes.value().savedComicId, 0);

    // RAM delta during 1-page streaming CBZ merge must stay well under 45 MB.
    constexpr std::size_t kMaxAllowedRamDelta = 45ULL * 1024ULL * 1024ULL;
    EXPECT_LT(maxDeltaCbz, kMaxAllowedRamDelta)
        << "CBZ merge RAM delta exceeded 45 MB: " << (maxDeltaCbz / (1024 * 1024)) << " MB";

    // Inspect raw libzip entries: verify ZIP_CM_STORE and V001_0001.jpg naming.
    {
        const std::wstring wideOut = FileManager::toWin32LongPath(outCbz);
        zip_error_t err;
        ::zip_error_init(&err);
        zip_source_t* src = ::zip_source_win32w_create(wideOut.c_str(), 0, -1, &err);
        ASSERT_NE(src, nullptr);
        zip_t* za = ::zip_open_from_source(src, ZIP_RDONLY, &err);
        ASSERT_NE(za, nullptr);
        ::zip_error_fini(&err);

        const zip_int64_t numEntries = ::zip_get_num_entries(za, 0);
        EXPECT_EQ(numEntries, static_cast<zip_int64_t>(kVolCount * kPagesPerVol + 1)); // + ComicInfo.xml

        for (zip_int64_t i = 0; i < numEntries; ++i) {
            zip_stat_t st;
            ::zip_stat_init(&st);
            ASSERT_EQ(::zip_stat_index(za, static_cast<zip_uint64_t>(i), 0, &st), 0);
            EXPECT_EQ(st.comp_method, ZIP_CM_STORE) << "Entry was recompressed: " << st.name;
        }
        ::zip_close(za);
    }

    // Verify 100% byte-identical images & entry naming through ArchiveEngine + ReaderController bookmarks!
    {
        auto openRes = ArchiveEngine::openArchive(outCbz);
        ASSERT_TRUE(openRes.isOk()) << openRes.error().message;
        auto& reader = *openRes.value();
        ASSERT_EQ(reader.pageCount(), kVolCount * kPagesPerVol);
        EXPECT_EQ(reader.pageName(0), "V001_0001.jpg");
        EXPECT_EQ(reader.pageName(4), "V002_0001.jpg");
        EXPECT_EQ(reader.pageName(19), "V005_0004.jpg");

        for (std::size_t i = 0; i < expectedAllPages.size(); ++i) {
            auto bytesRes = reader.readPageBytes(i);
            ASSERT_TRUE(bytesRes.isOk());
            EXPECT_EQ(bytesRes.value(), expectedAllPages[i]) << "Page " << i << " bytes altered!";
        }

        auto xmlRes = reader.readMetadataXml();
        ASSERT_TRUE(xmlRes.isOk());
        ASSERT_TRUE(xmlRes.value().has_value());
        EXPECT_NE(xmlRes.value()->find("Bookmark=\"Saga_Vol_1\""), std::string::npos);
        EXPECT_NE(xmlRes.value()->find("Bookmark=\"Saga_Vol_5\""), std::string::npos);
    }

    // Open merged CBZ in ReaderController and verify all 5 chapter bookmarks navigate properly.
    {
        ReaderController readerCtrl(&db, nullptr, nullptr);
        auto rcOpen = readerCtrl.openComic(outCbz, cbzRes.value().savedComicId, 0);
        ASSERT_TRUE(rcOpen.isOk()) << rcOpen.error().message;
        const auto& bms = readerCtrl.bookmarks();
        ASSERT_EQ(bms.size(), kVolCount);
        for (std::size_t v = 0; v < kVolCount; ++v) {
            EXPECT_EQ(bms[v].pageIndex, static_cast<std::uint32_t>(v * kPagesPerVol));
            EXPECT_EQ(bms[v].name, "Saga_Vol_" + std::to_string(v + 1));
            EXPECT_TRUE(readerCtrl.goToBookmark(bms[v].id));
            EXPECT_EQ(readerCtrl.currentPage(), v * kPagesPerVol);
        }
        readerCtrl.shutdown();
    }

    // =========================================================================
    // 2) Merge 5 .cbr -> 1 .pdf (Multi-Page PDF 1.4 + /Outlines Bookmarks)
    // =========================================================================
    const auto outPdf = tmp.path / L"Saga_Omnibus.pdf";
    const std::size_t baseRamPdf = currentWorkingSetBytes();
    std::size_t maxDeltaPdf = 0;

    MergeOptions pdfOpts;
    pdfOpts.inputPaths = cbrPaths;
    pdfOpts.outputPath = outPdf;
    pdfOpts.outputFormat = ComicFormat::PDF;
    pdfOpts.saveChapterBookmarks = true;
    pdfOpts.database = &db;

    auto pdfRes = ComicMerger::merge(
        pdfOpts,
        [&](const MergeProgress&) {
            const std::size_t now = currentWorkingSetBytes();
            if (now > baseRamPdf) {
                maxDeltaPdf = (std::max)(maxDeltaPdf, now - baseRamPdf);
            }
        });
    ASSERT_TRUE(pdfRes.isOk()) << pdfRes.error().message;
    EXPECT_EQ(pdfRes.value().totalVolumes, kVolCount);
    EXPECT_EQ(pdfRes.value().totalPages, kVolCount * kPagesPerVol);
    EXPECT_EQ(pdfRes.value().chapterBookmarks.size(), kVolCount);
    EXPECT_GT(pdfRes.value().savedComicId, 0);
    EXPECT_LT(maxDeltaPdf, kMaxAllowedRamDelta)
        << "PDF merge RAM delta exceeded 45 MB: " << (maxDeltaPdf / (1024 * 1024)) << " MB";

    // Open merged PDF via PdfDocumentReader (PDFium) and verify page count + rendering.
    {
        ASSERT_EQ(ArchiveEngine::detectFormat(outPdf), ComicFormat::PDF);
        PdfDocumentReader pdfReader;
        ASSERT_TRUE(pdfReader.open(outPdf).isOk());
        EXPECT_EQ(pdfReader.pageCount(), kVolCount * kPagesPerVol);
        auto frame0 = pdfReader.renderPage(0, 160, 0);
        ASSERT_TRUE(frame0.isOk());
        EXPECT_TRUE(frame0.value().isValid());
        auto frameLast = pdfReader.renderPage(19, 160, 0);
        ASSERT_TRUE(frameLast.isOk());
        EXPECT_TRUE(frameLast.value().isValid());
        pdfReader.close();
    }

    // Verify embedded PDF /Outlines chapter bookmarks open properly in PDFium!
    {
        const std::string pdfUtf8 = FileManager::wideToUtf8(outPdf.native());
        FPDF_DOCUMENT doc = ::FPDF_LoadDocument(pdfUtf8.c_str(), nullptr);
        ASSERT_NE(doc, nullptr);

        FPDF_BOOKMARK bm = ::FPDFBookmark_GetFirstChild(doc, nullptr);
        std::size_t outlineCount = 0;
        while (bm != nullptr) {
            FPDF_DEST dest = ::FPDFBookmark_GetDest(doc, bm);
            ASSERT_NE(dest, nullptr);
            const int destPage = ::FPDFDest_GetDestPageIndex(doc, dest);
            EXPECT_EQ(destPage, static_cast<int>(outlineCount * kPagesPerVol));
            ++outlineCount;
            bm = ::FPDFBookmark_GetNextSibling(doc, bm);
        }
        EXPECT_EQ(outlineCount, kVolCount);
        ::FPDF_CloseDocument(doc);
    }

    // Verify SQLite chapter bookmarks for the merged PDF open in ReaderController.
    {
        ReaderController readerCtrl(&db, nullptr, nullptr);
        ASSERT_TRUE(readerCtrl.openComic(outPdf, pdfRes.value().savedComicId, 0).isOk());
        const auto& bms = readerCtrl.bookmarks();
        ASSERT_EQ(bms.size(), kVolCount);
        for (std::size_t v = 0; v < kVolCount; ++v) {
            EXPECT_TRUE(readerCtrl.goToBookmark(bms[v].id));
            EXPECT_EQ(readerCtrl.currentPage(), v * kPagesPerVol);
        }
        readerCtrl.shutdown();
    }
}

TEST(ComicMergerTest, ExtractPagesToFolderAndCreateCbzFromFolderAndCancellation) {
    TempDirGuard tmp(L"mavira_test_extract_and_pack");

    const auto cbrPath = tmp.path / L"Sample_Vol.cbr";
    const auto jpg1 = makeTestJpeg(120, 160, 10, 20);
    const auto jpg2 = makeTestJpeg(120, 160, 80, 90);
    const auto jpg3 = makeTestJpeg(120, 160, 150, 160);
    createRar4Cbr(cbrPath, {
        {"001.jpg", jpg1},
        {"002.jpg", jpg2},
        {"003.jpg", jpg3},
    });

    // Extract pages to folder
    const auto extractDir = tmp.path / L"extracted";
    auto extRes = ComicMerger::extractPagesToFolder(cbrPath, extractDir);
    ASSERT_TRUE(extRes.isOk()) << extRes.error().message;
    EXPECT_EQ(extRes.value(), 3u);
    EXPECT_TRUE(std::filesystem::exists(extractDir / L"001.jpg"));
    EXPECT_TRUE(std::filesystem::exists(extractDir / L"003.jpg"));

    // Create CBZ from folder
    const auto repackedCbz = tmp.path / L"repacked.cbz";
    auto packRes = ComicMerger::createCbzFromFolder(extractDir, repackedCbz);
    ASSERT_TRUE(packRes.isOk()) << packRes.error().message;
    EXPECT_EQ(packRes.value(), 3u);

    auto openRes = ArchiveEngine::openArchive(repackedCbz);
    ASSERT_TRUE(openRes.isOk());
    EXPECT_EQ(openRes.value()->pageCount(), 3u);
    auto b0 = openRes.value()->readPageBytes(0);
    ASSERT_TRUE(b0.isOk());
    EXPECT_EQ(b0.value(), jpg1);

    // Cooperative stop_token cancellation
    std::stop_source stopSource;
    stopSource.request_stop();
    auto cancelExt = ComicMerger::extractPagesToFolder(cbrPath, tmp.path / L"cancel_ext", {}, stopSource.get_token());
    EXPECT_FALSE(cancelExt.isOk());
    EXPECT_EQ(cancelExt.errorCode(), ErrorCode::Cancelled);

    MergeOptions cancelOpts;
    cancelOpts.inputPaths = {cbrPath};
    cancelOpts.outputPath = tmp.path / L"cancel_out.cbz";
    cancelOpts.outputFormat = ComicFormat::CBZ;
    auto cancelMerge = ComicMerger::merge(cancelOpts, {}, stopSource.get_token());
    EXPECT_FALSE(cancelMerge.isOk());
    EXPECT_EQ(cancelMerge.errorCode(), ErrorCode::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(cancelOpts.outputPath));
}

TEST(ComicMergerTest, OpenSourceCb7MergeAndUnsupportedCbrCheck) {
    TempDirGuard tmp(L"mavira_test_cb7_merge");
    const auto cbrIn = tmp.path / L"in.cbr";
    createRar4Cbr(cbrIn, {
        {"001.jpg", makeTestJpeg(64, 64, 5, 5)},
        {"002.jpg", makeTestJpeg(64, 64, 25, 25)}
    });

    // Proprietary CBR output is no longer supported
    MergeOptions cbrOpts;
    cbrOpts.inputPaths = {cbrIn};
    cbrOpts.outputPath = tmp.path / L"out_unsupported.cbr";
    cbrOpts.outputFormat = ComicFormat::CBR;
    auto cbrRes = ComicMerger::merge(cbrOpts);
    EXPECT_FALSE(cbrRes.isOk());
    EXPECT_EQ(cbrRes.errorCode(), ErrorCode::UnsupportedFormat);

    // 100% open-source CB7 (7-Zip LZMA2 via libarchive)
    MergeOptions cb7Opts;
    cb7Opts.inputPaths = {cbrIn};
    cb7Opts.outputPath = tmp.path / L"out_merged.cb7";
    cb7Opts.outputFormat = ComicFormat::CB7;
    auto cb7Res = ComicMerger::merge(cb7Opts);
    ASSERT_TRUE(cb7Res.isOk()) << cb7Res.error().message;
    EXPECT_EQ(cb7Res.value().totalPages, 2u);
    EXPECT_TRUE(std::filesystem::exists(cb7Opts.outputPath));

    auto readerRes = ArchiveEngine::openArchive(cb7Opts.outputPath);
    ASSERT_TRUE(readerRes.isOk()) << readerRes.error().message;
    EXPECT_EQ(readerRes.value()->pageCount(), 2u);
}

TEST(ComicMergerTest, GrayscalePdfDeviceGrayAndCb7ToolJob) {
    TempDirGuard tmp(L"mavira_test_gray_pdf_cb7");

    // Create a 128x128 grayscale manga PNG page (R == G == B)
    ImageFrame grayFrame;
    grayFrame.width = 128;
    grayFrame.height = 128;
    grayFrame.stride = 128 * 4;
    grayFrame.pixels.resize(static_cast<std::size_t>(grayFrame.stride) * 128);
    for (std::uint32_t y = 0; y < 128; ++y) {
        for (std::uint32_t x = 0; x < 128; ++x) {
            const std::uint8_t v = static_cast<std::uint8_t>((x + y) & 0xFFu);
            const std::size_t off = (static_cast<std::size_t>(y) * 128 + x) * 4;
            grayFrame.pixels[off + 0] = v;
            grayFrame.pixels[off + 1] = v;
            grayFrame.pixels[off + 2] = v;
            grayFrame.pixels[off + 3] = 255;
        }
    }
    auto pngRes = ImageDecoder::encodePng(grayFrame, 6);
    ASSERT_TRUE(pngRes.isOk()) << pngRes.error().message;

    const auto cbzIn = tmp.path / L"manga_gray.cbz";
    const auto outDir = tmp.path / L"out";
    std::filesystem::create_directories(outDir);

    // Package into CBZ
    {
        int zipErr = 0;
        zip_t* za = ::zip_open(FileManager::wideToUtf8(cbzIn.native()).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zipErr);
        ASSERT_NE(za, nullptr);
        zip_source_t* src = ::zip_source_buffer(za, pngRes.value().data(), pngRes.value().size(), 0);
        ASSERT_NE(src, nullptr);
        ASSERT_GE(::zip_file_add(za, "001.png", src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8), 0);
        ASSERT_EQ(::zip_close(za), 0);
    }

    // 1. Convert grayscale CBZ -> lossless PDF and verify /DeviceGray + /Colors 1
    mavira::core::ToolExecutionOptions pdfOpts;
    pdfOpts.inputPaths = {cbzIn.native()};
    pdfOpts.outputFormat = "pdf";
    pdfOpts.outputFolderMode = "custom";
    pdfOpts.customOutputDir = outDir.native();
    pdfOpts.targetImageFormat = "original";
    pdfOpts.workerThreads = 4;

    auto pdfJobRes = ComicMerger::runToolJob("convert_comics", pdfOpts);
    ASSERT_TRUE(pdfJobRes.isOk()) << pdfJobRes.error().message;
    ASSERT_EQ(pdfJobRes.value().outputPaths.size(), 1u);
    EXPECT_FALSE(pdfJobRes.value().usedRawPassThrough);

    const auto pdfPath = pdfJobRes.value().outputPaths.front();
    std::ifstream ifs(pdfPath, std::ios::binary);
    ASSERT_TRUE(ifs.is_open());
    std::string pdfBytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_NE(pdfBytes.find("/ColorSpace /DeviceGray"), std::string::npos);
    EXPECT_NE(pdfBytes.find("/Colors 1"), std::string::npos);
    EXPECT_EQ(pdfBytes.find("/Colors 3"), std::string::npos);

    // 2. Convert CBZ -> CB7 via runToolJob (100% raw pass-through)
    mavira::core::ToolExecutionOptions cb7Opts = pdfOpts;
    cb7Opts.outputFormat = "cb7";
    auto cb7JobRes = ComicMerger::runToolJob("convert_comics", cb7Opts);
    ASSERT_TRUE(cb7JobRes.isOk()) << cb7JobRes.error().message;
    ASSERT_EQ(cb7JobRes.value().outputPaths.size(), 1u);
    EXPECT_TRUE(cb7JobRes.value().usedRawPassThrough);
    EXPECT_EQ(cb7JobRes.value().outputPaths.front().extension(), L".cb7");

    auto cb7Reader = ArchiveEngine::openArchive(cb7JobRes.value().outputPaths.front());
    ASSERT_TRUE(cb7Reader.isOk()) << cb7Reader.error().message;
    EXPECT_EQ(cb7Reader.value()->pageCount(), 1u);
}

TEST(ComicMergerTest, PdfDirectRawJpegExtractionMatchesSourceBytes) {
    TempDirGuard tmp(L"mavira_test_pdf_raw_extract");
    const std::vector<std::uint8_t> origJpg = makeTestJpeg(160, 220, 42, 77);
    const auto cbzIn = tmp.path / L"src.cbz";
    {
        int zipErr = 0;
        zip_t* za = ::zip_open(FileManager::wideToUtf8(cbzIn.native()).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zipErr);
        ASSERT_NE(za, nullptr);
        zip_source_t* src = ::zip_source_buffer(za, origJpg.data(), origJpg.size(), 0);
        ASSERT_NE(src, nullptr);
        ASSERT_GE(::zip_file_add(za, "001.jpg", src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8), 0);
        ASSERT_EQ(::zip_close(za), 0);
    }

    const auto pdfPath = tmp.path / L"manga.pdf";
    MergeOptions toPdf;
    toPdf.inputPaths = {cbzIn};
    toPdf.outputPath = pdfPath;
    toPdf.outputFormat = ComicFormat::PDF;
    ASSERT_TRUE(ComicMerger::merge(toPdf).isOk());

    // Direct PDFium raw JPEG extraction must return the exact original JPEG bytes
    PdfDocumentReader pdfReader;
    ASSERT_TRUE(pdfReader.open(pdfPath).isOk());
    auto rawRes = pdfReader.tryExtractRawPageImage(0);
    ASSERT_TRUE(rawRes.isOk()) << rawRes.error().message;
    EXPECT_EQ(rawRes.value(), origJpg);
    pdfReader.close();

    // Converting PDF -> CBZ with Keep the original must also preserve exact JPEG bytes
    const auto outDir = tmp.path / L"out";
    std::filesystem::create_directories(outDir);
    mavira::core::ToolExecutionOptions toCbz;
    toCbz.inputPaths = {pdfPath.native()};
    toCbz.outputFormat = "cbz";
    toCbz.outputFolderMode = "custom";
    toCbz.customOutputDir = outDir.native();
    toCbz.targetImageFormat = "original";
    auto jobRes = ComicMerger::runToolJob("convert_comics", toCbz);
    ASSERT_TRUE(jobRes.isOk()) << jobRes.error().message;
    ASSERT_EQ(jobRes.value().outputPaths.size(), 1u);

    auto cbzReader = ArchiveEngine::openArchive(jobRes.value().outputPaths.front());
    ASSERT_TRUE(cbzReader.isOk()) << cbzReader.error().message;
    auto pageBytes = cbzReader.value()->readPageBytes(0);
    ASSERT_TRUE(pageBytes.isOk());
    EXPECT_EQ(pageBytes.value(), origJpg);
}

TEST(ComicMergerTest, PdfNativeBitmapExtractionForNonJpegFlateMangaPdf) {
    TempDirGuard tmp(L"mavira_test_pdf_flate_bitmap");

    // Create a 128x160 grayscale PNG page -> lossless FlateDecode PDF
    ImageFrame gray;
    gray.width = 128;
    gray.height = 160;
    gray.stride = 128 * 4;
    gray.pixels.resize(static_cast<std::size_t>(gray.stride) * 160);
    for (std::uint32_t y = 0; y < 160; ++y) {
        for (std::uint32_t x = 0; x < 128; ++x) {
            const std::uint8_t v = static_cast<std::uint8_t>((x * 2 + y) & 0xFFu);
            const std::size_t off = (static_cast<std::size_t>(y) * 128 + x) * 4;
            gray.pixels[off + 0] = v;
            gray.pixels[off + 1] = v;
            gray.pixels[off + 2] = v;
            gray.pixels[off + 3] = 255;
        }
    }
    auto pngRes = ImageDecoder::encodePng(gray, 6);
    ASSERT_TRUE(pngRes.isOk());

    const auto cbzIn = tmp.path / L"flate_src.cbz";
    {
        int zipErr = 0;
        zip_t* za = ::zip_open(FileManager::wideToUtf8(cbzIn.native()).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zipErr);
        ASSERT_NE(za, nullptr);
        zip_source_t* src = ::zip_source_buffer(za, pngRes.value().data(), pngRes.value().size(), 0);
        ASSERT_NE(src, nullptr);
        ASSERT_GE(::zip_file_add(za, "001.png", src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8), 0);
        ASSERT_EQ(::zip_close(za), 0);
    }

    const auto outDir = tmp.path / L"out";
    std::filesystem::create_directories(outDir);
    mavira::core::ToolExecutionOptions toPdf;
    toPdf.inputPaths = {cbzIn.native()};
    toPdf.outputFormat = "pdf";
    toPdf.outputFolderMode = "custom";
    toPdf.customOutputDir = outDir.native();
    toPdf.targetImageFormat = "original";
    auto pdfJob = ComicMerger::runToolJob("convert_comics", toPdf);
    ASSERT_TRUE(pdfJob.isOk()) << pdfJob.error().message;
    const auto pdfPath = pdfJob.value().outputPaths.front();

    // Non-JPEG FlateDecode PDF: tryExtractRawPageImage fails (not DCTDecode),
    // but tryExtractNativePageBitmap succeeds at exact native 128x160 pixel dimensions!
    PdfDocumentReader pdfReader;
    ASSERT_TRUE(pdfReader.open(pdfPath).isOk());
    EXPECT_FALSE(pdfReader.tryExtractRawPageImage(0).isOk());
    auto bmpRes = pdfReader.tryExtractNativePageBitmap(0);
    ASSERT_TRUE(bmpRes.isOk()) << bmpRes.error().message;
    EXPECT_EQ(bmpRes.value().width, 128u);
    EXPECT_EQ(bmpRes.value().height, 160u);
    pdfReader.close();
}

TEST(ComicMergerTest, SanitizeWin32NameTrimsSpacesDotsIllegalCharsAndReservedDevices) {
    EXPECT_EQ(FileManager::sanitizeWin32Name("  Manga Vol 1  . . "), "Manga Vol 1");
    EXPECT_EQ(FileManager::sanitizeWin32Name("\tChapter: 10? *Test* | <2>\t "), "Chapter 10 Test  2");
    EXPECT_EQ(FileManager::sanitizeWin32Name("CON"), "_CON");
    EXPECT_EQ(FileManager::sanitizeWin32Name("nul "), "_nul");
    EXPECT_EQ(FileManager::sanitizeWin32Name("COM3"), "_COM3");
    EXPECT_EQ(FileManager::sanitizeWin32Name("LPT9"), "_LPT9");
    EXPECT_EQ(FileManager::sanitizeWin32Name("   ...  "), "untitled");
}

TEST(MergeComicsDialogTest, AddReorderAndLiveMergeUiWorkflow) {
    TempDirGuard tmp(L"mavira_test_merge_dialog");

    const auto vol1 = tmp.path / L"Vol_1.cbr";
    const auto vol2 = tmp.path / L"Vol_2.cbr";
    createRar4Cbr(vol1, {{"001.jpg", makeTestJpeg(100, 140, 10, 10)}});
    createRar4Cbr(vol2, {{"001.jpg", makeTestJpeg(100, 140, 90, 90)}});

    DatabaseManager db;
    ASSERT_TRUE(db.open(tmp.path / L"ui_test.db").isOk());

    MergeComicsDialog dlg(&db, {vol2, vol1});
    ASSERT_EQ(dlg.fileList()->count(), 2);

    // Reorder: move vol1 (row 1) up to row 0
    dlg.fileList()->setCurrentRow(1);
    dlg.moveSelectedUp();
    const auto ordered = dlg.orderedInputFiles();
    ASSERT_EQ(ordered.size(), 2u);
    EXPECT_EQ(ordered[0], vol1);
    EXPECT_EQ(ordered[1], vol2);

    // Choose PDF format and verify output extension updates automatically
    const auto outPdf = tmp.path / L"dialog_merged.pdf";
    dlg.setOutputPath(tmp.path / L"dialog_merged.cbz");
    dlg.setOutputFormat(ComicFormat::PDF);
    EXPECT_EQ(dlg.outputPath(), outPdf);

    // Run live merge and wait for mergeCompleted signal
    QEventLoop loop;
    bool completed = false;
    int reportedPages = 0;
    QObject::connect(&dlg, &MergeComicsDialog::mergeCompleted, &loop, [&](const QString&, int pages, qint64) {
        completed = true;
        reportedPages = pages;
        loop.quit();
    });
    QObject::connect(&dlg, &MergeComicsDialog::mergeFailed, &loop, [&](const QString& err) {
        ADD_FAILURE() << "Merge failed: " << err.toStdString();
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);

    dlg.startMerge();
    loop.exec();

    EXPECT_TRUE(completed);
    EXPECT_EQ(reportedPages, 2);
    EXPECT_TRUE(std::filesystem::exists(outPdf));
    ASSERT_TRUE(dlg.lastResult().has_value());
    EXPECT_EQ(dlg.lastResult()->chapterBookmarks.size(), 2u);
}

TEST(ComicMergerTest, ConvertComicBookBatchConvertImagesAndNativeOcrSmoke) {
    TempDirGuard tmp(L"mavira_convert_ocr_test");

    // 1. Create a 2-page CBZ and convert it to PDF via convertComicBook.
    const auto srcCbz = tmp.path / L"source.cbz";
    const auto outPdf = tmp.path / L"converted.pdf";
    const std::vector<std::uint8_t> jpg1 = makeTestJpeg(400, 600, 30, 60);
    const std::vector<std::uint8_t> jpg2 = makeTestJpeg(400, 600, 90, 120);
    {
        int err = 0;
        const std::string utf8 = FileManager::wideToUtf8(srcCbz.native());
        zip_t* za = ::zip_open(utf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
        ASSERT_NE(za, nullptr);
        zip_source_t* s1 = ::zip_source_buffer(za, jpg1.data(), jpg1.size(), 0);
        zip_source_t* s2 = ::zip_source_buffer(za, jpg2.data(), jpg2.size(), 0);
        ASSERT_GE(::zip_file_add(za, "001.jpg", s1, ZIP_FL_OVERWRITE), 0);
        ASSERT_GE(::zip_file_add(za, "002.jpg", s2, ZIP_FL_OVERWRITE), 0);
        ASSERT_EQ(::zip_close(za), 0);
    }

    auto convRes = ComicMerger::convertComicBook(srcCbz, outPdf, ComicFormat::PDF, 90);
    ASSERT_TRUE(convRes.isOk()) << convRes.error().message;
    EXPECT_EQ(convRes.value().totalPages, 2u);
    EXPECT_TRUE(std::filesystem::exists(outPdf));

    // 2. Batch convert standalone JPEGs -> PNG and WEBP with maxWidth=200.
    const auto img1Path = tmp.path / L"page1.jpg";
    const auto img2Path = tmp.path / L"page2.jpg";
    {
        std::ofstream f1(img1Path, std::ios::binary | std::ios::trunc);
        f1.write(reinterpret_cast<const char*>(jpg1.data()), static_cast<std::streamsize>(jpg1.size()));
        std::ofstream f2(img2Path, std::ios::binary | std::ios::trunc);
        f2.write(reinterpret_cast<const char*>(jpg2.data()), static_cast<std::streamsize>(jpg2.size()));
    }

    const auto pngOutDir = tmp.path / L"out_png";
    auto pngBatch = ComicMerger::batchConvertImages(
        {img1Path, img2Path},
        pngOutDir,
        mavira::core::imaging::ImageFormat::PNG,
        200,
        90);
    ASSERT_TRUE(pngBatch.isOk()) << pngBatch.error().message;
    EXPECT_EQ(pngBatch.value(), 2u);
    EXPECT_TRUE(std::filesystem::exists(pngOutDir / L"page1.png"));
    EXPECT_TRUE(std::filesystem::exists(pngOutDir / L"page2.png"));

    const auto webpOutDir = tmp.path / L"out_webp";
    auto webpBatch = ComicMerger::batchConvertImages(
        {img1Path},
        webpOutDir,
        mavira::core::imaging::ImageFormat::WEBP,
        200,
        80);
    ASSERT_TRUE(webpBatch.isOk()) << webpBatch.error().message;
    EXPECT_EQ(webpBatch.value(), 1u);
    EXPECT_TRUE(std::filesystem::exists(webpOutDir / L"page1.webp"));

    // 3. NativeOcrEngine smoke test (verify WinRT OCR initializes and runs cleanly).
    const auto langs = mavira::core::imaging::NativeOcrEngine::availableLanguages();
    EXPECT_GE(mavira::core::imaging::NativeOcrEngine::maxImageDimension(), 1024u);
    if (!langs.empty()) {
        auto ocrRes = mavira::core::imaging::NativeOcrEngine::recognizeImageFile(
            pngOutDir / L"page1.png", langs.front().languageTag);
        EXPECT_TRUE(ocrRes.isOk()) << ocrRes.error().message;
    }
}

TEST(ComicMergerTest, RunToolJobAllSixToolsWithSplitIfExistsAndCancellation) {
    TempDirGuard tmp(L"mavira_run_tool_job_test");

    // Create 4 test JPEGs in a source folder
    const auto imgDir = tmp.path / L"src_imgs";
    std::filesystem::create_directories(imgDir);
    for (int i = 1; i <= 4; ++i) {
        const auto bytes = makeTestJpeg(120, 160, static_cast<std::uint8_t>(i * 30), 60);
        const auto p = imgDir / (L"page_" + std::to_wstring(i) + L".jpg");
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // 1. create_comic -> splitIntoFiles = 2 CBZ files
    const auto createdDir = tmp.path / L"created";
    mavira::core::ToolExecutionOptions createOpts;
    createOpts.inputPaths = {imgDir.native()};
    createOpts.outputFormat = "cbz";
    createOpts.outputFolderMode = "custom";
    createOpts.customOutputDir = createdDir.native();
    createOpts.splitIntoFiles = 2;
    createOpts.compressionMode = "balanced";
    createOpts.ifExistsPolicy = "rename";

    auto createRes = ComicMerger::runToolJob("create_comic", createOpts);
    ASSERT_TRUE(createRes.isOk()) << createRes.error().message;
    ASSERT_EQ(createRes.value().outputPaths.size(), 2u);
    EXPECT_EQ(createRes.value().totalPagesProcessed, 4u);

    // Verify ifExistsPolicy = "skip" skips existing outputs
    createOpts.ifExistsPolicy = "skip";
    auto skipRes = ComicMerger::runToolJob("create_comic", createOpts);
    ASSERT_TRUE(skipRes.isOk());
    EXPECT_EQ(skipRes.value().skippedCount, 2u);
    EXPECT_TRUE(skipRes.value().outputPaths.empty());

    // 2. merge_comics -> merge the 2 created CBZs into 1 PDF
    const auto mergedDir = tmp.path / L"merged";
    mavira::core::ToolExecutionOptions mergeOpts;
    mergeOpts.inputPaths = {
        createRes.value().outputPaths[0].native(),
        createRes.value().outputPaths[1].native()
    };
    mergeOpts.outputFormat = "pdf";
    mergeOpts.outputFolderMode = "custom";
    mergeOpts.customOutputDir = mergedDir.native();
    mergeOpts.generateChapterBookmarks = true;
    mergeOpts.pdfCreationDpi = 150;

    auto mergeRes = ComicMerger::runToolJob("merge_comics", mergeOpts);
    ASSERT_TRUE(mergeRes.isOk()) << mergeRes.error().message;
    ASSERT_EQ(mergeRes.value().outputPaths.size(), 1u);
    EXPECT_EQ(mergeRes.value().outputPaths[0].filename().wstring(), L"src_imgs_part1.pdf");
    EXPECT_EQ(mergeRes.value().totalPagesProcessed, 4u);
    EXPECT_EQ(mergeRes.value().chapterBookmarks.size(), 2u);

    // 3. convert_comics -> convert merged PDF back to CBZ with image processing
    const auto convComicDir = tmp.path / L"conv_comics";
    mavira::core::ToolExecutionOptions convComicOpts;
    convComicOpts.inputPaths = {mergeRes.value().outputPaths[0].native()};
    convComicOpts.outputFormat = "cbz";
    convComicOpts.outputFolderMode = "custom";
    convComicOpts.customOutputDir = convComicDir.native();
    convComicOpts.scaleMode = "percentage";
    convComicOpts.scaleValue = 50;
    convComicOpts.targetImageFormat = "png";
    convComicOpts.grayscaleManga = true;

    auto convComicRes = ComicMerger::runToolJob("convert_comics", convComicOpts);
    ASSERT_TRUE(convComicRes.isOk()) << convComicRes.error().message;
    ASSERT_EQ(convComicRes.value().outputPaths.size(), 1u);
    EXPECT_EQ(convComicRes.value().outputPaths[0].filename().wstring(), L"src_imgs_part1.cbz");
    EXPECT_EQ(convComicRes.value().totalPagesProcessed, 4u);

    // 4. extract_pages -> extract converted CBZ pages to <Destination>/<ComicName>/001.png..004.png
    const auto extractedDir = tmp.path / L"extracted";
    mavira::core::ToolExecutionOptions extractOpts;
    extractOpts.inputPaths = {convComicRes.value().outputPaths[0].native()};
    extractOpts.outputFolderMode = "custom";
    extractOpts.customOutputDir = extractedDir.native();

    auto extractRes = ComicMerger::runToolJob("extract_pages", extractOpts);
    ASSERT_TRUE(extractRes.isOk()) << extractRes.error().message;
    EXPECT_EQ(extractRes.value().totalPagesProcessed, 4u);
    EXPECT_TRUE(std::filesystem::exists(extractedDir / L"src_imgs_part1" / L"001.png"));
    EXPECT_TRUE(std::filesystem::exists(extractedDir / L"src_imgs_part1" / L"004.png"));

    // 5. convert_images -> convert extracted images to WebP in parallel
    const auto convImgDir = tmp.path / L"conv_imgs";
    mavira::core::ToolExecutionOptions convImgOpts;
    convImgOpts.inputPaths = {extractedDir.native()};
    convImgOpts.outputFolderMode = "custom";
    convImgOpts.customOutputDir = convImgDir.native();
    convImgOpts.targetImageFormat = "webp";
    convImgOpts.webpQuality = 80;
    convImgOpts.workerThreads = 2;

    auto convImgRes = ComicMerger::runToolJob("convert_images", convImgOpts);
    ASSERT_TRUE(convImgRes.isOk()) << convImgRes.error().message;
    EXPECT_EQ(convImgRes.value().totalItemsProcessed, 4u);

    // 6. extract_ocr -> run OCR on converted CBZ if at least one OCR language pack is installed
    const auto langs = mavira::core::imaging::NativeOcrEngine::availableLanguages();
    if (!langs.empty()) {
        const auto ocrDir = tmp.path / L"ocr_out";
        mavira::core::ToolExecutionOptions ocrOpts;
        ocrOpts.inputPaths = {convComicRes.value().outputPaths[0].native()};
        ocrOpts.outputFolderMode = "custom";
        ocrOpts.customOutputDir = ocrDir.native();
        ocrOpts.workerThreads = 2;

        auto ocrRes = ComicMerger::runToolJob("extract_ocr", ocrOpts);
        ASSERT_TRUE(ocrRes.isOk()) << ocrRes.error().message;
        ASSERT_EQ(ocrRes.value().outputPaths.size(), 1u);
        EXPECT_EQ(ocrRes.value().outputPaths[0].filename().wstring(), L"src_imgs_part1.txt");
        EXPECT_TRUE(std::filesystem::exists(ocrRes.value().outputPaths[0]));
    }

    // 7. Cooperative std::stop_token cancellation
    std::stop_source stopSrc;
    stopSrc.request_stop();
    auto cancelRes = ComicMerger::runToolJob("convert_images", convImgOpts, {}, stopSrc.get_token());
    EXPECT_FALSE(cancelRes.isOk());
    EXPECT_EQ(cancelRes.errorCode(), ErrorCode::Cancelled);
}

TEST(ComicMergerTest, CleanFilenamesAndCollisionRenameAcrossTools) {
    TempDirGuard tmp(L"mavira_clean_names_test");
    const auto jpg = makeTestJpeg(80, 100, 42, 70);

    const auto srcDir = tmp.path / L"src";
    const auto comicPath = srcDir / L"One Piece Vol 01.cbz";
    createZipCbz(comicPath, {{"page1.jpg", jpg}, {"page2.jpg", jpg}});

    // convert_comics same_as_input cbz->cbz with ifExistsPolicy="rename" -> "One Piece Vol 01 (1).cbz"
    mavira::core::ToolExecutionOptions convSame;
    convSame.inputPaths = {comicPath.native()};
    convSame.outputFormat = "cbz";
    convSame.outputFolderMode = "same_as_input";
    convSame.ifExistsPolicy = "rename";
    auto convSameRes = ComicMerger::runToolJob("convert_comics", convSame);
    ASSERT_TRUE(convSameRes.isOk()) << convSameRes.error().message;
    ASSERT_EQ(convSameRes.value().outputPaths.size(), 1u);
    EXPECT_EQ(convSameRes.value().outputPaths[0].filename().wstring(), L"One Piece Vol 01 (1).cbz");

    // Second collision -> "One Piece Vol 01 (2).cbz"
    auto convSameRes2 = ComicMerger::runToolJob("convert_comics", convSame);
    ASSERT_TRUE(convSameRes2.isOk()) << convSameRes2.error().message;
    ASSERT_EQ(convSameRes2.value().outputPaths.size(), 1u);
    EXPECT_EQ(convSameRes2.value().outputPaths[0].filename().wstring(), L"One Piece Vol 01 (2).cbz");

    // convert_comics cbz->pdf -> exact stem "One Piece Vol 01.pdf" (no _converted)
    convSame.outputFormat = "pdf";
    auto convPdfRes = ComicMerger::runToolJob("convert_comics", convSame);
    ASSERT_TRUE(convPdfRes.isOk()) << convPdfRes.error().message;
    ASSERT_EQ(convPdfRes.value().outputPaths.size(), 1u);
    EXPECT_EQ(convPdfRes.value().outputPaths[0].filename().wstring(), L"One Piece Vol 01.pdf");

    // merge_comics -> exact stem of first input (no _merged)
    const auto mergeOut = tmp.path / L"merged";
    mavira::core::ToolExecutionOptions mergeOpts;
    mergeOpts.inputPaths = {comicPath.native(), (srcDir / L"One Piece Vol 01 (1).cbz").native()};
    mergeOpts.outputFormat = "cbz";
    mergeOpts.outputFolderMode = "custom";
    mergeOpts.customOutputDir = mergeOut.native();
    auto mergeRes = ComicMerger::runToolJob("merge_comics", mergeOpts);
    ASSERT_TRUE(mergeRes.isOk()) << mergeRes.error().message;
    ASSERT_EQ(mergeRes.value().outputPaths.size(), 1u);
    EXPECT_EQ(mergeRes.value().outputPaths[0].filename().wstring(), L"One Piece Vol 01.cbz");
}

TEST(ComicMergerTest, ExtractPagesStructuredSubfoldersDigitsRangeAndNaturalOrder) {
    TempDirGuard tmp(L"mavira_extract_pages_structured_test");
    const auto j1 = makeTestJpeg(40, 60, 10, 80);
    const auto j2 = makeTestJpeg(40, 60, 110, 80);
    const auto j10 = makeTestJpeg(40, 60, 210, 80);

    const auto inDir = tmp.path / L"comics";
    const auto comicA = inDir / L"Manga Vol 01.cbz";
    const auto comicB = inDir / L"Manga Vol 02.cbz";
    // Insert out of natural order to ensure natural sorting is respected
    createZipCbz(comicA, {{"page10.jpg", j10}, {"page2.jpg", j2}, {"page1.jpg", j1}});
    createZipCbz(comicB, {{"01.jpg", j1}, {"02.jpg", j2}});

    // 1. Folder of multiple comics -> <Dest>/<ComicStem>/001.jpg..NNN.jpg
    const auto dest1 = tmp.path / L"dest1";
    mavira::core::ToolExecutionOptions opts1;
    opts1.inputPaths = {inDir.native()};
    opts1.outputFolderMode = "custom";
    opts1.customOutputDir = dest1.native();
    auto res1 = ComicMerger::runToolJob("extract_pages", opts1);
    ASSERT_TRUE(res1.isOk()) << res1.error().message;
    EXPECT_EQ(res1.value().totalPagesProcessed, 5u);
    EXPECT_TRUE(std::filesystem::exists(dest1 / L"Manga Vol 01" / L"001.jpg"));
    EXPECT_TRUE(std::filesystem::exists(dest1 / L"Manga Vol 01" / L"002.jpg"));
    EXPECT_TRUE(std::filesystem::exists(dest1 / L"Manga Vol 01" / L"003.jpg"));
    EXPECT_TRUE(std::filesystem::exists(dest1 / L"Manga Vol 02" / L"001.jpg"));
    EXPECT_TRUE(std::filesystem::exists(dest1 / L"Manga Vol 02" / L"002.jpg"));
    // Ensure no loose images dumped directly in dest1
    for (const auto& entry : std::filesystem::directory_iterator(dest1)) {
        EXPECT_TRUE(entry.is_directory());
    }
    // Verify natural reading order: 001.jpg == j1, 003.jpg == j10
    {
        std::ifstream f1(dest1 / L"Manga Vol 01" / L"001.jpg", std::ios::binary);
        std::vector<std::uint8_t> b1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
        EXPECT_EQ(b1, j1);
        std::ifstream f3(dest1 / L"Manga Vol 01" / L"003.jpg", std::ios::binary);
        std::vector<std::uint8_t> b3((std::istreambuf_iterator<char>(f3)), std::istreambuf_iterator<char>());
        EXPECT_EQ(b3, j10);
    }

    // 2. pageRange = "1, 3" & pageNumberDigits = 4 -> 0001.jpg, 0002.jpg
    const auto dest2 = tmp.path / L"dest2";
    mavira::core::ToolExecutionOptions opts2;
    opts2.inputPaths = {comicA.native()};
    opts2.outputFolderMode = "custom";
    opts2.customOutputDir = dest2.native();
    opts2.pageRange = "1, 3";
    opts2.pageNumberDigits = 4;
    auto res2 = ComicMerger::runToolJob("extract_pages", opts2);
    ASSERT_TRUE(res2.isOk()) << res2.error().message;
    EXPECT_EQ(res2.value().totalPagesProcessed, 2u);
    EXPECT_TRUE(std::filesystem::exists(dest2 / L"Manga Vol 01" / L"0001.jpg"));
    EXPECT_TRUE(std::filesystem::exists(dest2 / L"Manga Vol 01" / L"0002.jpg"));

    // 3. pageNumberDigits = 0 -> keep original internal page names
    const auto dest3 = tmp.path / L"dest3";
    mavira::core::ToolExecutionOptions opts3;
    opts3.inputPaths = {comicA.native()};
    opts3.outputFolderMode = "custom";
    opts3.customOutputDir = dest3.native();
    opts3.pageNumberDigits = 0;
    auto res3 = ComicMerger::runToolJob("extract_pages", opts3);
    ASSERT_TRUE(res3.isOk()) << res3.error().message;
    EXPECT_TRUE(std::filesystem::exists(dest3 / L"Manga Vol 01" / L"page1.jpg"));
    EXPECT_TRUE(std::filesystem::exists(dest3 / L"Manga Vol 01" / L"page2.jpg"));
    EXPECT_TRUE(std::filesystem::exists(dest3 / L"Manga Vol 01" / L"page10.jpg"));

    // 4. >= 1000 pages auto 4-digit padding (0001.jpg .. 1002.jpg)
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> manyEntries;
    manyEntries.reserve(1002);
    for (int i = 1; i <= 1002; ++i) {
        manyEntries.emplace_back("p_" + std::to_string(i) + ".jpg", j1);
    }
    const auto bigComic = inDir / L"Omnibus.cbz";
    createZipCbz(bigComic, manyEntries);
    const auto dest4 = tmp.path / L"dest4";
    mavira::core::ToolExecutionOptions opts4;
    opts4.inputPaths = {bigComic.native()};
    opts4.outputFolderMode = "custom";
    opts4.customOutputDir = dest4.native();
    opts4.pageNumberDigits = 3;
    auto res4 = ComicMerger::runToolJob("extract_pages", opts4);
    ASSERT_TRUE(res4.isOk()) << res4.error().message;
    EXPECT_EQ(res4.value().totalPagesProcessed, 1002u);
    EXPECT_TRUE(std::filesystem::exists(dest4 / L"Omnibus" / L"0001.jpg"));
    EXPECT_TRUE(std::filesystem::exists(dest4 / L"Omnibus" / L"1002.jpg"));
}

TEST(ComicMergerTest, CreateComicFolderModesStripMetadataNormalizeWidthAndOcrFormats) {
    TempDirGuard tmp(L"mavira_specialized_options_test");
    const auto imgSmall = makeTestJpeg(60, 80, 50, 75);
    const auto imgWide = makeTestJpeg(120, 160, 150, 75);

    // 1. create_comic with per_subfolder vs single_combined
    const auto seriesRoot = tmp.path / L"SeriesRoot";
    std::filesystem::create_directories(seriesRoot / L"Chapter 01");
    std::filesystem::create_directories(seriesRoot / L"Chapter 02");
    {
        std::ofstream f1(seriesRoot / L"Chapter 01" / L"01.jpg", std::ios::binary);
        f1.write(reinterpret_cast<const char*>(imgSmall.data()), static_cast<std::streamsize>(imgSmall.size()));
        std::ofstream f2(seriesRoot / L"Chapter 02" / L"01.jpg", std::ios::binary);
        f2.write(reinterpret_cast<const char*>(imgWide.data()), static_cast<std::streamsize>(imgWide.size()));
    }

    const auto outPerSub = tmp.path / L"out_per_sub";
    mavira::core::ToolExecutionOptions subOpts;
    subOpts.inputPaths = {seriesRoot.native()};
    subOpts.outputFolderMode = "custom";
    subOpts.customOutputDir = outPerSub.native();
    subOpts.createFolderMode = "per_subfolder";
    auto subRes = ComicMerger::runToolJob("create_comic", subOpts);
    ASSERT_TRUE(subRes.isOk()) << subRes.error().message;
    ASSERT_EQ(subRes.value().outputPaths.size(), 2u);
    EXPECT_TRUE(std::filesystem::exists(outPerSub / L"Chapter 01.cbz"));
    EXPECT_TRUE(std::filesystem::exists(outPerSub / L"Chapter 02.cbz"));

    const auto outCombined = tmp.path / L"out_combined";
    mavira::core::ToolExecutionOptions combOpts = subOpts;
    combOpts.customOutputDir = outCombined.native();
    combOpts.createFolderMode = "single_combined";
    auto combRes = ComicMerger::runToolJob("create_comic", combOpts);
    ASSERT_TRUE(combRes.isOk()) << combRes.error().message;
    ASSERT_EQ(combRes.value().outputPaths.size(), 1u);
    EXPECT_TRUE(std::filesystem::exists(outCombined / L"SeriesRoot.cbz"));

    // 2. merge_comics with mergeNormalizeWidth = true
    const auto mergedNormDir = tmp.path / L"merged_norm";
    mavira::core::ToolExecutionOptions normOpts;
    normOpts.inputPaths = {
        (outPerSub / L"Chapter 01.cbz").native(),
        (outPerSub / L"Chapter 02.cbz").native()
    };
    normOpts.outputFormat = "cbz";
    normOpts.outputFolderMode = "custom";
    normOpts.customOutputDir = mergedNormDir.native();
    normOpts.mergeNormalizeWidth = true;
    auto normRes = ComicMerger::runToolJob("merge_comics", normOpts);
    ASSERT_TRUE(normRes.isOk()) << normRes.error().message;
    ASSERT_EQ(normRes.value().outputPaths.size(), 1u);

    auto openedNorm = ArchiveEngine::openArchive(normRes.value().outputPaths[0]);
    ASSERT_TRUE(openedNorm.isOk());
    ASSERT_EQ(openedNorm.value()->pageCount(), 2u);
    auto dec0 = ImageDecoder::decode(openedNorm.value()->readPageBytes(0).value());
    auto dec1 = ImageDecoder::decode(openedNorm.value()->readPageBytes(1).value());
    ASSERT_TRUE(dec0.isOk() && dec1.isOk());
    EXPECT_EQ(dec0.value().width, 120u);
    EXPECT_EQ(dec1.value().width, 120u);

    // 3. convert_images with stripImageMetadata = true strips injected JPEG COM marker
    std::vector<std::uint8_t> jpgWithComment = imgSmall;
    // Inject COM marker (FF FE 00 0C "SECRET_TAG") right after SOI (FF D8)
    const std::vector<std::uint8_t> comSegment = {
        0xFF, 0xFE, 0x00, 0x0C, 'S', 'E', 'C', 'R', 'E', 'T', '_', 'T', 'A', 'G'
    };
    jpgWithComment.insert(jpgWithComment.begin() + 2, comSegment.begin(), comSegment.end());
    const auto metaImgIn = tmp.path / L"meta_img.jpg";
    {
        std::ofstream f(metaImgIn, std::ios::binary);
        f.write(reinterpret_cast<const char*>(jpgWithComment.data()),
                static_cast<std::streamsize>(jpgWithComment.size()));
    }
    const auto metaOutDir = tmp.path / L"meta_out";
    mavira::core::ToolExecutionOptions stripOpts;
    stripOpts.inputPaths = {metaImgIn.native()};
    stripOpts.outputFolderMode = "custom";
    stripOpts.customOutputDir = metaOutDir.native();
    stripOpts.targetImageFormat = "jpg";
    stripOpts.stripImageMetadata = true;
    auto stripRes = ComicMerger::runToolJob("convert_images", stripOpts);
    ASSERT_TRUE(stripRes.isOk()) << stripRes.error().message;
    ASSERT_EQ(stripRes.value().outputPaths.size(), 1u);
    {
        std::ifstream f(stripRes.value().outputPaths[0], std::ios::binary);
        std::string outBytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        EXPECT_EQ(outBytes.find("SECRET_TAG"), std::string::npos);
    }

    // 4. extract_ocr with ocrOutputFormat = "md" and "json", pageRange, ocrIncludePageHeaders = false
    const auto langs = mavira::core::imaging::NativeOcrEngine::availableLanguages();
    if (!langs.empty()) {
        const auto ocrDir = tmp.path / L"ocr_formats";
        mavira::core::ToolExecutionOptions mdOpts;
        mdOpts.inputPaths = {(outPerSub / L"Chapter 01.cbz").native()};
        mdOpts.outputFolderMode = "custom";
        mdOpts.customOutputDir = ocrDir.native();
        mdOpts.ocrLanguage = "auto";
        mdOpts.ocrUpscaleFactor = 1.5;
        mdOpts.ocrOutputFormat = "md";
        mdOpts.ocrIncludePageHeaders = true;
        auto mdRes = ComicMerger::runToolJob("extract_ocr", mdOpts);
        ASSERT_TRUE(mdRes.isOk()) << mdRes.error().message;
        ASSERT_EQ(mdRes.value().outputPaths.size(), 1u);
        EXPECT_EQ(mdRes.value().outputPaths[0].filename().wstring(), L"Chapter 01.md");

        mavira::core::ToolExecutionOptions jsonOpts = mdOpts;
        jsonOpts.ocrOutputFormat = "json";
        jsonOpts.ocrIncludePageHeaders = false;
        auto jsonRes = ComicMerger::runToolJob("extract_ocr", jsonOpts);
        ASSERT_TRUE(jsonRes.isOk()) << jsonRes.error().message;
        ASSERT_EQ(jsonRes.value().outputPaths.size(), 1u);
        EXPECT_EQ(jsonRes.value().outputPaths[0].filename().wstring(), L"Chapter 01.json");
        std::ifstream jf(jsonRes.value().outputPaths[0], std::ios::binary);
        std::string jsonStr((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
        EXPECT_NE(jsonStr.find("\"pages\""), std::string::npos);
        EXPECT_EQ(jsonStr.find("\"header\""), std::string::npos);
    }
}

TEST(ComicMergerTest, LosslessRawPassThroughZeroByteExclusionAndMultiVolumeProgress) {
    TempDirGuard tmp(L"mavira_lossless_and_progress_test");
    const auto j1 = makeTestJpeg(48, 64, 15, 90);
    const auto j2 = makeTestJpeg(48, 64, 75, 130);
    const auto j3 = makeTestJpeg(48, 64, 135, 170);

    // 1. Verify requiresImageReencode helper
    mavira::core::ToolExecutionOptions defaultOpts;
    EXPECT_FALSE(ImageDecoder::requiresImageReencode(defaultOpts));
    auto modifiedOpts = defaultOpts;
    modifiedOpts.targetImageFormat = "png";
    EXPECT_TRUE(ImageDecoder::requiresImageReencode(modifiedOpts));
    modifiedOpts = defaultOpts;
    modifiedOpts.scaleValue = 80;
    EXPECT_TRUE(ImageDecoder::requiresImageReencode(modifiedOpts));

    // 2. Create two CBZs containing valid pages + 0-byte entry + __MACOSX/ + ._appledouble + dir/
    const auto vol1 = tmp.path / L"in" / L"Vol1.cbz";
    const auto vol2 = tmp.path / L"in" / L"Vol2.cbz";
    createZipCbz(vol1, {
        {"001.jpg", j1},
        {"002.jpg", j2},
        {"000_empty.jpg", {}},
        {"__MACOSX/._001.jpg", j1},
        {"._002.jpg", j2},
        {"subdir/", {}}
    });
    createZipCbz(vol2, {
        {"001.jpg", j3},
        {"empty2.png", {}}
    });

    // ArchiveEngine must exclude 0-byte, __MACOSX, ._*, and directory entries
    auto r1 = ArchiveEngine::openArchive(vol1);
    ASSERT_TRUE(r1.isOk());
    EXPECT_EQ(r1.value()->pageCount(), 2u);
    auto r2 = ArchiveEngine::openArchive(vol2);
    ASSERT_TRUE(r2.isOk());
    EXPECT_EQ(r2.value()->pageCount(), 1u);

    // 3. extract_pages across both volumes: verify 0 KB files excluded, exact byte identity, and progress <= total
    const auto dest = tmp.path / L"extracted";
    mavira::core::ToolExecutionOptions extractOpts;
    extractOpts.inputPaths = {(tmp.path / L"in").native()};
    extractOpts.outputFolderMode = "custom";
    extractOpts.customOutputDir = dest.native();

    std::vector<MergeProgress> progressSnapshots;
    auto extractRes = ComicMerger::runToolJob(
        "extract_pages",
        extractOpts,
        [&](const MergeProgress& prog) { progressSnapshots.push_back(prog); });
    ASSERT_TRUE(extractRes.isOk()) << extractRes.error().message;
    EXPECT_EQ(extractRes.value().totalPagesProcessed, 3u);
    ASSERT_EQ(progressSnapshots.size(), 3u);
    for (const auto& snap : progressSnapshots) {
        EXPECT_EQ(snap.totalPagesEstimated, 3u);
        EXPECT_LE(snap.totalPagesProcessed, snap.totalPagesEstimated);
    }

    // Verify 100% byte-identical pass-through (zero drift)
    {
        std::ifstream f(dest / L"Vol1" / L"001.jpg", std::ios::binary);
        std::vector<std::uint8_t> outBytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        EXPECT_EQ(outBytes, j1);
    }

    // 4. convert_images with "original" format + 100% scale: 100% byte-identical pass-through
    const auto imgOut = tmp.path / L"img_passthrough";
    mavira::core::ToolExecutionOptions imgOpts;
    imgOpts.inputPaths = {(dest / L"Vol1" / L"001.jpg").native()};
    imgOpts.outputFolderMode = "custom";
    imgOpts.customOutputDir = imgOut.native();
    imgOpts.targetImageFormat = "original";
    imgOpts.outputFormat = "cbz";
    auto imgRes = ComicMerger::runToolJob("convert_images", imgOpts);
    ASSERT_TRUE(imgRes.isOk()) << imgRes.error().message;
    ASSERT_EQ(imgRes.value().outputPaths.size(), 1u);
    {
        std::ifstream f(imgRes.value().outputPaths[0], std::ios::binary);
        std::vector<std::uint8_t> outBytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        EXPECT_EQ(outBytes, j1);
    }

    // 5. convert_comics PNG CBZ -> PDF with "original" format preserves lossless FlateDecode (no lossy JPEG)
    ImageFrame pngFrame;
    pngFrame.width = 48;
    pngFrame.height = 48;
    pngFrame.stride = 48 * 4;
    pngFrame.pixels.assign(48 * 48 * 4, 0xCC);
    auto pngRes = ImageDecoder::encodePng(pngFrame, 6);
    ASSERT_TRUE(pngRes.isOk());

    const auto pngCbz = tmp.path / L"PngSource.cbz";
    createZipCbz(pngCbz, {{"001.png", pngRes.value()}});
    mavira::core::ToolExecutionOptions pdfOpts;
    pdfOpts.inputPaths = {pngCbz.native()};
    pdfOpts.outputFolderMode = "custom";
    pdfOpts.customOutputDir = (tmp.path / L"pdf_out").native();
    pdfOpts.outputFormat = "pdf";
    pdfOpts.targetImageFormat = "original";
    auto pdfRes = ComicMerger::runToolJob("convert_comics", pdfOpts);
    ASSERT_TRUE(pdfRes.isOk()) << pdfRes.error().message;
    ASSERT_EQ(pdfRes.value().outputPaths.size(), 1u);
    {
        std::ifstream f(pdfRes.value().outputPaths[0], std::ios::binary);
        std::string pdfBytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        EXPECT_NE(pdfBytes.find("/Filter /FlateDecode"), std::string::npos);
        EXPECT_EQ(pdfBytes.find("/Filter /DCTDecode"), std::string::npos);
    }
    auto pdfReaderRes = ArchiveEngine::openArchive(pdfRes.value().outputPaths[0]);
    ASSERT_TRUE(pdfReaderRes.isOk()) << pdfReaderRes.error().message;
    EXPECT_EQ(pdfReaderRes.value()->pageCount(), 1u);
}

TEST(ComicMergerTest, OcrPostProcessingStripsNoiseAndNormalizesAllCaps) {
    using mavira::core::imaging::NativeOcrEngine;

    // 1. Strip isolated OCR noise tokens while preserving valid punctuation and CJK brackets.
    const std::string noisy =
        "| ~ ^ _ = ` / \\ -- .... Hello, world! Is this well-known? \"Wait...\" 「魔法」 (test) ----";
    const std::string filtered = NativeOcrEngine::cleanOcrText(noisy, /*filterSymbols=*/true, /*normalizeCaps=*/false);
    EXPECT_EQ(filtered, "Hello, world! Is this well-known? \"Wait...\" 「魔法」 (test)");

    // Preserve intra-word slashes like "and/or" while stripping isolated "/" or "\".
    EXPECT_EQ(
        NativeOcrEngine::cleanOcrText("and/or / \\ | ~ ^ _ = `", true, false),
        "and/or");

    // 2. Convert >75% uppercase Latin text to Sentence case, keeping standalone I/I'm/I'll/I've/I'd.
    const std::string allCaps =
        "| ~ WHAT ARE YOU DOING?! I'M NOT AFRAID... I'LL GO, I'VE SEEN IT, AND I'D DO IT AGAIN! IN THE END, I WIN. ----";
    const std::string normalized = NativeOcrEngine::cleanOcrText(allCaps, true, true);
    EXPECT_EQ(
        normalized,
        "What are you doing?! I'm not afraid... I'll go, I've seen it, and I'd do it again! In the end, I win.");

    // 3. Leave Japanese, Chinese, and Korean characters completely untouched while normalizing Latin.
    const std::string mixedCjk = "日本語テスト HELLO WORLD! 中文測試 한국어 테스트";
    EXPECT_EQ(
        NativeOcrEngine::cleanOcrText(mixedCjk, true, true),
        "日本語テスト Hello world! 中文測試 한국어 테스트");

    // Pure CJK remains identical.
    const std::string pureCjk = "「今日はいい天気ですね。」";
    EXPECT_EQ(NativeOcrEngine::cleanOcrText(pureCjk, true, true), pureCjk);

    // 4. Mixed-case Latin (<=75% uppercase) is not forced to Sentence case.
    const std::string mixedCase = "Hello World! This Is Title Case.";
    EXPECT_EQ(NativeOcrEngine::cleanOcrText(mixedCase, true, true), mixedCase);

    // 5. Respect opt-out flags.
    EXPECT_EQ(NativeOcrEngine::cleanOcrText("| HELLO |", false, false), "| HELLO |");
    EXPECT_EQ(NativeOcrEngine::cleanOcrText("| HELLO |", true, false), "HELLO");
    EXPECT_EQ(NativeOcrEngine::cleanOcrText("| HELLO |", false, true), "| Hello |");

    // 6. Verify pre-OCR halftone screentone despeckle/thresholding runs cleanly on synthetic noisy page.
    const auto langs = NativeOcrEngine::availableLanguages();
    if (!langs.empty()) {
        ImageFrame halftoneFrame;
        halftoneFrame.width = 64;
        halftoneFrame.height = 64;
        halftoneFrame.stride = 64 * 4;
        halftoneFrame.pixels.assign(64 * 64 * 4, 240);
        // Add isolated 1px halftone dots across the background.
        for (std::uint32_t y = 2; y < 62; y += 4) {
            for (std::uint32_t x = 2; x < 62; x += 4) {
                const std::size_t off = (static_cast<std::size_t>(y) * 64 + x) * 4;
                halftoneFrame.pixels[off + 0] = 60;
                halftoneFrame.pixels[off + 1] = 60;
                halftoneFrame.pixels[off + 2] = 60;
            }
        }
        auto ocrRes = NativeOcrEngine::recognizeFrame(
            halftoneFrame, "auto", 1.0, true, true);
        ASSERT_TRUE(ocrRes.isOk()) << ocrRes.error().message;
    }
}

TEST(ComicMergerTest, NewOpenSourceOutputFormatsCbtZipAndEpubWithRawPassThrough) {
    TempDirGuard tmp(L"mavira_test_cbt_zip_epub");
    const auto jpg1 = makeTestJpeg(160, 240, 20, 40);
    const auto jpg2 = makeTestJpeg(160, 240, 80, 120);
    const auto jpg3 = makeTestJpeg(160, 240, 140, 200);

    const auto srcCbz = tmp.path / L"src" / L"Vol01.cbz";
    createZipCbz(srcCbz, {{"001.jpg", jpg1}, {"002.jpg", jpg2}});

    const auto srcFolder = tmp.path / L"folder_src";
    std::filesystem::create_directories(srcFolder);
    {
        std::ofstream f1(srcFolder / L"001.jpg", std::ios::binary);
        f1.write(reinterpret_cast<const char*>(jpg1.data()), static_cast<std::streamsize>(jpg1.size()));
        std::ofstream f2(srcFolder / L"002.jpg", std::ios::binary);
        f2.write(reinterpret_cast<const char*>(jpg3.data()), static_cast<std::streamsize>(jpg3.size()));
    }

    // 1. .cbt (convert_comics + merge) -> uncompressed TAR with 100% raw pass-through
    {
        mavira::core::ToolExecutionOptions cbtOpts;
        cbtOpts.inputPaths = {srcCbz.native()};
        cbtOpts.outputFolderMode = "custom";
        cbtOpts.customOutputDir = (tmp.path / L"out_cbt").native();
        cbtOpts.outputFormat = "cbt";
        auto res = ComicMerger::runToolJob("convert_comics", cbtOpts);
        ASSERT_TRUE(res.isOk()) << res.error().message;
        EXPECT_TRUE(res.value().usedRawPassThrough);
        ASSERT_EQ(res.value().outputPaths.size(), 1u);
        const auto& cbtPath = res.value().outputPaths[0];
        EXPECT_EQ(cbtPath.extension().wstring(), L".cbt");
        EXPECT_EQ(ArchiveEngine::detectFormat(cbtPath), ComicFormat::CBT);

        auto r = ArchiveEngine::openArchive(cbtPath);
        ASSERT_TRUE(r.isOk()) << r.error().message;
        ASSERT_EQ(r.value()->pageCount(), 2u);
        EXPECT_EQ(r.value()->readPageBytes(0).value(), jpg1);
        EXPECT_EQ(r.value()->readPageBytes(1).value(), jpg2);
    }

    // 2. .zip (create_comic) -> standard ZIP with 100% raw pass-through
    {
        mavira::core::ToolExecutionOptions zipOpts;
        zipOpts.inputPaths = {srcFolder.native()};
        zipOpts.outputFolderMode = "custom";
        zipOpts.customOutputDir = (tmp.path / L"out_zip").native();
        zipOpts.outputFormat = "zip";
        zipOpts.createFolderMode = "single_combined";
        auto res = ComicMerger::runToolJob("create_comic", zipOpts);
        ASSERT_TRUE(res.isOk()) << res.error().message;
        EXPECT_TRUE(res.value().usedRawPassThrough);
        ASSERT_EQ(res.value().outputPaths.size(), 1u);
        const auto& zipPath = res.value().outputPaths[0];
        EXPECT_EQ(zipPath.extension().wstring(), L".zip");

        auto r = ArchiveEngine::openArchive(zipPath);
        ASSERT_TRUE(r.isOk()) << r.error().message;
        ASSERT_EQ(r.value()->pageCount(), 2u);
        EXPECT_EQ(r.value()->readPageBytes(0).value(), jpg1);
        EXPECT_EQ(r.value()->readPageBytes(1).value(), jpg3);
    }

    // 3. .epub (merge_comics) -> Fixed-Layout Comic EPUB3 package via libzip
    {
        mavira::core::ToolExecutionOptions epubOpts;
        epubOpts.inputPaths = {srcCbz.native(), srcFolder.native()};
        epubOpts.outputFolderMode = "custom";
        epubOpts.customOutputDir = (tmp.path / L"out_epub").native();
        epubOpts.outputFormat = "epub";
        epubOpts.generateChapterBookmarks = true;
        auto res = ComicMerger::runToolJob("merge_comics", epubOpts);
        ASSERT_TRUE(res.isOk()) << res.error().message;
        EXPECT_TRUE(res.value().usedRawPassThrough);
        ASSERT_EQ(res.value().outputPaths.size(), 1u);
        const auto& epubPath = res.value().outputPaths[0];
        EXPECT_EQ(epubPath.extension().wstring(), L".epub");
        EXPECT_EQ(ArchiveEngine::detectFormat(epubPath), ComicFormat::EPUB);

        // Verify uncompressed mimetype entry is written first at byte offset 30
        {
            std::ifstream ifs(epubPath, std::ios::binary);
            std::string head(64, '\0');
            ifs.read(head.data(), static_cast<std::streamsize>(head.size()));
            ASSERT_GE(ifs.gcount(), 58);
            EXPECT_EQ(head.substr(30, 28), "mimetypeapplication/epub+zip");
        }

        // Verify required EPUB3 package files via libzip
        int zerr = 0;
        const std::string epubUtf8 = FileManager::wideToUtf8(epubPath.native());
        zip_t* za = ::zip_open(epubUtf8.c_str(), ZIP_RDONLY, &zerr);
        ASSERT_NE(za, nullptr);
        EXPECT_GE(::zip_name_locate(za, "mimetype", 0), 0);
        EXPECT_GE(::zip_name_locate(za, "META-INF/container.xml", 0), 0);
        EXPECT_GE(::zip_name_locate(za, "OEBPS/content.opf", 0), 0);
        EXPECT_GE(::zip_name_locate(za, "OEBPS/toc.ncx", 0), 0);
        EXPECT_GE(::zip_name_locate(za, "OEBPS/page_001.xhtml", 0), 0);
        EXPECT_GE(::zip_name_locate(za, "OEBPS/page_004.xhtml", 0), 0);
        EXPECT_GE(::zip_name_locate(za, "OEBPS/images/page_001.jpg", 0), 0);
        EXPECT_GE(::zip_name_locate(za, "OEBPS/images/page_004.jpg", 0), 0);

        auto readZipEntryText = [&](const char* name) -> std::string {
            zip_file_t* zf = ::zip_fopen(za, name, 0);
            if (!zf) return {};
            std::string out;
            char buf[4096];
            zip_int64_t n = 0;
            while ((n = ::zip_fread(zf, buf, sizeof(buf))) > 0) {
                out.append(buf, static_cast<std::size_t>(n));
            }
            ::zip_fclose(zf);
            return out;
        };
        const std::string opf = readZipEntryText("OEBPS/content.opf");
        EXPECT_NE(opf.find("rendition:layout\">pre-paginated"), std::string::npos);
        EXPECT_NE(opf.find("images/page_001.jpg"), std::string::npos);
        const std::string ncx = readZipEntryText("OEBPS/toc.ncx");
        EXPECT_NE(ncx.find("page_001.xhtml"), std::string::npos);
        EXPECT_NE(ncx.find("page_003.xhtml"), std::string::npos);
        const std::string p1Xhtml = readZipEntryText("OEBPS/page_001.xhtml");
        EXPECT_NE(p1Xhtml.find("width=160, height=240"), std::string::npos);
        EXPECT_NE(p1Xhtml.find("images/page_001.jpg"), std::string::npos);
        ::zip_close(za);

        // Verify ArchiveEngine::openArchive opens the EPUB and yields 4 raw-identical pages
        auto r = ArchiveEngine::openArchive(epubPath);
        ASSERT_TRUE(r.isOk()) << r.error().message;
        ASSERT_EQ(r.value()->pageCount(), 4u);
        EXPECT_EQ(r.value()->readPageBytes(0).value(), jpg1);
        EXPECT_EQ(r.value()->readPageBytes(1).value(), jpg2);
        EXPECT_EQ(r.value()->readPageBytes(2).value(), jpg1);
        EXPECT_EQ(r.value()->readPageBytes(3).value(), jpg3);
    }

    // 4. Direct ComicMerger::convertComicBook to CBT and EPUB
    {
        const auto convCbt = tmp.path / L"direct.cbt";
        const auto convEpub = tmp.path / L"direct.epub";
        ASSERT_TRUE(ComicMerger::convertComicBook(srcCbz, convCbt, ComicFormat::CBT).isOk());
        ASSERT_TRUE(ComicMerger::convertComicBook(srcCbz, convEpub, ComicFormat::EPUB).isOk());
        EXPECT_EQ(ArchiveEngine::detectFormat(convCbt), ComicFormat::CBT);
        EXPECT_EQ(ArchiveEngine::detectFormat(convEpub), ComicFormat::EPUB);
    }
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
