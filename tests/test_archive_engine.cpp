#include "core/archive/ArchiveEngine.hpp"
#include "core/archive/LibArchiveReader.hpp"
#include "core/archive/PdfDocumentReader.hpp"
#include "core/archive/ZipArchiveReader.hpp"
#include "core/metadata/MetadataManager.hpp"
#include "data/fs/FileManager.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>
#include <zip.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using mavira::core::ComicFormat;
using mavira::core::ErrorCode;
using mavira::core::archive::ArchiveEngine;
using mavira::core::archive::LibArchiveReader;
using mavira::core::archive::PdfDocumentReader;
using mavira::core::archive::PdfMetadata;
using mavira::core::metadata::ComicMetadata;
using mavira::core::metadata::MetadataManager;
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

struct EntrySpec {
    std::string name;
    std::vector<std::uint8_t> data;
    bool storeUncompressed{false};
};

std::vector<std::uint8_t> toBytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::size_t countFilesInDirectory(const std::filesystem::path& dir) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

void createCbzWide(const std::filesystem::path& path, const std::vector<EntrySpec>& entries) {
    std::filesystem::create_directories(path.parent_path());
    const std::wstring wide = FileManager::toWin32LongPath(path);

    zip_error_t err;
    ::zip_error_init(&err);
    zip_source_t* src = ::zip_source_win32w_create(wide.c_str(), 0, -1, &err);
    ASSERT_NE(src, nullptr) << ::zip_error_strerror(&err);
    zip_t* za = ::zip_open_from_source(src, ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (za == nullptr) {
        ::zip_source_free(src);
        FAIL() << ::zip_error_strerror(&err);
    }
    ::zip_error_fini(&err);

    for (const auto& entry : entries) {
        zip_source_t* buf = ::zip_source_buffer(za, entry.data.data(), entry.data.size(), 0);
        ASSERT_NE(buf, nullptr);
        const zip_int64_t idx =
            ::zip_file_add(za, entry.name.c_str(), buf, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (idx < 0) {
            ::zip_source_free(buf);
            ::zip_discard(za);
            FAIL() << "Failed to add ZIP entry: " << entry.name;
        }
        if (entry.storeUncompressed) {
            ASSERT_EQ(::zip_set_file_compression(za, static_cast<zip_uint64_t>(idx), ZIP_CM_STORE, 0), 0);
        }
    }
    ASSERT_EQ(::zip_close(za), 0);
}

void createCbtTarWide(const std::filesystem::path& path, const std::vector<EntrySpec>& entries) {
    std::filesystem::create_directories(path.parent_path());
    const std::wstring wide = FileManager::toWin32LongPath(path);

    archive* a = ::archive_write_new();
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(::archive_write_set_format_ustar(a), ARCHIVE_OK);
    ASSERT_EQ(::archive_write_open_filename_w(a, wide.c_str()), ARCHIVE_OK);

    for (const auto& spec : entries) {
        archive_entry* entry = ::archive_entry_new();
        ASSERT_NE(entry, nullptr);
        ::archive_entry_set_pathname(entry, spec.name.c_str());
        ::archive_entry_set_size(entry, static_cast<la_int64_t>(spec.data.size()));
        ::archive_entry_set_filetype(entry, AE_IFREG);
        ::archive_entry_set_perm(entry, 0644);
        ASSERT_EQ(::archive_write_header(a, entry), ARCHIVE_OK);
        if (!spec.data.empty()) {
            ASSERT_EQ(
                ::archive_write_data(a, spec.data.data(), spec.data.size()),
                static_cast<la_ssize_t>(spec.data.size()));
        }
        ::archive_entry_free(entry);
    }

    ASSERT_EQ(::archive_write_close(a), ARCHIVE_OK);
    ASSERT_EQ(::archive_write_free(a), ARCHIVE_OK);
}

/// Builds a minimal valid 2-page PDF 1.4 file with an `/Info` metadata dictionary.
void createMinimalTwoPagePdf(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());

    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets;
    offsets.push_back(0); // Object 0

    auto addObj = [&](const std::string& body) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(offsets.size() - 1) + " 0 obj\n" + body + "\nendobj\n";
    };

    // 1: Catalog
    addObj("<< /Type /Catalog /Pages 2 0 R >>");
    // 2: Pages (2 pages)
    addObj("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>");
    // 3: Page 1 (200x300 pt)
    addObj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] >>");
    // 4: Page 2 (400x200 pt - wide spread)
    addObj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] >>");
    // 5: Info dictionary
    addObj(
        "<< /Title (Mavira Test Volume) "
        "/Author (Taro Yamada) "
        "/Subject (Cyberpunk Anthology) "
        "/Keywords (sci-fi, manga, action) >>");

    const std::size_t xrefPos = pdf.size();
    pdf += "xref\n0 " + std::to_string(offsets.size()) + "\n";
    pdf += "0000000000 65535 f \n";
    for (std::size_t i = 1; i < offsets.size(); ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", offsets[i]);
        pdf += buf;
    }

    pdf += "trailer\n<< /Size " + std::to_string(offsets.size()) +
           " /Root 1 0 R /Info 5 0 R >>\nstartxref\n" +
           std::to_string(xrefPos) + "\n%%EOF\n";

    std::ofstream ofs(path, std::ios::binary);
    ASSERT_TRUE(ofs.is_open());
    ofs.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
    ofs.close();
}

