#include "data/fs/FileManager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mavira::data::fs::FileManager;

TEST(NaturalSortTest, OrdersNumericRunsByMagnitudeAcrossUtf8AndWide) {
    EXPECT_LT(FileManager::naturalCompare("page2.jpg", "page10.jpg"), 0);
    EXPECT_GT(FileManager::naturalCompare("page10.jpg", "page2.jpg"), 0);
    EXPECT_EQ(FileManager::naturalCompare("page10.jpg", "page10.jpg"), 0);

    EXPECT_TRUE(FileManager::naturalLess(L"ch1_p2.png", L"ch1_p10.png"));
    EXPECT_FALSE(FileManager::naturalLess(L"ch1_p10.png", L"ch1_p2.png"));
    EXPECT_TRUE(FileManager::naturalLess("vol2/001.webp", "vol10/001.webp"));
}

TEST(NaturalSortTest, HandlesLeadingZerosAndLargeNumericTokens) {
    // Same numeric value: shorter digit run or consistent tie-breaking, never crashes on >64-bit digits.
    EXPECT_TRUE(FileManager::naturalLess("img00000000000000000000000000000009.jpg",
                                         "img00000000000000000000000000000010.jpg"));
    EXPECT_NE(FileManager::naturalCompare("page01.jpg", "page1.jpg"), 0);
    EXPECT_EQ(FileManager::naturalCompare("page007.jpg", "page007.jpg"), 0);
}

TEST(NaturalSortTest, SortsMixedComicPageFilenamesCorrectly) {
    std::vector<std::wstring> pages = {
        L"Vol_10/page_100.jpg",
        L"Vol_2/page_10.jpg",
        L"Vol_2/page_2.jpg",
        L"Vol_1/page_1.jpg",
        L"Vol_2/page_1.jpg",
        L"Vol_10/page_20.jpg",
        L"Vol_1/page_12.jpg",
        L"Vol_1/page_2.jpg"
    };

    std::sort(pages.begin(), pages.end(), [](const std::wstring& a, const std::wstring& b) {
        return FileManager::naturalLess(a, b);
    });

    const std::vector<std::wstring> expected = {
        L"Vol_1/page_1.jpg",
        L"Vol_1/page_2.jpg",
        L"Vol_1/page_12.jpg",
        L"Vol_2/page_1.jpg",
        L"Vol_2/page_2.jpg",
        L"Vol_2/page_10.jpg",
        L"Vol_10/page_20.jpg",
        L"Vol_10/page_100.jpg"
    };
    EXPECT_EQ(pages, expected);
}

TEST(NaturalSortTest, HandlesEmptyStringsPrefixesAndUnicode) {
    EXPECT_LT(FileManager::naturalCompare("", "a"), 0);
    EXPECT_GT(FileManager::naturalCompare("a", ""), 0);
    EXPECT_EQ(FileManager::naturalCompare("", ""), 0);

    EXPECT_TRUE(FileManager::naturalLess("chapter", "chapter1"));
    EXPECT_TRUE(FileManager::naturalLess(L"第2巻.cbz", L"第10巻.cbz"));
}

} // namespace
