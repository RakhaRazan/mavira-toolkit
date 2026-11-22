#include "ui/LocalizationManager.hpp"

#include <unordered_map>

namespace mavira::ui {

namespace {

std::string gCurrentLang = "en";

struct LangEntry {
    const char* en;
    const char* id;
    const char* zh;
    const char* ja;
    const char* ko;
};

const std::unordered_map<std::string_view, LangEntry>& translationTable() {
    static const std::unordered_map<std::string_view, LangEntry> kTable = {
        // Menus
        {"menu.file", {"&File", "&Berkas", "文件(&F)", "ファイル(&F)", "파일(&F)"}},
        {"menu.edit", {"&Edit", "&Edit", "编辑(&E)", "編集(&E)", "편집(&E)"}},
        {"menu.tools", {"&Tools", "&Alat", "工具(&T)", "ツール(&T)", "도구(&T)"}},
        {"menu.view", {"&View", "&Tampilan", "视图(&V)", "表示(&V)", "보기(&V)"}},
        {"menu.help", {"&Help", "&Bantuan", "帮助(&H)", "ヘルプ(&H)", "도움말(&H)"}},

        // Top Bar & Reader Bar
        {"btn.add_folder", {"Add Folder", "Tambah Folder", "添加文件夹", "フォルダ追加", "폴더 추가"}},
        {"btn.open_file", {"Open File", "Buka Berkas", "打开文件", "ファイルを開く", "파일 열기"}},
        {"btn.random_unread", {"Random Unread", "Acak Belum Dibaca", "随机未读", "ランダム未読", "무작위 미열람"}},
        {"search.placeholder", {
            "Search library (title, series, author, tags, path)...",
            "Cari pustaka (judul, seri, penulis, tag, lokasi)...",
            "搜索书库 (标题、系列、作者、标签、路径)...",
            "ライブラリ検索 (タイトル、シリーズ、著者、タグ、パス)...",
            "라이브러리 검색 (제목, 시리즈, 작가, 태그, 경로)..."
        }},
        {"btn.library", {"Library", "Pustaka", "书库", "ライブラリ", "라이브러리"}},
        {"btn.reader", {"Reader", "Pembaca", "阅读器", "リーダー", "리더"}},
        {"btn.settings", {"Settings", "Pengaturan", "设置", "設定", "설정"}},
        {"btn.back_to_library", {"Back to Library (Esc)", "Kembali ke Pustaka (Esc)", "返回书库 (Esc)", "ライブラリへ戻る (Esc)", "라이브러리로 돌아가기 (Esc)"}},
        {"reader.no_volume", {"No Volume Loaded", "Tidak Ada Volume", "未加载卷册", "ボリューム未読み込み", "불러온 권 없음"}},
        {"strip.continue_reading", {"CONTINUE READING", "LANJUTKAN MEMBACA", "继续阅读", "続きを読む", "계속 읽기"}},
        {"status.ready", {"Ready", "Siap", "就绪", "準備完了", "준비됨"}},

        // Sidebar
        {"side.library", {"LIBRARY", "PUSTAKA", "书库", "ライブラリ", "라이브러리"}},
        {"side.all_comics", {"All Comics", "Semua Komik", "全部漫画", "すべてのコミック", "모든 만화"}},
        {"side.favorites", {"Favorites", "Favorit", "收藏夹", "お気に入り", "즐겨찾기"}},
        {"side.tools", {"TOOLS", "ALAT", "工具", "ツール", "도구"}},
        {"side.folders_series", {"FOLDERS & SERIES (A-Z)", "FOLDER & SERI (A-Z)", "文件夹与系列 (A-Z)", "フォルダ・シリーズ (A-Z)", "폴더 및 시리즈 (A-Z)"}},
        {"side.watched_folders", {"Watched Folders", "Folder Terpantau", "监控文件夹", "監視フォルダ", "감시 폴더"}},
        {"side.series_az", {"Series A-Z", "Seri A-Z", "系列 A-Z", "シリーズ A-Z", "시리즈 A-Z"}},
        {"side.shelves", {"SHELVES", "RAK", "书架", "本棚", "책장"}},
        {"side.smartviews", {"SMARTVIEWS", "TAMPILAN PINTAR", "智能视图", "スマートビュー", "스마트 뷰"}},
        {"side.cloud_sources", {"CLOUD SOURCES", "SUMBER CLOUD", "云端来源", "クラウドソース", "클라우드 소스"}},

        // Tools
        {"tool.convert_comics", {"Convert Comic Books", "Konversi Buku Komik", "转换漫画书", "コミック変換", "만화책 변환"}},
        {"tool.convert_images", {"Convert Images", "Konversi Gambar", "转换图像", "画像変換", "이미지 변환"}},
        {"tool.create_comic", {"Create Comic Book", "Buat Buku Komik", "创建漫画书", "コミック作成", "만화책 만들기"}},
        {"tool.extract_pages", {"Extract Pages", "Ekstrak Halaman", "提取页面", "ページ抽出", "페이지 추출"}},
        {"tool.extract_ocr", {"Extract Text (OCR)", "Ekstrak Teks (OCR)", "提取文本 (OCR)", "テキスト抽出 (OCR)", "텍스트 추출 (OCR)"}},
        {"tool.extract_ocr_badge", {"Extract Text (Native OCR)", "Ekstrak Teks (OCR Asli)", "提取文本 (原生 OCR)", "テキスト抽出 (ネイティブ OCR)", "텍스트 추출 (네이티브 OCR)"}},
        {"tool.merge_comics", {"Merge Comics", "Gabungkan Komik", "合并漫画", "コミック結合", "만화 병합"}},
        {"tool.close", {"CLOSE TOOL", "TUTUP ALAT", "关闭工具", "ツールを閉じる", "도구 닫기"}},
        {"tool.sections", {"WORKSPACE SECTIONS", "BAGIAN RUANG KERJA", "工作区部分", "ワークスペース項目", "작업 공간 섹션"}},
        {"tool.tab_general", {"General Options", "Opsi Umum", "常规选项", "一般オプション", "일반 옵션"}},
        {"tool.tab_advanced", {"Advanced Options", "Opsi Lanjutan", "高级选项", "詳細オプション", "고급 옵션"}},
        {"tool.tab_settings", {"Settings", "Pengaturan", "设置", "設定", "설정"}},
        {"tool.card_input", {"INPUT OPTIONS", "OPSI INPUT", "输入选项", "入力オプション", "입력 옵션"}},
        {"tool.card_output", {"OUTPUT OPTIONS", "OPSI OUTPUT", "输出选项", "出力オプション", "출력 옵션"}},
        {"tool.card_image", {"IMAGE PROCESSING OPTIONS", "OPSI PEMROSESAN GAMBAR", "图像处理选项", "画像処理オプション", "이미지 처리 옵션"}},
        {"tool.card_ocr", {"OCR EXTRACTED TEXT", "TEKS HASIL OCR", "OCR 提取文本", "OCR 抽出テキスト", "OCR 추출 텍스트"}},

        // Preferences
        {"prefs.back", {"BACK TO LIBRARY / READER", "KEMBALI KE PUSTAKA / PEMBACA", "返回书库 / 阅读器", "ライブラリ / リーダーへ戻る", "라이브러리 / 리더로 돌아가기"}},
        {"prefs.apply", {"APPLY CHANGES", "TERAPKAN PERUBAHAN", "应用更改", "変更を適用", "변경 사항 적용"}},
        {"prefs.reset_all", {"RESET ALL", "ATUR ULANG SEMUA", "全部重置", "すべてリセット", "모두 재설정"}},
        {"prefs.categories", {"CATEGORIES", "KATEGORI", "分类", "カテゴリ", "카테고리"}},
        {"prefs.badge", {"PREFERENCES", "PREFERENSI", "偏好设置", "環境設定", "환경 설정"}},
        {"prefs.cat.appearance", {"Appearance", "Tampilan", "外观", "外観", "모양"}},
        {"prefs.cat.ui", {"User Interface", "Antarmuka Pengguna", "用户界面", "ユーザーインターフェース", "사용자 인터페이스"}},
        {"prefs.cat.reader", {"Reader", "Pembaca", "阅读器", "リーダー", "리더"}},
        {"prefs.cat.library", {"Library & Storage", "Pustaka & Penyimpanan", "书库与存储", "ライブラリとストレージ", "라이브러리 및 저장소"}},
        {"prefs.cat.formats", {"File Formats", "Format Berkas", "文件格式", "ファイル形式", "파일 형식"}},
        {"prefs.cat.external", {"External Tools", "Alat Eksternal", "外部工具", "外部ツール", "외부 도구"}},
        {"prefs.cat.shortcuts", {"Shortcuts", "Pintasan", "快捷键", "ショートカット", "단축키"}},
        {"prefs.cat.advanced", {"Advanced", "Lanjutan", "高级", "詳細", "고급"}},
        {"prefs.lbl.theme", {"Color Theme:", "Tema Warna:", "颜色主题:", "カラーテーマ:", "색상 테마:"}},
        {"prefs.lbl.language", {"Interface Language:", "Bahasa Antarmuka:", "界面语言:", "表示言語:", "인터페이스 언어:"}},
        {"prefs.lbl.ui_scale", {"UI Scale:", "Skala UI:", "界面缩放:", "UI スケール:", "UI 배율:"}},
        {"prefs.lbl.card_density", {"Library Card Density:", "Kepadatan Kartu Pustaka:", "书库卡片密度:", "ライブラリカード密度:", "라이브러리 카드 밀도:"}}
    };
    return kTable;
}

} // namespace

void LocalizationManager::setLanguage(std::string_view langCode) {
    if (langCode == "id" || langCode == "zh" || langCode == "ja" || langCode == "ko") {
        gCurrentLang = std::string(langCode);
    } else {
        gCurrentLang = "en";
    }
}

const std::string& LocalizationManager::currentLanguage() noexcept {
    return gCurrentLang;
}

QString LocalizationManager::tr(std::string_view key) {
    const auto& table = translationTable();
    const auto it = table.find(key);
    if (it == table.end()) {
        return QString::fromUtf8(key.data(), static_cast<int>(key.size()));
    }
    const LangEntry& e = it->second;
    const char* text = e.en;
    if (gCurrentLang == "id") text = e.id;
    else if (gCurrentLang == "zh") text = e.zh;
    else if (gCurrentLang == "ja") text = e.ja;
    else if (gCurrentLang == "ko") text = e.ko;
    return QString::fromUtf8(text);
}

} // namespace mavira::ui