TEST(ArchiveEngineIntegrationTest, CbzEndToEndWithMetadataXml) {
    TempDirGuard tmp(L"mavira_test_engine_cbz");
    const std::filesystem::path cbzPath = tmp.path / L"[Kurokawa] Neon_Blade_Vol1 (C103).cbz";

    const std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<ComicInfo>
  <Title>Neon Blade &amp; Shadow</Title>
  <Series>Neon Blade</Series>
  <Writer>Kurokawa Ken</Writer>
  <Genre>Action, Cyberpunk</Genre>
  <Tags>mecha, neon, anthology</Tags>
  <Volume>1</Volume>
  <PageCount>2</PageCount>
</ComicInfo>)";

    const auto page1 = toBytes("fake-jpg-page-1");
    const auto page2 = toBytes("fake-png-page-2");

    createCbzWide(cbzPath, {
        {"002.png", page2},
        {"001.jpg", page1},
        {"ComicInfo.xml", toBytes(xml)},
    });

    ASSERT_EQ(ArchiveEngine::detectFormat(cbzPath), ComicFormat::CBZ);
    auto openRes = ArchiveEngine::openArchive(cbzPath);
    ASSERT_TRUE(openRes.isOk()) << openRes.error().message;
    auto& reader = *openRes.value();

    ASSERT_EQ(reader.pageCount(), 2u);
    EXPECT_EQ(reader.pageName(0), "001.jpg");
    EXPECT_EQ(reader.pageName(1), "002.png");

    auto bytes0 = reader.readPageBytes(0);
    ASSERT_TRUE(bytes0.isOk());
    EXPECT_EQ(bytes0.value(), page1);

    auto xmlRes = reader.readMetadataXml();
    ASSERT_TRUE(xmlRes.isOk());
    ASSERT_TRUE(xmlRes.value().has_value());

    const ComicMetadata meta = MetadataManager::extractMetadata(cbzPath, xmlRes.value());
    EXPECT_EQ(meta.title, "Neon Blade & Shadow");
    EXPECT_EQ(meta.series, "Neon Blade");
    EXPECT_EQ(meta.writer, "Kurokawa Ken");
    EXPECT_EQ(meta.event, "C103");
    ASSERT_EQ(meta.genres.size(), 2u);
    EXPECT_EQ(meta.genres[0], "Action");
    EXPECT_EQ(meta.genres[1], "Cyberpunk");
    ASSERT_EQ(meta.tags.size(), 3u);
    EXPECT_EQ(meta.tags[0], "mecha");
    EXPECT_EQ(meta.volumeNumber.value_or(0), 1);
    EXPECT_EQ(meta.pageCount.value_or(0), 2);
}

