#include "ui/LocalizationManager.hpp"

#include <QByteArray>
#include <QDebug>

#include <cctype>
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

QString humanizeKeyToken(std::string_view key) {
    const std::size_t dotPos = key.rfind('.');
    const std::string_view token = (dotPos != std::string_view::npos && dotPos + 1 < key.size())
        ? key.substr(dotPos + 1)
        : key;
    std::string out;
    out.reserve(token.size());
    bool capNext = true;
    for (unsigned char ch : token) {
        if (ch == '_' || ch == '-') {
            out.push_back(' ');
            capNext = true;
        } else if (capNext) {
            out.push_back(static_cast<char>(std::toupper(ch)));
            capNext = false;
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return QString::fromUtf8(out.data(), static_cast<int>(out.size()));
}

const std::unordered_map<std::string_view, LangEntry>& translationTable() {
    static const std::unordered_map<std::string_view, LangEntry> kTable = {
        // Menus
        {"menu.file", {"&File", "&Berkas", "文件(&F)", "ファイル(&F)", "파일(&F)"}},
        {"menu.file.open_file", {"Open File...", "Buka Berkas...", "打开文件...", "ファイルを開く...", "파일 열기..."}},
        {"menu.file.add_folder", {"Add Watched Folder...", "Tambah Folder Terpantau...", "添加监控文件夹...", "監視フォルダを追加...", "감시 폴더 추가..."}},
        {"menu.file.prune_unavailable", {"Remove Unavailable Entries", "Hapus Entri Tidak Tersedia", "移除不可用条目", "利用不可のエントリを削除", "사용할 수 없는 항목 제거"}},
        {"menu.file.exit", {"E&xit", "&Keluar", "退出(&X)", "終了(&X)", "종료(&X)"}},

        {"menu.edit", {"&Edit", "&Edit", "编辑(&E)", "編集(&E)", "편집(&E)"}},
        {"menu.edit.select_all", {"Select All", "Pilih Semua", "全选", "すべて選択", "모두 선택"}},
        {"menu.edit.remove_selected", {"Remove Selected from Library", "Hapus Pilihan dari Pustaka", "从书库移除所选", "選択項目をライブラリから削除", "선택 항목을 라이브러리에서 제거"}},
        {"menu.edit.delete_selected", {"Delete Selected to Recycle Bin", "Hapus Pilihan ke Keranjang Sampah", "将所选移至回收站", "選択項目をごみ箱へ移動", "선택 항목을 휴지통으로 삭제"}},
        {"menu.edit.preferences", {"Preferences / Settings...", "Preferensi / Pengaturan...", "偏好设置 / 设置...", "環境設定 / 設定...", "환경 설정 / 설정..."}},

        {"menu.tools", {"&Tools", "&Alat", "工具(&T)", "ツール(&T)", "도구(&T)"}},
        {"menu.tools.convert", {"Convert", "Konversi", "转换", "変換", "변환"}},
        {"menu.tools.convert_comics", {"Comic Books (CBR/PDF -> CBZ/PDF)...", "Buku Komik (CBR/PDF -> CBZ/PDF)...", "漫画书 (CBR/PDF -> CBZ/PDF)...", "コミック (CBR/PDF -> CBZ/PDF)...", "만화책 (CBR/PDF -> CBZ/PDF)..."}},
        {"menu.tools.convert_images", {"Images (JPG/PNG/WEBP)...", "Gambar (JPG/PNG/WEBP)...", "图像 (JPG/PNG/WEBP)...", "画像 (JPG/PNG/WEBP)...", "이미지 (JPG/PNG/WEBP)..."}},
        {"menu.tools.create", {"Create", "Buat", "创建", "作成", "만들기"}},
        {"menu.tools.create_comic", {"Comic Book from Folder (CBZ/PDF)...", "Buku Komik dari Folder (CBZ/PDF)...", "从文件夹创建漫画书 (CBZ/PDF)...", "フォルダからコミックを作成 (CBZ/PDF)...", "폴더에서 만화책 만들기 (CBZ/PDF)..."}},
        {"menu.tools.extract", {"Extract", "Ekstrak", "提取", "抽出", "추출"}},
        {"menu.tools.extract_pages", {"Pages from Comic Book...", "Halaman dari Buku Komik...", "从漫画书提取页面...", "コミックからページを抽出...", "만화책에서 페이지 추출..."}},
        {"menu.tools.extract_ocr", {"Text from Image (Native OCR)...", "Teks dari Gambar (OCR Asli)...", "从图像提取文本 (原生 OCR)...", "画像からテキスト抽出 (ネイティブ OCR)...", "이미지에서 텍스트 추출 (네이티브 OCR)..."}},
        {"menu.tools.merge", {"Merge", "Gabungkan", "合并", "結合", "병합"}},
        {"menu.tools.merge_comics", {"Merge Multiple Comics (CBR/CBZ/PDF)...", "Gabungkan Beberapa Komik (CBR/CBZ/PDF)...", "合并多部漫画 (CBR/CBZ/PDF)...", "複数コミックを結合 (CBR/CBZ/PDF)...", "여러 만화 병합 (CBR/CBZ/PDF)..."}},

        {"menu.view", {"&View", "&Tampilan", "视图(&V)", "表示(&V)", "보기(&V)"}},
        {"menu.view.library_grid", {"Library Grid", "Kisi Pustaka", "书库网格", "ライブラリグリッド", "라이브러리 그리드"}},
        {"menu.view.sidebar_browser", {"Folder / Series Browser", "Peramban Folder / Seri", "文件夹 / 系列浏览器", "フォルダ / シリーズブラウザ", "폴더 / 시리즈 브라우저"}},
        {"menu.view.reader_view", {"Reader View", "Tampilan Pembaca", "阅读器视图", "リーダー表示", "리더 보기"}},
        {"menu.view.toggle_theme", {"Toggle Dark / Light Theme", "Ganti Tema Gelap / Terang", "切换深色 / 浅色主题", "ダーク / ライトテーマ切替", "다크 / 라이트 테마 전환"}},

        {"menu.help", {"&Help", "&Bantuan", "帮助(&H)", "ヘルプ(&H)", "도움말(&H)"}},
        {"menu.help.shortcuts", {"Keyboard Shortcuts", "Pintasan Papan Ketik", "键盘快捷键", "キーボードショートカット", "키보드 단축키"}},
        {"menu.help.about", {"Open-Source Licenses && About MAVIRA", "Lisensi Sumber Terbuka && Tentang MAVIRA", "开源许可 && 关于 MAVIRA", "オープンソースライセンス && MAVIRA について", "오픈 소스 라이선스 && MAVIRA 정보"}},

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
        {"reader.hud.library", {"Library", "Pustaka", "书库", "ライブラリ", "라이브러리"}},
        {"reader.hud.bookmark", {"Bookmark", "Markah", "书签", "ブックマーク", "북마크"}},
        {"reader.hud.bookmarked", {"Bookmarked", "Dimarkahi", "已加书签", "ブックマーク済み", "북마크됨"}},
        {"reader.hud.autocrop", {"Auto-Crop", "Potong Otomatis", "自动裁剪", "自動トリミング", "자동 자르기"}},
        {"strip.continue_reading", {"CONTINUE READING", "LANJUTKAN MEMBACA", "继续阅读", "続きを読む", "계속 읽기"}},
        {"library.header.all_comics", {"ALL COMICS", "SEMUA KOMIK", "全部漫画", "すべてのコミック", "모든 만화"}},
        {"sort.title_az", {"Sort: Title A-Z", "Urut: Judul A-Z", "排序: 标题 A-Z", "並び替え: タイトル A-Z", "정렬: 제목 A-Z"}},
        {"sort.recently_added", {"Sort: Recently Added", "Urut: Baru Ditambahkan", "排序: 最近添加", "並び替え: 追加順", "정렬: 최근 추가"}},
        {"sort.last_read", {"Sort: Last Read", "Urut: Terakhir Dibaca", "排序: 最近阅读", "並び替え: 最近読んだ順", "정렬: 최근 읽은 순"}},
        {"sort.file_size", {"Sort: File Size", "Urut: Ukuran Berkas", "排序: 文件大小", "並び替え: ファイルサイズ", "정렬: 파일 크기"}},
        {"filter.all", {"Status: All", "Status: Semua", "状态: 全部", "状態: すべて", "상태: 전체"}},
        {"filter.unread", {"Unread", "Belum Dibaca", "未读", "未読", "미열람"}},
        {"filter.in_progress", {"In Progress", "Sedang Dibaca", "阅读中", "読書中", "읽는 중"}},
        {"filter.finished", {"Finished", "Selesai", "已完成", "既読", "완독"}},
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
        {"tool.btn.convert", {"CONVERT", "KONVERSI", "转换", "変換", "변환"}},
        {"tool.btn.create", {"CREATE", "BUAT", "创建", "作成", "만들기"}},
        {"tool.btn.extract", {"EXTRACT", "EKSTRAK", "提取", "抽出", "추출"}},
        {"tool.btn.ocr", {"RUN OCR", "JALANKAN OCR", "运行 OCR", "OCR 実行", "OCR 실행"}},
        {"tool.btn.run_ocr", {"RUN OCR", "JALANKAN OCR", "运行 OCR", "OCR 実行", "OCR 실행"}},
        {"tool.btn.merge", {"MERGE", "GABUNGKAN", "合并", "結合", "병합"}},
        {"tool.btn.cancel", {"CANCEL", "BATAL", "取消", "キャンセル", "취소"}},
        {"tool.btn.close", {"CLOSE TOOL", "TUTUP ALAT", "关闭工具", "ツールを閉じる", "도구 닫기"}},
        {"tool.btn.add_files", {"ADD FILES", "TAMBAH BERKAS", "添加文件", "ファイル追加", "파일 추가"}},
        {"tool.btn.add_folders", {"ADD FOLDERS", "TAMBAH FOLDER", "添加文件夹", "フォルダ追加", "폴더 추가"}},
        {"tool.btn.clear_list", {"CLEAR LIST", "BERSIHKAN DAFTAR", "清空列表", "リストをクリア", "목록 지우기"}},
        {"tool.btn.move_up", {"MOVE UP", "NAIK", "上移", "上へ", "위로"}},
        {"tool.btn.move_down", {"MOVE DOWN", "TURUN", "下移", "下へ", "아래로"}},
        {"tool.btn.change", {"CHANGE", "UBAH", "更改", "変更", "변경"}},
        {"tool.btn.open", {"OPEN", "BUKA", "打开", "開く", "열기"}},
        {"tool.btn.copy_text", {"COPY TEXT", "SALIN TEKS", "复制文本", "テキストをコピー", "텍스트 복사"}},
        {"tool.btn.save_txt", {"SAVE .TXT", "SIMPAN .TXT", "保存 .TXT", ".TXT を保存", ".TXT 저장"}},
        {"tool.btn.reset_image_ops", {"RESET IMAGE OPS", "ATUR ULANG OPSI GAMBAR", "重置图像处理", "画像処理をリセット", "이미지 처리 재설정"}},
        {"tool.btn.reset_options", {"RESET OPTIONS", "ATUR ULANG OPSI", "重置选项", "オプションをリセット", "옵션 재설정"}},
        {"tool.sections", {"WORKSPACE SECTIONS", "BAGIAN RUANG KERJA", "工作区部分", "ワークスペース項目", "작업 공간 섹션"}},
        {"tool.tab_general", {"General Options", "Opsi Umum", "常规选项", "一般オプション", "일반 옵션"}},
        {"tool.tab_advanced", {"Advanced Options", "Opsi Lanjutan", "高级选项", "詳細オプション", "고급 옵션"}},
        {"tool.tab_settings", {"Settings", "Pengaturan", "设置", "設定", "설정"}},
        {"tool.card_input", {"INPUT OPTIONS", "OPSI INPUT", "输入选项", "入力オプション", "입력 옵션"}},
        {"tool.card_output", {"OUTPUT OPTIONS", "OPSI OUTPUT", "输出选项", "出力オプション", "출력 옵션"}},
        {"tool.card_image", {"IMAGE PROCESSING OPTIONS", "OPSI PEMROSESAN GAMBAR", "图像处理选项", "画像処理オプション", "이미지 처리 옵션"}},
        {"tool.card_ocr", {"OCR EXTRACTED TEXT", "TEKS HASIL OCR", "OCR 提取文本", "OCR 抽出テキスト", "OCR 추출 텍스트"}},
        {"tool.card_adv_input", {"ADVANCED INPUT OPTIONS", "OPSI INPUT LANJUTAN", "高级输入选项", "詳細入力オプション", "고급 입력 옵션"}},
        {"tool.card_adv_output", {"ADVANCED OUTPUT OPTIONS", "OPSI OUTPUT LANJUTAN", "高级输出选项", "詳細出力オプション", "고급 출력 옵션"}},
        {"tool.card_adv_image", {"ADVANCED IMAGE PROCESSING OPTIONS", "OPSI PEMROSESAN GAMBAR LANJUTAN", "高级图像处理选项", "詳細画像処理オプション", "고급 이미지 처리 옵션"}},
        {"tool.card_settings", {"SETTINGS", "PENGATURAN", "设置", "設定", "설정"}},
        {"tool.chk.remember_last", {"Remember last used options for each tool", "Ingat opsi terakhir yang digunakan untuk setiap alat", "记住每个工具上次使用的选项", "各ツールの前回オプションを記憶する", "각 도구에서 마지막으로 사용한 옵션 기억"}},

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
        {"prefs.lbl.card_density", {"Library Card Density:", "Kepadatan Kartu Pustaka:", "书库卡片密度:", "ライブラリカード密度:", "라이브러리 카드 밀도:"}},
        {"prefs.card.appearance", {"APPEARANCE", "TAMPILAN", "外观", "外観", "모양"}},
        {"prefs.card.ui", {"USER INTERFACE", "ANTARMUKA PENGGUNA", "用户界面", "ユーザーインターフェース", "사용자 인터페이스"}},
        {"prefs.card.page_layout", {"PAGE LAYOUT & CANVAS", "TATA LETAK HALAMAN & KANVAS", "页面布局与画布", "ページレイアウトとキャンバス", "페이지 레이아웃 및 캔버스"}},
        {"prefs.card.reader_behaviour", {"READER BEHAVIOUR & TOOLS", "PERILAKU PEMBACA & ALAT", "阅读器行为与工具", "リーダー動作とツール", "리더 동작 및 도구"}},
        {"prefs.card.ram_cache", {"RAM PAGE CACHE & LIBRARY", "TEMBOLOK RAM HALAMAN & PUSTAKA", "RAM 页面缓存与书库", "RAM ページキャッシュとライブラリ", "RAM 페이지 캐시 및 라이브러리"}},
        {"prefs.card.cover_cache", {"COVER CACHE STORAGE", "PENYIMPANAN TEMBOLOK SAMPUL", "封面缓存存储", "カバーキャッシュ保存先", "표지 캐시 저장소"}},
        {"prefs.card.file_formats", {"FILE FORMAT DEFAULTS", "BAKU FORMAT BERKAS", "文件格式默认值", "ファイル形式の既定値", "파일 형식 기본값"}},
        {"prefs.card.scanned_formats", {"SCANNED LIBRARY FORMATS", "FORMAT PUSTAKA TERPINDAI", "扫描书库格式", "スキャン対象ライブラリ形式", "스캔할 라이브러리 형식"}},
        {"prefs.card.external_app", {"EXTERNAL COMPANION APP", "APLIKASI PENDAMPING EKSTERNAL", "外部配套应用", "外部連携アプリ", "외부 연동 앱"}},
        {"prefs.card.shortcuts", {"KEYBOARD SHORTCUTS", "PINTASAN PAPAN KETIK", "键盘快捷键", "キーボードショートカット", "키보드 단축키"}},
        {"prefs.card.diagnostics", {"DIAGNOSTICS & MAINTENANCE", "DIAGNOSTIK & PEMELIHARAAN", "诊断与维护", "診断とメンテナンス", "진단 및 유지 관리"}},
        {"prefs.card.portable_paths", {"PORTABLE DATA PATHS", "LOKASI DATA PORTABEL", "便携数据路径", "ポータブルデータパス", "포터블 데이터 경로"}},
        {"prefs.btn.build_pack", {"BUILD / REBUILD COVERS.PACK", "BANGUN / BANGUN ULANG COVERS.PACK", "构建 / 重建 COVERS.PACK", "COVERS.PACK を構築 / 再構築", "COVERS.PACK 빌드 / 재빌드"}},
        {"prefs.btn.purge_cache", {"PURGE COVER CACHE", "BERSIHKAN TEMBOLOK SAMPUL", "清除封面缓存", "カバーキャッシュを削除", "표지 캐시 비우기"}},
        {"prefs.btn.clear_offline", {"CLEAR OFFLINE CACHE", "BERSIHKAN TEMBOLOK LURING", "清除离线缓存", "オフラインキャッシュを削除", "오프라인 캐시 지우기"}},
        {"prefs.btn.browse", {"BROWSE...", "JELAJAHI...", "浏览...", "参照...", "찾아보기..."}},
        {"prefs.btn.reset_shortcuts", {"RESET ALL SHORTCUTS", "ATUR ULANG SEMUA PINTASAN", "重置所有快捷键", "すべてのショートカットをリセット", "모든 단축키 재설정"}},
        {"prefs.btn.open_data_folder", {"OPEN PORTABLE DATA FOLDER", "BUKA FOLDER DATA PORTABEL", "打开便携数据文件夹", "ポータブルデータフォルダを開く", "포터블 데이터 폴더 열기"}},
        {"prefs.btn.optimize_db", {"OPTIMIZE SQLITE DATABASE", "OPTIMALKAN BASIS DATA SQLITE", "优化 SQLITE 数据库", "SQLITE データベースを最適化", "SQLITE 데이터베이스 최적화"}},
        {"prefs.btn.reset_prefs", {"RESET ALL PREFERENCES", "ATUR ULANG SEMUA PREFERENSI", "重置所有偏好设置", "すべての環境設定をリセット", "모든 환경 설정 재설정"}}
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
        qWarning() << "LocalizationManager: missing translation key:"
                   << QByteArray(key.data(), static_cast<int>(key.size()));
        return humanizeKeyToken(key);
    }
    const LangEntry& e = it->second;
    const char* text = e.en;
    if (gCurrentLang == "id") text = e.id;
    else if (gCurrentLang == "zh") text = e.zh;
    else if (gCurrentLang == "ja") text = e.ja;
    else if (gCurrentLang == "ko") text = e.ko;

    if (text == nullptr || text[0] == '\0') {
        text = e.en;
    }
    if (text == nullptr || text[0] == '\0') {
        qWarning() << "LocalizationManager: empty translation for key:"
                   << QByteArray(key.data(), static_cast<int>(key.size()));
        return humanizeKeyToken(key);
    }
    return QString::fromUtf8(text);
}

} // namespace mavira::ui