TEST(ArchiveEngineIntegrationTest, LibArchiveReaderStreamsTarCbtInMemory) {
    TempDirGuard tmp(L"mavira_test_engine_cbt_\u65e5\u672c\u8a9e");
    const std::filesystem::path cbtPath = tmp.path / L"sample_tar.cbt";

    const auto p1 = toBytes("tar-page-1-bytes");
    const auto p2 = toBytes("tar-page-2-bytes");
    const auto p10 = toBytes("tar-page-10-bytes");
    const std::string xml = "<ComicInfo><Series>TarSeries</Series></ComicInfo>";

    createCbtTarWide(cbtPath, {
        {"p10.jpg", p10},
        {"p2.jpg", p2},
        {"ComicInfo.xml", toBytes(xml)},
        {"p1.jpg", p1},
    });

    ASSERT_EQ(ArchiveEngine::detectFormat(cbtPath), ComicFormat::CBT);
    auto openRes = ArchiveEngine::openArchive(cbtPath);
    ASSERT_TRUE(openRes.isOk()) << openRes.error().message;
    auto& reader = *openRes.value();

    ASSERT_EQ(reader.pageCount(), 3u);
    EXPECT_EQ(reader.pageName(0), "p1.jpg");
    EXPECT_EQ(reader.pageName(1), "p2.jpg");
    EXPECT_EQ(reader.pageName(2), "p10.jpg");

    // Read out-of-order to verify backward seek reopens stream cleanly without temp files.
    auto r2 = reader.readPageBytes(2);
    ASSERT_TRUE(r2.isOk());
    EXPECT_EQ(r2.value(), p10);

    auto r0 = reader.readPageBytes(0);
    ASSERT_TRUE(r0.isOk());
    EXPECT_EQ(r0.value(), p1);

    auto r1 = reader.readPageBytes(1);
    ASSERT_TRUE(r1.isOk());
    EXPECT_EQ(r1.value(), p2);

    auto metaXml = reader.readMetadataXml();
    ASSERT_TRUE(metaXml.isOk());
    ASSERT_TRUE(metaXml.value().has_value());
    EXPECT_EQ(*metaXml.value(), xml);

    std::stop_source stopSource;
    stopSource.request_stop();
    auto cancelRes = reader.readPageBytes(0, stopSource.get_token());
    EXPECT_FALSE(cancelRes.isOk());
    EXPECT_EQ(cancelRes.errorCode(), ErrorCode::Cancelled);
}

TEST(ArchiveEngineIntegrationTest, PdfDocumentReaderStreams64BitAndExtractsMetadata) {
    TempDirGuard tmp(L"mavira_test_engine_pdf_\u65e5\u672c\u8a9e");
    const std::filesystem::path pdfPath = tmp.path / L"stream_test_\u7b2c1\u5dfb.pdf";
    createMinimalTwoPagePdf(pdfPath);

    ASSERT_EQ(ArchiveEngine::detectFormat(pdfPath), ComicFormat::PDF);

    PdfDocumentReader pdfReader;
    auto openRes = pdfReader.open(pdfPath);
    ASSERT_TRUE(openRes.isOk()) << openRes.error().message;
    EXPECT_TRUE(pdfReader.isOpen());
    EXPECT_EQ(pdfReader.pageCount(), 2u);
    EXPECT_GT(pdfReader.fileSizeBytes(), 0u);
    EXPECT_EQ(pdfReader.pageName(0), "page_1.pdf");
    EXPECT_EQ(pdfReader.pageName(1), "page_2.pdf");

    // Verify PDF Info dictionary metadata extraction
    const PdfMetadata pdfMeta = pdfReader.pdfMetadata();
    EXPECT_EQ(pdfMeta.title, "Mavira Test Volume");
    EXPECT_EQ(pdfMeta.author, "Taro Yamada");
    EXPECT_EQ(pdfMeta.subject, "Cyberpunk Anthology");
    EXPECT_EQ(pdfMeta.keywords, "sci-fi, manga, action");

    // Render page 0 and page 1 (verifying aspect ratio of 200x300 vs 400x200)
    auto frame0 = pdfReader.renderPage(0, 100, 0);
    ASSERT_TRUE(frame0.isOk()) << frame0.error().message;
    EXPECT_TRUE(frame0.value().isValid());
    EXPECT_EQ(frame0.value().width, 100u);
    EXPECT_EQ(frame0.value().height, 150u);

    auto frame1 = pdfReader.renderPage(1, 200, 0);
    ASSERT_TRUE(frame1.isOk()) << frame1.error().message;
    EXPECT_TRUE(frame1.value().isValid());
    EXPECT_EQ(frame1.value().width, 200u);
    EXPECT_EQ(frame1.value().height, 100u);

    // Verify IArchiveReader::readPageBytes returns valid BMP header ("BM")
    auto bmpRes = pdfReader.readPageBytes(0);
    ASSERT_TRUE(bmpRes.isOk());
    ASSERT_GE(bmpRes.value().size(), 54u);
    EXPECT_EQ(bmpRes.value()[0], 'B');
    EXPECT_EQ(bmpRes.value()[1], 'M');

    // Verify stop_token cancellation
    std::stop_source stopSource;
    stopSource.request_stop();
    auto cancelRes = pdfReader.renderPage(0, 100, 0, stopSource.get_token());
    EXPECT_FALSE(cancelRes.isOk());
    EXPECT_EQ(cancelRes.errorCode(), ErrorCode::Cancelled);

    // Verify ArchiveEngine::openArchive also opens PDFs via factory
    auto factoryRes = ArchiveEngine::openArchive(pdfPath);
    ASSERT_TRUE(factoryRes.isOk()) << factoryRes.error().message;
    EXPECT_EQ(factoryRes.value()->pageCount(), 2u);

    // Merge PDF metadata via MetadataManager
    const ComicMetadata merged =
        MetadataManager::extractMetadata(pdfPath, std::nullopt, pdfMeta, true);
    EXPECT_EQ(merged.title, "Mavira Test Volume");
    EXPECT_EQ(merged.writer, "Taro Yamada");
    EXPECT_EQ(merged.series, "Cyberpunk Anthology");
    ASSERT_EQ(merged.tags.size(), 3u);
    EXPECT_EQ(merged.tags[0], "sci-fi");
    EXPECT_EQ(merged.tags[1], "manga");
    EXPECT_EQ(merged.tags[2], "action");
}

TEST(MetadataManagerTest, RegexFallbackAndSmartRenameTitleCleaner) {
    // Pattern: [Author] Title (Event)
    const ComicMetadata parsed1 = MetadataManager::parseFromFilename(
        L"C:\\Comics\\[Studio Alpha] Starlight_Chronicles Vol. 04 (Comiket 104).cbz");
    EXPECT_EQ(parsed1.writer, "Studio Alpha");
    EXPECT_EQ(parsed1.event, "Comiket 104");
    EXPECT_EQ(parsed1.title, "Starlight Chronicles Vol. 04");
    EXPECT_EQ(parsed1.volumeNumber.value_or(0), 4);

    // Pattern: (Event) [Author] Title [Language]
    const ComicMetadata parsed2 = MetadataManager::parseFromFilename(
        L"(C102) [Hana_Works] Summer_Memories [EN].pdf");
    EXPECT_EQ(parsed2.writer, "Hana_Works");
    EXPECT_EQ(parsed2.event, "C102");
    EXPECT_EQ(parsed2.title, "Summer Memories");

    // Smart Rename visual title cleaner
    EXPECT_EQ(
        MetadataManager::cleanDisplayTitle("[Digital] One_Piece_Vol_105_(2023)_[ScanGroup].cbz"),
        "One Piece Vol 105");
    EXPECT_EQ(
        MetadataManager::cleanDisplayTitle("Berserk.Vol.01.(Dark.Horse).pdf"),
        "Berserk Vol 01");
}

TEST(ZipArchiveReaderTest, OpensUnicodeCbzIndexesNaturallyAndReadsInMemoryWithoutTempFiles) {
    TempDirGuard tmp(L"mavira_test_zip_unicode_\u65e5\u672c\u8a9e");
    const std::filesystem::path cbzPath =
        tmp.path / L"\u30de\u30f3\u30ac_\u7b2c1\u5dfb_Caf\u00e9.cbz";

    const std::vector<std::uint8_t> page1Bytes = {0xFF, 0xD8, 0xFF, 0xE0, 0x01, 0x02, 0x03};
    const std::vector<std::uint8_t> page2Bytes = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const std::vector<std::uint8_t> page10Bytes = {'R', 'I', 'F', 'F', 0x10, 0x20, 'W', 'E', 'B', 'P'};
    const std::string comicInfoXml =
        "<?xml version=\"1.0\"?><ComicInfo><Title>Unicode Volume 1</Title></ComicInfo>";

    createCbzWide(cbzPath, {
        {"page10.webp", page10Bytes},
        {"__MACOSX/._page1.jpg", toBytes("junk")},
        {"notes.txt", toBytes("ignore me")},
        {"page2.png", page2Bytes},
        {"ComicInfo.xml", toBytes(comicInfoXml)},
        {"page1.jpg", page1Bytes},
    });

    const std::size_t fileCountBefore = countFilesInDirectory(tmp.path);
    ASSERT_EQ(fileCountBefore, 1u);

    mavira::core::archive::ZipArchiveReader reader;
    auto openRes = reader.open(cbzPath);
    ASSERT_TRUE(openRes.isOk()) << openRes.error().message;
    EXPECT_TRUE(reader.isOpen());
    ASSERT_EQ(reader.pageCount(), 3u);

    // Natural sort order: page1.jpg < page2.png < page10.webp
    EXPECT_EQ(reader.pageName(0), "page1.jpg");
    EXPECT_EQ(reader.pageName(1), "page2.png");
    EXPECT_EQ(reader.pageName(2), "page10.webp");
    EXPECT_TRUE(reader.pageName(99).empty());

    // Read specific pages in arbitrary O(1) random order directly into memory.
    auto read2 = reader.readPageBytes(2);
    ASSERT_TRUE(read2.isOk());
    EXPECT_EQ(read2.value(), page10Bytes);

    auto read0 = reader.readPageBytes(0);
    ASSERT_TRUE(read0.isOk());
    EXPECT_EQ(read0.value(), page1Bytes);

    auto read1 = reader.readPageBytes(1);
    ASSERT_TRUE(read1.isOk());
    EXPECT_EQ(read1.value(), page2Bytes);

    // Metadata XML lookup
    auto xmlRes = reader.readMetadataXml();
    ASSERT_TRUE(xmlRes.isOk());
    ASSERT_TRUE(xmlRes.value().has_value());
    EXPECT_EQ(*xmlRes.value(), comicInfoXml);

    // Out-of-range page index
    auto oobRes = reader.readPageBytes(3);
    EXPECT_FALSE(oobRes.isOk());
    EXPECT_EQ(oobRes.errorCode(), ErrorCode::InvalidArgument);

    // Cooperative cancellation via std::stop_token
    std::stop_source stopSource;
    stopSource.request_stop();
    auto cancelRes = reader.readPageBytes(0, stopSource.get_token());
    EXPECT_FALSE(cancelRes.isOk());
    EXPECT_EQ(cancelRes.errorCode(), ErrorCode::Cancelled);

    // Verify no temporary files were extracted anywhere in the archive directory.
    EXPECT_EQ(countFilesInDirectory(tmp.path), fileCountBefore);

    reader.close();
    EXPECT_FALSE(reader.isOpen());
    EXPECT_EQ(reader.pageCount(), 0u);
}

TEST(ArchiveEngineTest, DetectsFormatsByMagicBytesAndOpensCbzEpubAndFolders) {
    TempDirGuard tmp(L"mavira_test_engine");

    // 1. ZIP file disguised with a wrong extension (.cbr) must be detected as CBZ via magic bytes!
    const std::filesystem::path misnamedZip = tmp.path / L"disguised.cbr";
    const std::vector<std::uint8_t> imgData = {0xFF, 0xD8, 0xFF, 0xD9};
    createCbzWide(misnamedZip, {{"001.jpg", imgData}});

    EXPECT_EQ(ArchiveEngine::detectFormat(misnamedZip), ComicFormat::CBZ);
    auto openMisnamed = ArchiveEngine::openArchive(misnamedZip);
    ASSERT_TRUE(openMisnamed.isOk()) << openMisnamed.error().message;
    EXPECT_EQ(openMisnamed.value()->pageCount(), 1u);
    auto page0 = openMisnamed.value()->readPageBytes(0);
    ASSERT_TRUE(page0.isOk());
    EXPECT_EQ(page0.value(), imgData);

    // 2. EPUB container with uncompressed mimetype entry
    const std::filesystem::path epubPath = tmp.path / L"manga.epub";
    createCbzWide(epubPath, {
        {"mimetype", toBytes("application/epub+zip"), true},
        {"OEBPS/Images/img_02.png", toBytes("png2")},
        {"OEBPS/Images/img_10.png", toBytes("png10")},
        {"OEBPS/Images/img_1.png", toBytes("png1")},
    });
    EXPECT_EQ(ArchiveEngine::detectFormat(epubPath), ComicFormat::EPUB);
    auto openEpub = ArchiveEngine::openArchive(epubPath);
    ASSERT_TRUE(openEpub.isOk()) << openEpub.error().message;
    ASSERT_EQ(openEpub.value()->pageCount(), 3u);
    EXPECT_EQ(openEpub.value()->pageName(0), "OEBPS/Images/img_1.png");
    EXPECT_EQ(openEpub.value()->pageName(1), "OEBPS/Images/img_02.png");
    EXPECT_EQ(openEpub.value()->pageName(2), "OEBPS/Images/img_10.png");

    // 3. Magic byte detection for PDF, RAR5, 7z
    auto writeRaw = [](const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
        std::ofstream ofs(p, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };

    const std::filesystem::path fakePdf = tmp.path / L"doc.bin";
    writeRaw(fakePdf, {'%', 'P', 'D', 'F', '-', '1', '.', '7'});
    EXPECT_EQ(ArchiveEngine::detectFormat(fakePdf), ComicFormat::PDF);

    const std::filesystem::path fakeRar = tmp.path / L"arc.bin";
    writeRaw(fakeRar, {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00});
    EXPECT_EQ(ArchiveEngine::detectFormat(fakeRar), ComicFormat::CBR);

    const std::filesystem::path fake7z = tmp.path / L"pack.bin";
    writeRaw(fake7z, {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04});
    EXPECT_EQ(ArchiveEngine::detectFormat(fake7z), ComicFormat::CB7);
}

TEST(FolderArchiveReaderTest, ReadsLooseImagesNaturallyWithMetadataAndCancellation) {
    TempDirGuard tmp(L"mavira_test_folder_\u7b2c2\u5dfb");

    auto writeFile = [](const std::filesystem::path& p, const std::string& content) {
        std::ofstream ofs(p, std::ios::binary);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    };

    writeFile(tmp.path / L"ch1_p10.jpg", "page10-data");
    writeFile(tmp.path / L"ch1_p2.jpg", "page2-data");
    writeFile(tmp.path / L"ch1_p1.png", "page1-data");
    writeFile(tmp.path / L"readme.txt", "not an image");
    writeFile(tmp.path / L"ComicInfo.xml", "<ComicInfo><Series>FolderTest</Series></ComicInfo>");

    EXPECT_EQ(ArchiveEngine::detectFormat(tmp.path), ComicFormat::Folder);

    auto openRes = ArchiveEngine::openArchive(tmp.path);
    ASSERT_TRUE(openRes.isOk()) << openRes.error().message;
    auto& reader = *openRes.value();

    ASSERT_EQ(reader.pageCount(), 3u);
    EXPECT_EQ(reader.pageName(0), "ch1_p1.png");
    EXPECT_EQ(reader.pageName(1), "ch1_p2.jpg");
    EXPECT_EQ(reader.pageName(2), "ch1_p10.jpg");

    auto p1 = reader.readPageBytes(1);
    ASSERT_TRUE(p1.isOk());
    EXPECT_EQ(p1.value(), toBytes("page2-data"));

    auto xml = reader.readMetadataXml();
    ASSERT_TRUE(xml.isOk());
    ASSERT_TRUE(xml.value().has_value());
    EXPECT_EQ(*xml.value(), "<ComicInfo><Series>FolderTest</Series></ComicInfo>");

    std::stop_source stopSource;
    stopSource.request_stop();
    auto cancelRes = reader.readPageBytes(0, stopSource.get_token());
    EXPECT_FALSE(cancelRes.isOk());
    EXPECT_EQ(cancelRes.errorCode(), ErrorCode::Cancelled);
}

} // namespace
