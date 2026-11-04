#include "backup.hpp"
#include "bgdl.hpp"
#include "catalog.hpp"
#include "gfx.hpp"
#include "images.hpp"
#include "ime.hpp"
#include "net.hpp"
#include "pkg.hpp"
#include "promote.hpp"
#include "tasks.hpp"
#include "util.hpp"

#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/cpu.h>

#include <json/json.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

#define VPKGJ_VERSION "1.1"
#define VPKGJ_AUTHOR "Risaro"
#define VPKGJ_TG "t.me/lik1sa"
#define VPKGJ_FOR "phc.lokilab.ru"
#define APP_REPO "Risaro/vpkgj"

int _newlib_heap_size_user = 160 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;

static const int W = 960;
static const int H = 544;
static const int HEADER_H = 64;
static const int FOOTER_Y = 508;
static const int COLS = 4;
static const int CARD_W = 206;
static const int CARD_IMG_H = 116;
static const int CARD_H = 174;
static const int GAP_X = 18;
static const int GAP_Y = 20;
static const int GRID_X = (W - (COLS * CARD_W + (COLS - 1) * GAP_X)) / 2;
static const int GRID_TOP = HEADER_H + 14;
static const int TAB_UPDATES = CAT_COUNT;
static const int TAB_DOWNLOADS = CAT_COUNT + 1;
static const int TAB_COUNT = CAT_COUNT + 2;

enum Screen { SCR_BROWSE, SCR_DETAIL, SCR_SETTINGS, SCR_BACKUPS, SCR_PRELOAD };
enum ImePurpose { IME_NONE, IME_SEARCH, IME_REPO, IME_BRANCH };

struct Hit {
    float x, y, w, h;
    std::function<void()> fn;
};

struct Modal {
    bool on = false;
    std::string title;
    std::vector<std::string> lines;
    std::string yes;
    std::function<void()> action;
    std::string alt;
    std::function<void()> alt_action;
};

struct App {
    Settings settings;
    Catalog catalog;
    Installed installed;
    Tasks tasks;
    Images images;

    Screen screen = SCR_PRELOAD;
    int tab = CAT_PORTS;
    int sel[TAB_COUNT] = {0};
    float scroll[TAB_COUNT] = {0};
    float scroll_target[TAB_COUNT] = {0};
    std::vector<int> filtered[CAT_COUNT];
    std::string query;

    Entry detail;
    int shot = 0;
    int prev_shot = -1;
    uint64_t shot_time = 0;
    uint64_t shot_auto = 0;
    float desc_scroll = 0;
    float desc_target = 0;
    std::vector<std::string> desc_lines;
    std::vector<Entry> detail_deps;
    std::vector<Entry> related;
    std::vector<std::string> related_kind;
    std::vector<Entry> back_stack;
    int rel_sel = -1;
    bool viewer = false;
    void draw_viewer();
    bool has_saves = false;
    float rel_scroll = 0;
    void go_back();
    void draw_related();
    std::vector<bool> detail_dep_ok;
    void check_deps();

    int settings_sel = 0;
    int backup_sel = 0;
    std::vector<Backup> backups;
    void draw_backups();
    void open_backups();
    void saves_menu(const Entry& e);
    void check_reboot();
    bool daemon_enabled();
    void daemon_notify(const std::string& text);
    void toggle_daemon();
    int daemon_state = -1;
    int dl_sel = 0;

    float fx = 0, fy = 0, fw = 0, fh = 0;
    bool focus_init = false;

    Modal modal;
    std::string toast;
    uint64_t toast_until = 0;
    ImePurpose ime_purpose = IME_NONE;

    std::vector<Hit> hits;

    Mutex refresh_m;
    std::atomic<int> refreshing{0};
    std::unique_ptr<Catalog> fresh;
    std::string refresh_err;
    bool manual_refresh = false;
    bool loaded_once = false;

    uint32_t ok_btn = SCE_CTRL_CROSS;
    uint32_t back_btn = SCE_CTRL_CIRCLE;
    Btn ok_icon = BTN_CROSS;
    Btn back_icon = BTN_CIRCLE;

    uint32_t prev_buttons = 0;
    uint32_t pressed = 0;
    uint32_t repeat = 0;
    uint64_t repeat_at = 0;
    bool touching = false;
    float tx0 = 0, ty0 = 0, tx = 0, ty = 0;
    bool dragged = false;

    std::vector<std::function<void()>> settings_actions;
    int speed_mode = 0;
    void load_prefs();
    void save_prefs();
    void apply_speed();
    std::string bg_name;
    uint64_t bg_done = 0, bg_total = 0;
    int bg_left = 0;
    uint64_t bg_polled = 0;
    void poll_bg();
    void queue_background(const Entry& e);
    void cancel_background();
    void ask_daemon();
    void ask_app_update(bool manual);
    bool app_update_available();
    bool app_prompted = false;
    bool ignored_app(const std::string& v)
    {
        std::string s;
        return fs::read(APP_DIR + "/app_skip.txt", s) && trim(s) == v;
    }
    void draw_about(float y);
    void start_preload();
    void draw_preload();
    void input_preload();
    uint64_t pre_t = 0;
    uint64_t pre_b = 0;
    double pre_speed = 0;
    bool pre_checked = false;
    uint64_t free_bytes = 0, free_at = 0;
    uint64_t free_cached()
    {
        if (!free_at || now_ms() - free_at > 3000) {
            free_bytes = fs::free_space("ux0:");
            free_at = now_ms();
        }
        return free_bytes;
    }
    void run();
    void input();
    void touch();
    void tap(float x, float y);
    void draw();
    void draw_header();
    void draw_grid();
    void draw_card(const Entry& e, float x, float y);
    void draw_detail();
    void draw_downloads();
    void draw_settings();
    void draw_footer();
    void draw_modal();
    void draw_toast();
    void draw_badge(float x, float y, const std::string& s, unsigned bg, unsigned fg);

    void rebuild();
    std::vector<Entry> deps_of(const Entry& e);
    std::string icon_url(const Entry& e);
    Status status_of(const Entry& e);
    std::vector<Entry> collect_updates();
    std::vector<Entry> collect_alt();
    std::vector<Entry> upd;
    std::vector<bool> upd_alt;
    int upd_sel = 0;
    void refresh_updates();
    void draw_updates();
    void input_updates(uint32_t act);
    void ask_alt(const Entry& e);
    bool ignored(const Entry& e, const std::string& version);
    std::set<std::string> ignored_set;
    bool ignored_loaded = false;
    void ignore(const Entry& e, const std::string& version);
    struct View {
        int sort = 0;
        int region = 0;
        bool flag[16] = {};
    };
    View views[CAT_COUNT];
    std::string query_lower;
    std::set<std::string> apps;
    uint64_t apps_scanned_at = 0;
    bool fs_app(const std::string& id);
    void load_views();
    void save_views();
    void rebuild_cat(int c);
    bool filters_open = false;
    int filter_sel = 0;
    void draw_filters();
    void input_filters(uint32_t act);
    std::string filter_summary();
    void notify_updates();
    int updates_count = 0;
    std::vector<Entry>& list(int c) { return catalog.items[c]; }
    const Entry* selected();
    void open_detail(const Entry& e);
    void primary_action();
    void ask_install(const Entry& e, bool reinstall);
    void launch(const Entry& e);
    void background_download(const Entry& e);
    void show_toast(const std::string& s);
    void start_refresh(bool manual);
    static int refresh_thread(unsigned int, void* arg);
    void apply_refresh();
    void ime_done(const std::string& text);
    void set_tab(int t);
    void move_sel(int dx, int dy);
    void ensure_visible();
};

static bool launchable(const Entry& e)
{
    return e.cat == CAT_PORTS || e.cat == CAT_HOMEBREW || (e.cat == CAT_GAMES && !e.dlc);
}

static uint32_t read_buttons()
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t b = pad.buttons;
    if (pad.ly < 40)
        b |= SCE_CTRL_UP;
    if (pad.ly > 215)
        b |= SCE_CTRL_DOWN;
    if (pad.lx < 40)
        b |= SCE_CTRL_LEFT;
    if (pad.lx > 215)
        b |= SCE_CTRL_RIGHT;
    if (pad.ry < 50)
        b |= 0x10000000;
    if (pad.ry > 205)
        b |= 0x20000000;
    return b;
}

static const uint32_t RS_UP = 0x10000000;
static const uint32_t RS_DOWN = 0x20000000;

void App::show_toast(const std::string& s)
{
    toast = s;
    toast_until = now_ms() + 2600;
}

enum SortKind { S_CATALOG, S_NAME, S_NEW, S_BIG, S_SMALL, S_REGION, S_AUTHOR, S_GAME, S_SECTION };
enum FilterKind { F_REGION, F_RU, F_INSTALLED, F_UPDATES, F_HIDE_DLC, F_ONLY_DLC, F_DATA, F_OWN_FILES, F_GAME_HERE, F_COUNT };

static const char* SORT_NAMES[] = {"Как в каталоге", "По названию", "Сначала новые", "Сначала большие", "Сначала маленькие", "По региону", "По автору", "По игре", "По разделу"};
static const char* FILTER_NAMES[] = {"Регион", "Есть русский язык", "Только установленные", "Только с обновлением", "Скрыть DLC", "Только DLC", "Data скачается сама", "Нужны свои файлы", "Игра есть на консоли"};
static const char* REGION_NAMES[] = {"Все", "EU", "US", "JP", "ASIA"};

static const std::vector<int>& sorts_for(int c)
{
    static const std::vector<int> v[CAT_COUNT] = {
        {S_CATALOG, S_NAME, S_NEW, S_REGION, S_BIG, S_SMALL},
        {S_CATALOG, S_NAME, S_AUTHOR, S_NEW, S_BIG, S_SMALL},
        {S_CATALOG, S_NAME, S_GAME, S_NEW, S_BIG, S_SMALL},
        {S_CATALOG, S_NAME, S_AUTHOR, S_NEW, S_BIG, S_SMALL},
        {S_CATALOG, S_NAME, S_SECTION, S_AUTHOR},
    };
    return v[c];
}

static const std::vector<int>& filters_for(int c)
{
    static const std::vector<int> v[CAT_COUNT] = {
        {F_REGION, F_RU, F_INSTALLED, F_UPDATES, F_HIDE_DLC, F_ONLY_DLC},
        {F_DATA, F_OWN_FILES, F_RU, F_INSTALLED, F_UPDATES},
        {F_GAME_HERE, F_INSTALLED, F_UPDATES},
        {F_RU, F_INSTALLED, F_UPDATES},
        {F_INSTALLED, F_UPDATES},
    };
    return v[c];
}

static int region_rank(const std::string& r)
{
    for (int i = 1; i < 5; i++)
        if (r == REGION_NAMES[i])
            return i;
    return 5;
}

void App::load_views()
{
    std::string s;
    if (!fs::read(APP_DIR + "/view.txt", s))
        return;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        std::string line = s.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? s.size() : e + 1;
        int c = -1, sort = 0, region = 0;
        unsigned flags = 0;
        if (sscanf(line.c_str(), "%d %d %d %u", &c, &sort, &region, &flags) == 4 && c >= 0 && c < CAT_COUNT) {
            View& v = views[c];
            v.sort = sort >= 0 && sort < (int)sorts_for(c).size() ? sort : 0;
            v.region = region >= 0 && region < 5 ? region : 0;
            for (int i = 0; i < F_COUNT; i++)
                v.flag[i] = (flags >> i) & 1;
        }
    }
}

void App::save_views()
{
    std::string s;
    for (int c = 0; c < CAT_COUNT; c++) {
        unsigned flags = 0;
        for (int i = 0; i < F_COUNT; i++)
            if (views[c].flag[i])
                flags |= 1u << i;
        char line[64];
        snprintf(line, sizeof(line), "%d %d %d %u\n", c, views[c].sort, views[c].region, flags);
        s += line;
    }
    fs::write(APP_DIR + "/view.txt", s);
}

void App::rebuild_cat(int c)
{
    const std::string& q = query_lower;
    const View& v = views[c];
    auto& items = catalog.items[c];
    auto& out = filtered[c];
    out.clear();
    out.reserve(items.size());
    bool need_status = v.flag[F_INSTALLED] || v.flag[F_UPDATES];
    for (int i = 0; i < (int)items.size(); i++) {
        const Entry& e = items[i];
        if (!q.empty() && e.search.find(q) == std::string::npos)
            continue;
        if (c == CAT_GAMES) {
            if (v.region && e.region != REGION_NAMES[v.region])
                continue;
            if (v.flag[F_HIDE_DLC] && e.dlc)
                continue;
            if (v.flag[F_ONLY_DLC] && !e.dlc)
                continue;
        }
        if (v.flag[F_RU] && !e.ru)
            continue;
        if (v.flag[F_DATA] && e.data.empty())
            continue;
        if (v.flag[F_OWN_FILES] && !(e.data.empty() && !e.data_path.empty()))
            continue;
        if (v.flag[F_GAME_HERE] && !fs_app(e.id))
            continue;
        if (need_status) {
            Status st = status_of(e);
            if (v.flag[F_INSTALLED] && st == ST_NONE)
                continue;
            if (v.flag[F_UPDATES] && st != ST_UPDATE)
                continue;
        }
        out.push_back(i);
    }
    int kind = sorts_for(c)[v.sort];
    if (kind != S_CATALOG) {
        auto cmp = [&](int a, int b) {
            const Entry& x = items[a];
            const Entry& y = items[b];
            switch (kind) {
            case S_NAME:
                return x.sort_name < y.sort_name;
            case S_NEW:
                if (x.date != y.date)
                    return x.date > y.date;
                return a > b;
            case S_BIG:
                return x.size > y.size;
            case S_SMALL:
                if ((x.size == 0) != (y.size == 0))
                    return y.size == 0;
                return x.size < y.size;
            case S_REGION: {
                int rx = region_rank(x.region), ry = region_rank(y.region);
                if (rx != ry)
                    return rx < ry;
                return x.sort_name < y.sort_name;
            }
            case S_AUTHOR:
                if (x.author != y.author)
                    return lower(x.author) < lower(y.author);
                return x.sort_name < y.sort_name;
            case S_GAME:
                if (x.id != y.id)
                    return x.id < y.id;
                return x.sort_name < y.sort_name;
            case S_SECTION:
                if (x.tai_section != y.tai_section)
                    return x.tai_section < y.tai_section;
                return x.sort_name < y.sort_name;
            }
            return false;
        };
        std::stable_sort(out.begin(), out.end(), cmp);
    }
    if (sel[c] >= (int)out.size())
        sel[c] = std::max(0, (int)out.size() - 1);
}

bool App::fs_app(const std::string& id)
{
    if (apps_scanned_at == 0 || now_ms() - apps_scanned_at > 5000) {
        apps.clear();
        for (auto& a : fs::list("ux0:app"))
            apps.insert(a);
        apps_scanned_at = now_ms();
    }
    return apps.count(id) > 0;
}

void App::rebuild()
{
    query_lower = lower(trim(query));
    for (int c = 0; c < CAT_COUNT; c++)
        rebuild_cat(c);
}

std::string App::filter_summary()
{
    if (tab >= CAT_COUNT)
        return "";
    const View& v = views[tab];
    std::string s;
    auto add = [&](const std::string& x) { s += (s.empty() ? "" : " · ") + x; };
    if (v.sort)
        add(SORT_NAMES[sorts_for(tab)[v.sort]]);
    for (int f : filters_for(tab)) {
        if (f == F_REGION) {
            if (v.region)
                add(REGION_NAMES[v.region]);
        } else if (v.flag[f]) {
            add(FILTER_NAMES[f]);
        }
    }
    return s;
}

void App::input_filters(uint32_t act)
{
    if (tab >= CAT_COUNT) {
        filters_open = false;
        return;
    }
    View& v = views[tab];
    const auto& fl = filters_for(tab);
    int rows = 1 + (int)fl.size();
    if (filter_sel >= rows)
        filter_sel = 0;
    if (act & SCE_CTRL_UP)
        filter_sel = (filter_sel + rows - 1) % rows;
    if (act & SCE_CTRL_DOWN)
        filter_sel = (filter_sel + 1) % rows;
    int d = (act & SCE_CTRL_LEFT) ? -1 : (act & (SCE_CTRL_RIGHT | ok_btn)) ? 1 : 0;
    bool changed = false;
    if (d) {
        if (filter_sel == 0) {
            int n = (int)sorts_for(tab).size();
            v.sort = (v.sort + n + d) % n;
        } else {
            int f = fl[filter_sel - 1];
            if (f == F_REGION) {
                v.region = (v.region + 5 + d) % 5;
            } else {
                v.flag[f] = !v.flag[f];
                if (f == F_HIDE_DLC && v.flag[f])
                    v.flag[F_ONLY_DLC] = false;
                if (f == F_ONLY_DLC && v.flag[f])
                    v.flag[F_HIDE_DLC] = false;
                if (f == F_DATA && v.flag[f])
                    v.flag[F_OWN_FILES] = false;
                if (f == F_OWN_FILES && v.flag[f])
                    v.flag[F_DATA] = false;
            }
        }
        changed = true;
    }
    if (pressed & SCE_CTRL_TRIANGLE) {
        v = View();
        changed = true;
    }
    if (changed) {
        rebuild_cat(tab);
        sel[tab] = 0;
        scroll_target[tab] = 0;
        save_views();
    }
    if (pressed & (back_btn | SCE_CTRL_SELECT))
        filters_open = false;
}

void App::draw_filters()
{
    const View& v = views[tab];
    const auto& fl = filters_for(tab);
    int rows = 1 + (int)fl.size();
    gfx::rect(0, 0, W, H, gfx::alpha(RGBA8(0, 0, 0, 255), 0.6f));
    float w = 540, h = 96 + rows * 46, x = (W - w) / 2, y = (H - h) / 2 - 10;
    gfx::rrect(x, y, w, h, 22, col::SURFACE);
    gfx::rframe(x, y, w, h, 22, 1, col::LINE);
    gfx::text(gfx::bold, x + 28, y + 42, 20, col::TEXT, "Сортировка и фильтры");
    gfx::text_right(gfx::regular, x + w - 28, y + 41, 14, col::ACCENT, category_title((Category)tab));
    for (int i = 0; i < rows; i++) {
        float ry = y + 64 + i * 46;
        bool on = i == filter_sel;
        std::string title, value;
        bool active = false;
        if (i == 0) {
            title = "Сортировка";
            value = SORT_NAMES[sorts_for(tab)[v.sort]];
            active = v.sort != 0;
        } else {
            int f = fl[i - 1];
            title = FILTER_NAMES[f];
            if (f == F_REGION) {
                value = REGION_NAMES[v.region];
                active = v.region != 0;
            } else {
                value = v.flag[f] ? "да" : "нет";
                active = v.flag[f];
            }
        }
        gfx::rrect(x + 16, ry, w - 32, 40, 12, on ? col::SURFACE2 : gfx::alpha(col::SURFACE2, 0.35f));
        if (on)
            gfx::rframe(x + 16, ry, w - 32, 40, 12, 2, col::ACCENT);
        if (active)
            gfx::circle(x + 32, ry + 20, 4, col::ACCENT);
        gfx::text(gfx::regular, x + 44, ry + 26, 15, col::TEXT, title);
        gfx::text_right(gfx::bold, x + w - 32, ry + 26, 15, on || active ? col::ACCENT : col::MUTED, "‹ " + value + " ›");
        int idx = i;
        hits.push_back({x + 16, ry, w - 32, 40, [this, idx]() {
                            filter_sel = idx;
                            input_filters(SCE_CTRL_RIGHT);
                        }});
    }
}

bool App::ignored(const Entry& e, const std::string& version)
{
    if (!ignored_loaded) {
        ignored_loaded = true;
        std::string s;
        fs::read(APP_DIR + "/ignored.txt", s);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t n = s.find('\n', pos);
            if (n == std::string::npos)
                n = s.size();
            if (n > pos)
                ignored_set.insert(s.substr(pos, n - pos));
            pos = n + 1;
        }
    }
    return ignored_set.count(std::string(category_key(e.cat)) + "/" + e.id + "/" + version) > 0;
}

void App::ignore(const Entry& e, const std::string& version)
{
    std::string k = std::string(category_key(e.cat)) + "/" + e.id + "/" + version;
    ignored(e, version);
    ignored_set.insert(k);
    std::string s;
    for (auto& x : ignored_set)
        s += x + "\n";
    fs::write(APP_DIR + "/ignored.txt", s);
}

std::vector<Entry> App::collect_alt()
{
    std::vector<Entry> out;
    for (int c : {CAT_PORTS, CAT_HOMEBREW}) {
        for (auto& e : catalog.items[c]) {
            if (e.alt_url.empty() || e.alt_version.empty() || installed.status(e) == ST_NONE)
                continue;
            std::string cur = installed.version_of(e);
            if (cur.empty() || cur == "-")
                cur = e.version;
            if (version_cmp(cur, e.alt_version) < 0 && version_cmp(e.version, e.alt_version) < 0 && !ignored(e, e.alt_version))
                out.push_back(e);
        }
    }
    return out;
}

void App::refresh_updates()
{
    upd.clear();
    upd_alt.clear();
    for (auto& e : collect_updates()) {
        upd.push_back(e);
        upd_alt.push_back(false);
    }
    for (auto& e : collect_alt()) {
        upd.push_back(e);
        upd_alt.push_back(true);
    }
    updates_count = 0;
    for (size_t i = 0; i < upd.size(); i++)
        if (!upd_alt[i])
            updates_count++;
    if (upd_sel >= (int)upd.size())
        upd_sel = std::max(0, (int)upd.size() - 1);
}

void App::ask_alt(const Entry& e)
{
    modal = Modal();
    modal.on = true;
    modal.title = "Версия " + e.alt_version + " из стороннего источника";
    modal.lines.push_back("Источник: " + (e.alt_source.empty() ? std::string("GitHub автора") : e.alt_source));
    modal.lines.push_back("Основная версия в каталоге: " + e.version);
    modal.lines.push_back("Может слететь русификатор или игра перестанет запускаться.");
    modal.lines.push_back("Ставишь на свой риск. Вернуть основную: □ Переустановить.");
    modal.yes = "Поставить на свой риск";
    Entry alt = e;
    alt.url = e.alt_url;
    alt.mirror = "";
    alt.version = e.alt_version;
    alt.name = e.name + " " + e.alt_version;
    std::vector<Entry> deps = deps_of(e);
    modal.action = [this, alt, deps]() {
        tasks.add(alt, deps);
        show_toast("Добавлено в загрузки");
    };
}

void App::input_updates(uint32_t act)
{
    int n = (int)upd.size();
    if (act & SCE_CTRL_UP)
        upd_sel = std::max(0, upd_sel - 1);
    if (act & SCE_CTRL_DOWN)
        upd_sel = std::min(std::max(0, n - 1), upd_sel + 1);
    if ((pressed & ok_btn) && upd_sel < n) {
        if (upd_alt[upd_sel])
            ask_alt(upd[upd_sel]);
        else
            ask_install(upd[upd_sel], false);
    }
    if ((pressed & SCE_CTRL_SQUARE) && updates_count) {
        int k = 0;
        for (int i = 0; i < n; i++)
            if (!upd_alt[i]) {
                tasks.add(upd[i], deps_of(upd[i]));
                k++;
            }
        show_toast("В очередь добавлено обновлений: " + std::to_string(k));
    }
    if ((pressed & SCE_CTRL_TRIANGLE) && upd_sel < n) {
        const Entry& e = upd[upd_sel];
        ignore(e, upd_alt[upd_sel] ? e.alt_version : e.version);
        show_toast("Это обновление больше не будет показываться");
        refresh_updates();
    }
}

void App::draw_updates()
{
    if (upd.empty()) {
        gfx::mascot(W / 2 - 70, 112, 140, 0.95f);
        gfx::text_center(gfx::bold, W / 2, 300, 20, col::TEXT, "Всё обновлено");
        gfx::text_center(gfx::regular, W / 2, 328, 15, col::MUTED, "Когда выйдут новые версии, они появятся здесь");
        return;
    }
    float y = GRID_TOP + 4;
    gfx::text(gfx::regular, 32, y + 12, 14, col::MUTED,
              "Обновлений: " + std::to_string(updates_count) + (upd.size() > (size_t)updates_count ? " · сторонних версий: " + std::to_string(upd.size() - updates_count) : ""));
    y += 28;
    int first = std::max(0, upd_sel - 4);
    for (int i = first; i < (int)upd.size() && y < FOOTER_Y - 70; i++) {
        const Entry& e = upd[i];
        bool on = i == upd_sel;
        bool alt = upd_alt[i];
        gfx::rrect(28, y, W - 56, 72, 14, on ? col::SURFACE2 : col::SURFACE);
        if (on)
            gfx::rframe(28, y, W - 56, 72, 14, 2, alt ? col::ORANGE : col::ACCENT);
        float a = 0;
        vita2d_texture* ic = images.get(icon_url(e), &a, IMG_THUMB);
        if (ic)
            gfx::fit_round(ic, 42, y + 12, 48, 48, 10, a);
        else
            gfx::placeholder_round(42, y + 12, 48, 48, 10, e.name, 16);
        gfx::text(gfx::bold, 106, y + 30, 16, col::TEXT, gfx::ellipsize(gfx::bold, 16, e.name, 560));
        std::string from = e.cat == CAT_GAMES ? catalog.game_versions[e.id] : installed.version_of(e);
        if (from.empty() || from == "-")
            from = "?";
        std::string to = alt ? e.alt_version : e.version;
        std::string sub = std::string(category_title(e.cat)) + " · " + from + " → " + to + (e.size ? " · " + fmt_size(e.size) : "");
        gfx::text(gfx::regular, 106, y + 52, 13, col::MUTED, gfx::ellipsize(gfx::regular, 13, sub, 560));
        if (alt)
            draw_badge(W - 46, y + 26, "Сторонний источник, на свой риск", col::ORANGE, col::BG);
        else
            draw_badge(W - 46, y + 26, "Обновление", col::ACCENT, col::TEXT);
        int idx = i;
        hits.push_back({28, y, (float)(W - 56), 72, [this, idx]() {
                            if (idx >= (int)upd.size())
                                return;
                            if (upd_sel == idx) {
                                if (upd_alt[idx])
                                    ask_alt(upd[idx]);
                                else
                                    ask_install(upd[idx], false);
                            }
                            upd_sel = idx;
                        }});
        y += 82;
    }
}

std::string App::icon_url(const Entry& e)
{
    if (!e.icon.empty())
        return catalog.media_url(e.icon);
    if (e.cat == CAT_GAMES && !e.dlc && installed.status(e) != ST_NONE)
        return "ur0:appmeta/" + e.id + "/icon0.png";
    return "";
}

std::vector<Entry> App::deps_of(const Entry& e)
{
    std::vector<std::string> ids;
    std::string cur;
    for (char c : e.needs + ",") {
        if (c == ',' || c == ' ' || c == ';') {
            if (!cur.empty())
                ids.push_back(lower(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (e.cat == CAT_TRANSLATIONS && (e.install.empty() || lower(e.install).find("repatch") != std::string::npos))
        if (std::find(ids.begin(), ids.end(), "repatch") == ids.end())
            ids.insert(ids.begin(), "repatch");
    std::vector<Entry> out;
    for (auto& id : ids)
        for (auto& p : catalog.items[CAT_PLUGINS])
            if (lower(p.id) == id && !(e.cat == CAT_PLUGINS && p.id == e.id))
                out.push_back(p);
    return out;
}

void App::check_deps()
{
    detail_dep_ok.clear();
    for (auto& d : detail_deps)
        detail_dep_ok.push_back(plugin_present(d));
}

Status App::status_of(const Entry& e)
{
    if (catalog.patch_for(e))
        return ST_UPDATE;
    return installed.status(e);
}

std::vector<Entry> App::collect_updates()
{
    std::vector<Entry> out;
    for (int c = 0; c < CAT_COUNT; c++) {
        if (c == CAT_GAMES)
            continue;
        for (auto& e : catalog.items[c])
            if (installed.status(e) == ST_UPDATE && !ignored(e, e.version))
                out.push_back(e);
    }
    for (auto& kv : catalog.game_versions) {
        auto p = catalog.patches.find(kv.first);
        if (p != catalog.patches.end() && version_cmp(kv.second, p->second.version) < 0 && !ignored(p->second, p->second.version))
            out.push_back(p->second);
    }
    return out;
}

void App::notify_updates()
{
    auto ups = collect_updates();
    refresh_updates();
    if (ups.empty())
        return;
    std::string seen;
    fs::read(APP_DIR + "/notified.txt", seen);
    std::string add;
    std::vector<std::string> names;
    for (auto& u : ups) {
        std::string key = std::string(category_key(u.cat)) + "/" + u.id + "/" + u.version + "\n";
        if (seen.find(key) != std::string::npos)
            continue;
        add += key;
        names.push_back(u.name + " " + u.version);
    }
    if (!add.empty())
        fs::write(APP_DIR + "/notified.txt", seen + add);
    if (names.size() == 1)
        daemon_notify("Вышло обновление: " + names[0]);
    else if (names.size() > 1)
        daemon_notify("Доступно обновлений: " + std::to_string(names.size()));
    if (names.empty() || modal.on) {
        show_toast("Доступно обновлений: " + std::to_string(updates_count) + ", смотри вкладку Загрузки");
        return;
    }
    modal = Modal();
    modal.on = true;
    modal.title = "Новые обновления: " + std::to_string(names.size());
    for (size_t i = 0; i < names.size() && i < 5; i++)
        modal.lines.push_back(names[i]);
    if (names.size() > 5)
        modal.lines.push_back("и ещё " + std::to_string(names.size() - 5));
    modal.yes = "Обновить всё";
    modal.action = [this]() {
        auto list = collect_updates();
        for (auto& u : list)
            tasks.add(u, deps_of(u));
        show_toast("В очередь добавлено обновлений: " + std::to_string(list.size()));
    };
}

const Entry* App::selected()
{
    if (tab >= CAT_COUNT || filtered[tab].empty())
        return nullptr;
    if (sel[tab] < 0 || sel[tab] >= (int)filtered[tab].size())
        sel[tab] = 0;
    return &catalog.items[tab][filtered[tab][sel[tab]]];
}

int App::refresh_thread(unsigned int, void* arg)
{
    App* a = *(App**)arg;
    std::string body, err;
    std::string base = a->settings.base();
    std::unique_ptr<Catalog> c(new Catalog());
    bool ok = net::get(base + "index.json", body, err) && c->parse(body, err);
    if (ok)
        fs::write(APP_DIR + "/index.json", body);
    else
        c->load_cache();
    if (c->media.empty())
        c->media = base + "media/";
    pkgj_fetch(a->manual_refresh);
    std::string covers;
    if ((a->manual_refresh || !fs::exists(APP_DIR + "/covers.json") || ok) && net::get(base + "covers.json", covers, err) && covers.size() > 2 && covers[0] == '{')
        fs::write(APP_DIR + "/covers.json", covers);
    c->load_pkgj();
    c->apply_covers();
    c->load_updates();
    c->prepare();
    if (c->app_url.empty()) {
        std::string rel, rerr;
        if (net::get("https://api.github.com/repos/" APP_REPO "/releases/latest", rel, rerr)) {
            Json::CharReaderBuilder rb;
            Json::Value root;
            std::string errs;
            std::istringstream in(rel);
            if (Json::parseFromStream(rb, in, &root, &errs) && root.isObject()) {
                std::string tag = root["tag_name"].isString() ? root["tag_name"].asString() : "";
                while (!tag.empty() && !isdigit((unsigned char)tag[0]))
                    tag.erase(0, 1);
                for (const auto& a : root["assets"]) {
                    std::string u = a["browser_download_url"].isString() ? a["browser_download_url"].asString() : "";
                    if (ends_with(lower(u), ".vpk")) {
                        c->app_url = u;
                        break;
                    }
                }
                if (!c->app_url.empty()) {
                    c->app_version = tag;
                    c->app_notes = root["body"].isString() ? root["body"].asString() : "";
                }
            }
        }
    }
    {
        std::lock_guard<Mutex> lock(a->refresh_m);
        a->fresh = std::move(c);
        a->refresh_err = ok ? "" : (err.empty() ? "Ошибка сети" : err);
    }
    a->refreshing = 2;
    return sceKernelExitDeleteThread(0);
}

void App::start_refresh(bool manual)
{
    if (refreshing == 1)
        return;
    manual_refresh = manual;
    refreshing = 1;
    SceUID th = sceKernelCreateThread("vpkgj_refresh", refresh_thread, 180, 512 * 1024, 0, 0, nullptr);
    App* self = this;
    sceKernelStartThread(th, sizeof(self), &self);
}

void App::apply_refresh()
{
    if (refreshing != 2)
        return;
    refreshing = 0;
    std::lock_guard<Mutex> lock(refresh_m);
    if (fresh) {
        bool changed = fresh->version != catalog.version;
        catalog = std::move(*fresh);
        fresh.reset();
        loaded_once = loaded_once || refresh_err.empty();
        rebuild();
        notify_updates();
        if (!pre_checked) {
            pre_checked = true;
            std::string mark;
            fs::read(APP_DIR + "/preload.txt", mark);
            if (loaded_once && trim(mark) != catalog.media + "|" + catalog.version)
                start_preload();
            else
                screen = SCR_BROWSE;
        }
        if (!app_prompted)
            ask_app_update(false);
        if (!refresh_err.empty()) {
            if (manual_refresh || !loaded_once)
                show_toast("Не удалось обновить каталог: " + refresh_err);
        } else if (manual_refresh || (changed && !catalog.version.empty())) {
            show_toast(changed ? "Каталог обновлён" : "Каталог актуален");
        }
    }
}

void App::set_tab(int t)
{
    tab = (t + TAB_COUNT) % TAB_COUNT;
    if (tab == TAB_UPDATES)
        refresh_updates();
    focus_init = false;
}

void App::ensure_visible()
{
    if (tab >= CAT_COUNT)
        return;
    int row = sel[tab] / COLS;
    float top = row * (CARD_H + GAP_Y);
    float view = FOOTER_Y - GRID_TOP - 10;
    if (top < scroll_target[tab])
        scroll_target[tab] = top;
    if (top + CARD_H > scroll_target[tab] + view)
        scroll_target[tab] = top + CARD_H - view;
    if (scroll_target[tab] < 0)
        scroll_target[tab] = 0;
}

void App::move_sel(int dx, int dy)
{
    int n = (int)filtered[tab].size();
    if (!n)
        return;
    int s = sel[tab];
    if (dx) {
        int col = s % COLS + dx;
        if (col >= 0 && col < COLS && s + dx < n)
            s += dx;
    }
    if (dy) {
        int t = s + dy * COLS;
        if (t < 0)
            t = s;
        if (t >= n)
            t = (s / COLS + 1) * COLS < n ? n - 1 : s;
        s = t;
    }
    sel[tab] = s;
    ensure_visible();
}

void App::open_detail(const Entry& e)
{
    detail = e;
    rel_sel = -1;
    viewer = false;
    has_saves = !e.id.empty() && fs::exists(savedata_path(e.id));
    related.clear();
    related_kind.clear();
    auto add = [&](const Entry& x, const std::string& kind) {
        if (related.size() < 60) {
            related.push_back(x);
            related_kind.push_back(kind);
        }
    };
    if (e.cat == CAT_GAMES && !e.dlc) {
        for (auto& t : catalog.items[CAT_TRANSLATIONS])
            if (t.id == e.id)
                add(t, "Русификатор");
        for (auto& p : catalog.items[CAT_PORTS])
            if (p.id == e.id)
                add(p, "Порт");
        for (auto& d : catalog.items[CAT_GAMES])
            if (d.dlc && d.id == e.id)
                add(d, "DLC");
    } else if (e.cat == CAT_TRANSLATIONS || (e.cat == CAT_GAMES && e.dlc)) {
        for (auto& g : catalog.items[CAT_GAMES])
            if (!g.dlc && g.id == e.id)
                add(g, g.region.empty() ? "Игра" : "Игра " + g.region);
        if (e.cat == CAT_TRANSLATIONS)
            for (auto& d : catalog.items[CAT_GAMES])
                if (d.dlc && d.id == e.id)
                    add(d, "DLC");
    }
    shot = 0;
    prev_shot = -1;
    shot_time = now_ms();
    shot_auto = now_ms() + 6000;
    desc_scroll = desc_target = 0;
    std::string d = e.description;
    if (e.cat == CAT_GAMES) {
        std::string info;
        if (!e.region.empty())
            info += "Регион: " + e.region + "\n";
        if (!e.content.empty())
            info += "Content ID: " + e.content + "\n";
        if (e.size)
            info += "Размер: " + fmt_size(e.size) + "\n";
        if (starts_with(e.version, "FW "))
            info += "Нужна прошивка " + e.version.substr(3) + " или новее\n";
        if (!e.dlc) {
            int dlcs = 0;
            for (auto& x : catalog.items[CAT_GAMES])
                if (x.dlc && x.id == e.id)
                    dlcs++;
            if (dlcs)
                info += "DLC в списке: " + std::to_string(dlcs) + ", найдёшь поиском по " + e.id + "\n";
        }
        d = d.empty() ? info : d + "\n\n" + info;
    }
    if (trim(d).empty())
        d = "Описание пока не добавлено.";
    desc_lines = gfx::wrap(gfx::regular, 15, d, W - 64);
    detail_deps = deps_of(e);
    check_deps();
    screen = SCR_DETAIL;
}

void App::background_download(const Entry& e)
{
    std::vector<uint8_t> rif;
    std::string err;
    if (!e.zrif.empty() && !zrif_decode(e.zrif, rif, err)) {
        show_toast(err);
        return;
    }
    std::string url = e.url.substr(0, e.url.find_first_of("|;"));
    if (bgdl_start(e.dlc ? BGDL_DLC : BGDL_GAME, e.name, url, rif, err))
        show_toast("Качается системой в фоне, установка из уведомлений");
    else
        show_toast("Фоновая загрузка: " + err);
}

void App::launch(const Entry& e)
{
    std::string uri = "psgm:play?titleid=" + e.id;
    sceAppMgrLaunchAppByUri(0xFFFFF, uri.c_str());
}

void App::ask_install(const Entry& e, bool reinstall)
{
    modal = Modal();
    modal.on = true;
    modal.title = (reinstall ? "Переустановить " : "Установить ") + e.name + "?";
    uint64_t free_ux0 = fs::free_space("ux0:");
    if (e.size)
        modal.lines.push_back("Размер: " + fmt_size(e.size) + ", свободно на ux0: " + fmt_size(free_ux0));
    else
        modal.lines.push_back("Свободно на ux0: " + fmt_size(free_ux0));
    switch (e.cat) {
    case CAT_PORTS:
    case CAT_HOMEBREW:
        if (e.data.empty())
            modal.lines.push_back("Будет установлен vpk, data не требуется");
        else
            modal.lines.push_back("Будет установлен vpk и data в " + (e.data_path.empty() ? std::string("ux0:data") : e.data_path));
        break;
    case CAT_GAMES:
        modal.lines.push_back("PKG будет скачан и установлен, нужен NoNpDrm");
        break;
    case CAT_TRANSLATIONS:
        if (!fs::exists("ux0:app/" + e.id))
            modal.lines.push_back("Игра " + e.id + " не найдена на консоли");
        modal.lines.push_back("Файлы перевода лягут в ux0:rePatch/" + e.id);
        if (lower(e.install).find("plugin") != std::string::npos)
            modal.lines.push_back("Плагин пропишется в " + tai_dir() + "/config.txt");
        break;
    case CAT_PLUGINS:
        modal.lines.push_back("Плагин пропишется в " + tai_dir() + "/config.txt, раздел " +
                              (e.tai_section.empty() ? std::string("*ALL") : e.tai_section));
        break;
    default:
        break;
    }
    std::vector<Entry> deps = deps_of(e);
    std::string need;
    std::string manual;
    for (auto& d : deps) {
        if (plugin_present(d))
            continue;
        if (d.url.empty())
            manual += (manual.empty() ? "" : ", ") + d.name;
        else
            need += (need.empty() ? "" : ", ") + d.name;
    }
    if (!need.empty())
        modal.lines.push_back("Сначала поставится: " + need);
    if (!manual.empty())
        modal.lines.push_back("Нужно поставить вручную: " + manual);
    modal.yes = reinstall ? "Переустановить" : "Установить";
    Entry copy = e;
    modal.action = [this, copy, deps]() {
        tasks.add(copy, deps);
        show_toast("Добавлено в загрузки");
    };
    if (e.cat == CAT_GAMES && !e.dlc) {
        std::vector<Entry> trs;
        for (auto& t : catalog.items[CAT_TRANSLATIONS])
            if (t.id == e.id)
                trs.push_back(t);
        if (!trs.empty()) {
            modal.lines.push_back("Есть русификатор: " + trs[0].name + (trs[0].lang.empty() ? "" : " (" + trs[0].lang + ")"));
            modal.yes = "Игра + русификатор";
            modal.alt = "Только игра";
            modal.alt_action = modal.action;
            modal.action = [this, copy, deps, trs]() {
                tasks.add(copy, deps);
                for (auto& t : trs)
                    tasks.add(t, deps_of(t));
                show_toast("Игра и русификатор добавлены в загрузки");
            };
        }
    }
}

void App::primary_action()
{
    const Entry& e = detail;
    auto t = tasks.find(e);
    if (t && (t->state == TS_RUNNING || t->state == TS_QUEUED)) {
        modal = Modal();
        modal.on = true;
        modal.title = "Отменить загрузку?";
        modal.lines.push_back(e.name);
        modal.yes = "Отменить";
        modal.action = [t]() { t->progress.cancel = true; };
        return;
    }
    if (e.cat == CAT_PLUGINS && e.url.empty()) {
        show_toast(plugin_present(e) ? e.name + " уже на месте" : e.description);
        return;
    }
    Status st = status_of(e);
    if (st == ST_INSTALLED && launchable(e)) {
        launch(e);
        return;
    }
    if (const Entry* p = catalog.patch_for(e)) {
        ask_install(*p, false);
        return;
    }
    ask_install(e, st == ST_INSTALLED);
}

void App::ime_done(const std::string& text)
{
    switch (ime_purpose) {
    case IME_SEARCH:
        query = trim(text);
        rebuild();
        if (tab < CAT_COUNT) {
            sel[tab] = 0;
            scroll_target[tab] = 0;
        }
        if (!query.empty() && (tab >= CAT_COUNT || filtered[tab].empty())) {
            for (int c = 0; c < CAT_COUNT; c++) {
                if (!filtered[c].empty()) {
                    set_tab(c);
                    sel[c] = 0;
                    scroll_target[c] = 0;
                    show_toast(std::string("Нашлось во вкладке ") + category_title((Category)c));
                    break;
                }
            }
        }
        break;
    case IME_REPO:
        if (!trim(text).empty()) {
            settings.repo = trim(text);
            settings.save();
            start_refresh(true);
        }
        break;
    case IME_BRANCH:
        if (!trim(text).empty()) {
            settings.branch = trim(text);
            settings.save();
            start_refresh(true);
        }
        break;
    default:
        break;
    }
    ime_purpose = IME_NONE;
}

void App::touch()
{
    SceTouchData t;
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
    if (t.reportNum > 0) {
        float x = t.report[0].x / 2.0f, y = t.report[0].y / 2.0f;
        if (!touching) {
            touching = true;
            dragged = false;
            tx0 = tx = x;
            ty0 = ty = y;
            return;
        }
        float dy = y - ty;
        if (fabsf(x - tx0) > 14 || fabsf(y - ty0) > 14)
            dragged = true;
        if (dragged && !modal.on) {
            if (screen == SCR_BROWSE && tab < CAT_COUNT) {
                scroll_target[tab] -= dy;
                float rows = (filtered[tab].size() + COLS - 1) / COLS;
                float max = std::max(0.0f, rows * (CARD_H + GAP_Y) - (FOOTER_Y - GRID_TOP - 10));
                scroll_target[tab] = std::max(0.0f, std::min(max, scroll_target[tab]));
                scroll[tab] = scroll_target[tab];
            } else if (screen == SCR_DETAIL && ty0 > 300) {
                desc_target -= dy;
                desc_scroll = desc_target;
            }
        }
        tx = x;
        ty = y;
        return;
    }
    if (!touching)
        return;
    touching = false;
    if (!dragged) {
        tap(tx, ty);
        return;
    }
    if (screen == SCR_DETAIL && ty0 < 300 && !detail.shots.empty()) {
        float dx = tx - tx0;
        int n = (int)detail.shots.size();
        if (fabsf(dx) > 60) {
            prev_shot = shot;
            shot = (shot + (dx < 0 ? 1 : n - 1)) % n;
            shot_time = now_ms();
            shot_auto = now_ms() + 8000;
        }
    }
}

void App::tap(float x, float y)
{
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
            auto fn = it->fn;
            fn();
            return;
        }
    }
}

void App::input()
{
    uint32_t b = read_buttons();
    pressed = b & ~prev_buttons;
    uint32_t dirs = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
    repeat = pressed & dirs;
    if (b & dirs) {
        if (pressed & dirs)
            repeat_at = now_ms() + 320;
        else if (now_ms() >= repeat_at) {
            repeat = b & dirs;
            repeat_at = now_ms() + 70;
        }
    }
    prev_buttons = b;
    uint32_t act = pressed | repeat;

    touch();

    if (modal.on) {
        if (pressed & ok_btn) {
            modal.on = false;
            if (modal.action) {
                auto fn = modal.action;
                fn();
                }
        } else if ((pressed & SCE_CTRL_TRIANGLE) && modal.alt_action) {
            modal.on = false;
            auto fn = modal.alt_action;
            fn();
        } else if (pressed & back_btn) {
            modal.on = false;
        }
        return;
    }

    if (screen == SCR_PRELOAD) {
        input_preload();
        return;
    }

    if (screen == SCR_BROWSE) {
        if (pressed & SCE_CTRL_LTRIGGER)
            set_tab(tab - 1);
        if (pressed & SCE_CTRL_RTRIGGER)
            set_tab(tab + 1);
        if (filters_open) {
            input_filters(act);
            return;
        }
        if (pressed & SCE_CTRL_START) {
            screen = SCR_SETTINGS;
            return;
        }
        if ((pressed & SCE_CTRL_SELECT) && tab < CAT_COUNT) {
            filters_open = true;
            return;
        }
        if (tab == TAB_UPDATES) {
            input_updates(act);
            return;
        }
        if (tab == TAB_DOWNLOADS) {
            auto list = tasks.list();
            if ((pressed & SCE_CTRL_SQUARE) && updates_count) {
                auto ups = collect_updates();
                for (auto& u : ups)
                    tasks.add(u, deps_of(u));
                show_toast("В очередь добавлено обновлений: " + std::to_string(ups.size()));
            }
            if (act & SCE_CTRL_UP)
                dl_sel = std::max(0, dl_sel - 1);
            if (act & SCE_CTRL_DOWN)
                dl_sel = std::min((int)list.size() - 1, dl_sel + 1);
            if (dl_sel < 0)
                dl_sel = 0;
            if (pressed & SCE_CTRL_TRIANGLE) {
                tasks.clear_finished();
                dl_sel = 0;
            }
            if ((pressed & SCE_CTRL_SELECT) && bg_left)
                cancel_background();
            if ((pressed & ok_btn) && dl_sel < (int)list.size()) {
                auto t = list[dl_sel];
                if (t->state == TS_RUNNING || t->state == TS_QUEUED) {
                    modal = Modal();
                    modal.on = true;
                    modal.title = "Отменить загрузку?";
                    modal.lines.push_back(t->entry.name);
                    modal.yes = "Отменить";
                    modal.action = [t]() { t->progress.cancel = true; };
                } else if (t->state == TS_FAILED || t->state == TS_CANCELED) {
                    tasks.add(t->entry, t->deps);
                    show_toast("Повторная попытка");
                }
            }
            return;
        }
        if (act & SCE_CTRL_LEFT)
            move_sel(-1, 0);
        if (act & SCE_CTRL_RIGHT)
            move_sel(1, 0);
        if (act & SCE_CTRL_UP)
            move_sel(0, -1);
        if (act & SCE_CTRL_DOWN)
            move_sel(0, 1);
        if (pressed & SCE_CTRL_TRIANGLE) {
            ime_purpose = IME_SEARCH;
            ime::open("Поиск", query, 64);
        }
        if (pressed & SCE_CTRL_SQUARE)
            start_refresh(true);
        if ((pressed & back_btn) && !query.empty()) {
            query.clear();
            rebuild();
        }
        if (pressed & ok_btn) {
            const Entry* e = selected();
            if (e)
                { back_stack.clear(); open_detail(*e); }
        }
        return;
    }

    if (screen == SCR_DETAIL && viewer) {
        int n = (int)detail.shots.size();
        if (n > 1 && (act & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
            prev_shot = shot;
            shot = (shot + ((act & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER)) ? 1 : n - 1)) % n;
            shot_time = now_ms();
        }
        if (pressed & (back_btn | ok_btn | SCE_CTRL_SELECT))
            viewer = false;
        return;
    }

    if (screen == SCR_DETAIL) {
        int n = (int)detail.shots.size();
        int rn = (int)related.size();
        if ((pressed & SCE_CTRL_SELECT) && n) {
            viewer = true;
            shot_auto = now_ms() + 600000;
            return;
        }
        if (rel_sel >= 0) {
            if (act & SCE_CTRL_LEFT)
                rel_sel = std::max(0, rel_sel - 1);
            if (act & SCE_CTRL_RIGHT)
                rel_sel = std::min(rn - 1, rel_sel + 1);
            if (act & SCE_CTRL_UP)
                rel_sel = -1;
            if (act & RS_DOWN)
                desc_target += 44;
            if (act & RS_UP)
                desc_target -= 44;
            if (pressed & back_btn)
                go_back();
            else if ((pressed & ok_btn) && rel_sel >= 0 && rel_sel < rn) {
                back_stack.push_back(detail);
                Entry next = related[rel_sel];
                open_detail(next);
            }
            return;
        }
        if (n > 1 && (act & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {
            prev_shot = shot;
            shot = (shot + ((act & SCE_CTRL_RIGHT) ? 1 : n - 1)) % n;
            shot_time = now_ms();
            shot_auto = now_ms() + 8000;
        }
        if (rn && (act & SCE_CTRL_DOWN))
            rel_sel = 0;
        else if (act & (SCE_CTRL_DOWN | RS_DOWN))
            desc_target += 44;
        if (act & (SCE_CTRL_UP | RS_UP))
            desc_target -= 44;
        if (pressed & back_btn) {
            go_back();
            return;
        }
        if (pressed & ok_btn)
            primary_action();
        if ((pressed & SCE_CTRL_TRIANGLE) && detail.cat == CAT_GAMES)
            background_download(detail);
        else if ((pressed & SCE_CTRL_TRIANGLE) && !(detail.cat == CAT_PLUGINS && detail.url.empty()))
            queue_background(detail);
        if ((pressed & SCE_CTRL_LTRIGGER) && !detail.alt_url.empty() && version_cmp(detail.version, detail.alt_version) < 0)
            ask_alt(detail);
        if ((pressed & SCE_CTRL_RTRIGGER) && has_saves)
            saves_menu(detail);
        if ((pressed & SCE_CTRL_SQUARE) && installed.status(detail) != ST_NONE) {
            auto t = tasks.find(detail);
            if (!t || (t->state != TS_RUNNING && t->state != TS_QUEUED))
                ask_install(detail, true);
        }
        return;
    }

    if (screen == SCR_BACKUPS) {
        int n = (int)backups.size();
        if (act & SCE_CTRL_UP)
            backup_sel = std::max(0, backup_sel - 1);
        if (act & SCE_CTRL_DOWN)
            backup_sel = std::min(std::max(0, n - 1), backup_sel + 1);
        if (pressed & back_btn) {
            screen = SCR_SETTINGS;
            return;
        }
        if (n && (pressed & ok_btn)) {
            Backup b = backups[backup_sel];
            modal = Modal();
            modal.on = true;
            modal.title = "Восстановить копию?";
            modal.lines.push_back(b.label);
            for (auto& it : b.items)
                modal.lines.push_back("Вернётся: " + it.first);
            modal.yes = "Восстановить";
            modal.action = [this, b]() {
                std::string err;
                if (backup_restore(b, err)) {
                    show_toast("Восстановлено");
                    bool tai = false;
                    for (auto& it : b.items)
                        if (it.first.find("tai/config.txt") != std::string::npos)
                            tai = true;
                    if (tai) {
                        modal = Modal();
                        modal.on = true;
                        modal.title = "Нужна перезагрузка";
                        modal.lines.push_back("config.txt восстановлен, изменения вступят в силу после перезагрузки");
                        modal.yes = "Перезагрузить";
                        modal.action = []() { scePowerRequestColdReset(); };
                    }
                } else {
                    show_toast(err);
                }
            };
        }
        if (n && (pressed & SCE_CTRL_TRIANGLE)) {
            Backup b = backups[backup_sel];
            modal = Modal();
            modal.on = true;
            modal.title = "Удалить копию?";
            modal.lines.push_back(b.label);
            modal.yes = "Удалить";
            modal.action = [this, b]() {
                backup_delete(b);
                open_backups();
                show_toast("Копия удалена");
            };
        }
        return;
    }

    if (screen == SCR_SETTINGS) {
        int n = (int)settings_actions.size();
        if (act & SCE_CTRL_UP)
            settings_sel = std::max(0, settings_sel - 1);
        if (act & SCE_CTRL_DOWN)
            settings_sel = std::min(std::max(0, n - 1), settings_sel + 1);
        if (pressed & (back_btn | SCE_CTRL_START | SCE_CTRL_SELECT)) {
            screen = SCR_BROWSE;
            return;
        }
        if (settings_sel == 4 && (act & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {
            speed_mode = (speed_mode + ((act & SCE_CTRL_LEFT) ? 4 : 1)) % 5;
            save_prefs();
            apply_speed();
        }
        if ((pressed & ok_btn) && settings_sel < n)
            settings_actions[settings_sel]();
    }
}

void App::draw_badge(float x, float y, const std::string& s, unsigned bg, unsigned fg)
{
    int w = gfx::width(gfx::bold, 11, s) + 14;
    gfx::rrect(x - w, y, w, 20, 10, bg);
    gfx::text(gfx::bold, x - w + 7, y + 14, 11, fg, s);
}

void App::draw_header()
{
    hits.push_back({0, 0, (float)W, (float)HEADER_H, []() {}});
    gfx::circle(42, 33, 20, gfx::alpha(col::ACCENT, 0.35f));
    gfx::logo(24, 15, 36);
    hits.push_back({20, 12, 44, 44, [this]() { screen = SCR_SETTINGS; }});
    float x = 74;
    gfx::button(BTN_L, x, 23, 20);
    x += 34;
    for (int t = 0; t < TAB_COUNT; t++) {
        std::string label = t == TAB_DOWNLOADS ? "Загрузки" : t == TAB_UPDATES ? "Обновления" : category_title((Category)t);
        if (!query.empty() && t < CAT_COUNT)
            label += " " + std::to_string(filtered[t].size());
        int n = t == TAB_DOWNLOADS ? tasks.active() : t == TAB_UPDATES ? updates_count : 0;
        int tw = gfx::width(gfx::bold, 15, label);
        float w = tw + 28 + (n ? 26 : 0);
        bool on = t == tab;
        if (on)
            gfx::rrect(x, 16, w, 34, 17, col::ACCENT);
        gfx::text(on ? gfx::bold : gfx::regular, x + 14, 38, 15, on ? col::TEXT : col::MUTED, label);
        if (n) {
            std::string c = std::to_string(n);
            gfx::circle(x + w - 22, 33, 9, on ? col::TEXT : col::ACCENT);
            gfx::text_center(gfx::bold, x + w - 22, 37, 11, on ? col::ACCENT : col::TEXT, c);
        }
        int tt = t;
        hits.push_back({x, 10, w, 46, [this, tt]() {
                            set_tab(tt);
                            screen = SCR_BROWSE;
                        }});
        x += w + 4;
    }
    gfx::button(BTN_R, x + 6, 23, 20);
    if (refreshing == 1)
        gfx::spinner(W - 28, 33, 7, col::MUTED);
}

void App::draw_card(const Entry& e, float x, float y)
{
    gfx::rrect(x, y, CARD_W, CARD_H, 14, col::SURFACE);
    float a = 0;
    vita2d_texture* img = nullptr;
    vita2d_texture* icon = images.get(icon_url(e), &a, IMG_THUMB);
    float ia = a;
    if (!e.shots.empty())
        img = images.get(catalog.media_url(e.shots[0]), &a, IMG_THUMB);
    if (img) {
        if (a < 1)
            gfx::placeholder_round(x, y, CARD_W, CARD_IMG_H, 14, e.name, 34);
        gfx::cover_round(img, x, y, CARD_W, CARD_IMG_H, 14, true, false, a);
        if (icon) {
            gfx::rrect(x + 9, y + CARD_IMG_H - 59, 54, 54, 10, gfx::alpha(col::BG, 0.65f * ia));
            gfx::fit_round(icon, x + 12, y + CARD_IMG_H - 56, 48, 48, 8, ia);
        }
    } else if (icon) {
        gfx::placeholder_round(x, y, CARD_W, CARD_IMG_H, 14, e.name, 0);
        gfx::fit_round(icon, x + CARD_W / 2 - 50, y + 6, 100, CARD_IMG_H - 12, 6, ia);
    } else {
        gfx::placeholder_round(x, y, CARD_W, CARD_IMG_H, 14, e.name, 34);
    }
    gfx::rect(x, y + CARD_IMG_H - 1, CARD_W, 1, col::LINE);

    float bx = x + CARD_W - 8;
    auto t = tasks.find(e);
    Status st = status_of(e);
    if (t && t->state == TS_RUNNING) {
        uint64_t total = t->progress.total, done = t->progress.done;
        std::string s = total ? std::to_string((int)(done * 100 / total)) + "%" : "...";
        draw_badge(bx, y + 8, s, col::ACCENT, col::TEXT);
    } else if (t && t->state == TS_QUEUED) {
        draw_badge(bx, y + 8, "В очереди", gfx::alpha(col::BG, 0.8f), col::TEXT);
    } else if (st == ST_UPDATE) {
        draw_badge(bx, y + 8, "Обновление", col::ORANGE, col::BG);
    } else if (st == ST_INSTALLED) {
        draw_badge(bx, y + 8, "Установлено", col::GREEN, col::BG);
    } else if (e.cat == CAT_TRANSLATIONS && fs_app(e.id)) {
        draw_badge(bx, y + 8, "Игра есть", col::ACCENT2, col::BG);
    }
    float lx = x + 8;
    if (!e.lang.empty()) {
        std::string l = e.lang;
        for (auto& c : l)
            c = toupper(c);
        int w = gfx::width(gfx::bold, 11, l) + 14;
        gfx::rrect(lx, y + 8, w, 20, 10, gfx::alpha(col::BG, 0.75f));
        gfx::text(gfx::bold, lx + 7, y + 22, 11, col::TEXT, l);
        lx += w + 4;
    }
    if (e.cat == CAT_PORTS) {
        std::string l = e.data.empty() ? (e.data_path.empty() ? "" : "СВОИ ФАЙЛЫ") : "DATA";
        if (!l.empty()) {
            int w = gfx::width(gfx::bold, 11, l) + 14;
            gfx::rrect(lx, y + 8, w, 20, 10, e.data.empty() ? gfx::alpha(col::BG, 0.75f) : col::GREEN);
            gfx::text(gfx::bold, lx + 7, y + 22, 11, e.data.empty() ? col::ORANGE : col::BG, l);
        }
    }

    gfx::text(gfx::bold, x + 12, y + CARD_IMG_H + 26, 15, col::TEXT, gfx::ellipsize(gfx::bold, 15, e.name, CARD_W - 24));
    std::string sub;
    if (e.cat == CAT_GAMES)
        sub = e.region.empty() ? e.id : e.region + " · " + e.id;
    else
        sub = e.author.empty() ? e.version : e.version.empty() ? e.author : e.author + " · " + e.version;
    std::string size = e.size ? fmt_size(e.size) : "";
    int sw = gfx::width(gfx::regular, 12, size);
    gfx::text(gfx::regular, x + 12, y + CARD_IMG_H + 48, 12, col::MUTED, gfx::ellipsize(gfx::regular, 12, sub, CARD_W - 32 - sw));
    gfx::text_right(gfx::regular, x + CARD_W - 12, y + CARD_IMG_H + 48, 12, col::MUTED, size);

    if (t && t->state == TS_RUNNING && t->progress.total) {
        float f = (float)t->progress.done / t->progress.total;
        gfx::rect(x + 12, y + CARD_H - 8, CARD_W - 24, 3, col::SURFACE2);
        gfx::rect(x + 12, y + CARD_H - 8, (CARD_W - 24) * f, 3, col::ACCENT);
    }
}

void App::draw_grid()
{
    auto& f = filtered[tab];
    if (f.empty()) {
        float cy = 300;
        if (!(refreshing == 1 && !loaded_once))
            gfx::mascot(W / 2 - 70, 112, 140, 0.95f);
        if (refreshing == 1 && !loaded_once) {
            gfx::spinner(W / 2, cy - 20, 12, col::ACCENT);
            gfx::text_center(gfx::regular, W / 2, cy + 22, 16, col::MUTED, "Загружаю каталог...");
        } else if (!filter_summary().empty()) {
            gfx::text_center(gfx::bold, W / 2, cy, 20, col::TEXT, "Под фильтры ничего не подходит");
            gfx::text_center(gfx::regular, W / 2, cy + 28, 15, col::MUTED, filter_summary() + " · SELECT, чтобы изменить");
        } else if (!query.empty()) {
            gfx::text_center(gfx::bold, W / 2, cy, 20, col::TEXT, "Ничего не нашлось");
            gfx::text_center(gfx::regular, W / 2, cy + 28, 15, col::MUTED, "Поиск: " + query);
        } else if (!loaded_once && !refresh_err.empty()) {
            gfx::text_center(gfx::bold, W / 2, cy, 20, col::TEXT, "Не удалось загрузить каталог");
            gfx::text_center(gfx::regular, W / 2, cy + 28, 15, col::MUTED, gfx::ellipsize(gfx::regular, 15, refresh_err, 700));
            gfx::text_center(gfx::regular, W / 2, cy + 52, 15, col::MUTED, settings.repo + " · " + settings.branch);
        } else {
            gfx::text_center(gfx::bold, W / 2, cy, 20, col::TEXT, "Здесь пока пусто");
            gfx::text_center(gfx::regular, W / 2, cy + 28, 15, col::MUTED, "Записи появятся, когда их добавят в каталог");
        }
        return;
    }
    scroll[tab] += (scroll_target[tab] - scroll[tab]) * 0.22f;
    if (fabsf(scroll_target[tab] - scroll[tab]) < 0.5f)
        scroll[tab] = scroll_target[tab];
    float top = GRID_TOP - scroll[tab];
    vita2d_set_clip_rectangle(0, HEADER_H, W, FOOTER_Y);
    vita2d_enable_clipping();
    int s = sel[tab];
    float sx = GRID_X + (s % COLS) * (CARD_W + GAP_X);
    float sy = top + (s / COLS) * (CARD_H + GAP_Y);
    if (!focus_init) {
        fx = sx;
        fy = sy;
        focus_init = true;
    }
    fx += (sx - fx) * 0.3f;
    fy += (sy - fy) * 0.3f;
    gfx::rrect(fx - 4, fy - 4, CARD_W + 8, CARD_H + 8, 17, col::ACCENT);
    gfx::rrect(fx - 1, fy - 1, CARD_W + 2, CARD_H + 2, 15, col::BG);
    float row_h = CARD_H + GAP_Y;
    int first_row = std::max(0, (int)((HEADER_H - 20 - top - CARD_H) / row_h));
    int last_row = (int)((FOOTER_Y + 10 - top) / row_h);
    bool down = scroll_target[tab] >= scroll[tab];
    int pre_from = std::max(0, down ? last_row + 1 : first_row - 3);
    int pre_to = down ? last_row + 3 : first_row - 1;
    for (int r = pre_from; r <= pre_to; r++)
        for (int k = r * COLS; k < (r + 1) * COLS && k < (int)f.size(); k++) {
            const Entry& e = catalog.items[tab][f[k]];
            if (!e.shots.empty())
                images.prefetch(catalog.media_url(e.shots[0]), IMG_THUMB);
            images.prefetch(icon_url(e), IMG_THUMB);
        }
    for (int i = first_row * COLS; i < (int)f.size(); i++) {
        float x = GRID_X + (i % COLS) * (CARD_W + GAP_X);
        float y = top + (i / COLS) * (CARD_H + GAP_Y);
        if (y > FOOTER_Y + 10)
            break;
        if (y + CARD_H < HEADER_H - 20)
            continue;
        const Entry& e = catalog.items[tab][f[i]];
        draw_card(e, x, y);
        int idx = i;
        hits.push_back({x, y, (float)CARD_W, (float)CARD_H, [this, idx]() {
                            if (sel[tab] == idx) {
                                const Entry* e = selected();
                                if (e)
                                    { back_stack.clear(); open_detail(*e); }
                            } else {
                                sel[tab] = idx;
                                ensure_visible();
                            }
                        }});
    }
    vita2d_disable_clipping();
    std::string fs_ = filter_summary();
    if (!fs_.empty()) {
        int w = gfx::width(gfx::regular, 13, fs_) + 24;
        gfx::rrect(24, FOOTER_Y - 34, w, 26, 13, gfx::alpha(col::ACCENT, 0.9f));
        gfx::text(gfx::regular, 36, FOOTER_Y - 16, 13, col::TEXT, fs_);
    }
    if (!query.empty()) {
        std::string q = "Поиск: " + query;
        int w = gfx::width(gfx::regular, 13, q) + 24;
        gfx::rrect(W - 24 - w, FOOTER_Y - 34, w, 26, 13, gfx::alpha(col::SURFACE2, 0.95f));
        gfx::text(gfx::regular, W - 24 - w + 12, FOOTER_Y - 16, 13, col::TEXT, q);
    }
}

void App::draw_detail()
{
    const Entry& e = detail;
    const float HERO = related.empty() ? 300 : 250;
    int n = (int)e.shots.size();
    if (n > 1 && now_ms() > shot_auto && !touching) {
        prev_shot = shot;
        shot = (shot + 1) % n;
        shot_time = now_ms();
        shot_auto = now_ms() + 6000;
    }
    float fade = std::min(1.0f, (now_ms() - shot_time) / 350.0f);
    float ia = 0;
    vita2d_texture* icon = images.get(icon_url(e), &ia, IMG_THUMB);
    if (n) {
        std::string cur_url = catalog.media_url(e.shots[shot]);
        if (prev_shot >= 0 && fade < 1) {
            std::string pu = catalog.media_url(e.shots[prev_shot]);
            vita2d_texture* p = images.peek(pu, IMG_FULL);
            if (!p)
                p = images.peek(pu, IMG_THUMB);
            if (p)
                gfx::cover(p, 0, 0, W, HERO, 1);
        }
        float a = 0;
        vita2d_texture* cur = images.get(cur_url, &a, IMG_FULL);
        float sa = std::min(a, prev_shot >= 0 ? fade : 1.0f);
        if (!cur || a < 1) {
            vita2d_texture* low = images.peek(cur_url, IMG_THUMB);
            if (low)
                gfx::cover(low, 0, 0, W, HERO, prev_shot >= 0 ? fade : 1.0f);
            else if (!cur)
                gfx::spinner(W / 2, HERO / 2, 12, col::MUTED);
        }
        if (cur)
            gfx::cover(cur, 0, 0, W, HERO, sa);
        for (int i = 1; i < 3 && i < n; i++)
            images.prefetch(catalog.media_url(e.shots[(shot + i) % n]), IMG_FULL);
        if (n > 1)
            images.prefetch(catalog.media_url(e.shots[(shot + n - 1) % n]), IMG_FULL);
    } else {
        gfx::placeholder(0, 0, W, HERO, e.name, 0);
        if (icon)
            gfx::fit(icon, W / 2 - 110, 0, 220, HERO, ia * 0.3f);
    }
    gfx::vgrad(0, 0, W, 90, gfx::alpha(col::BG, 0.75f), gfx::alpha(col::BG, 0));
    gfx::vgrad(0, HERO - 150, W, 150, gfx::alpha(col::BG, 0), col::BG);
    gfx::rect(0, HERO, W, H - HERO, col::BG);

    gfx::rrect(20, 16, 98, 34, 17, gfx::alpha(col::BG, 0.7f));
    gfx::button(back_icon, 28, 23, 20);
    gfx::text(gfx::regular, 56, 38, 14, col::TEXT, "Назад");
    hits.push_back({20, 16, 98, 34, [this]() { go_back(); }});

    if (n > 1) {
        float dw = n * 14;
        for (int i = 0; i < n; i++)
            gfx::circle(W / 2 - dw / 2 + i * 14 + 7, HERO - 118, i == shot ? 4.5f : 3, i == shot ? col::TEXT : gfx::alpha(col::TEXT, 0.4f));
        hits.push_back({0, 60, W / 3.0f, 160, [this, n]() {
                            prev_shot = shot;
                            shot = (shot + n - 1) % n;
                            shot_time = now_ms();
                            shot_auto = now_ms() + 8000;
                        }});
        hits.push_back({W / 3.0f, 60, W / 3.0f, 160, [this]() {
                            viewer = true;
                            shot_auto = now_ms() + 600000;
                        }});
        hits.push_back({W * 2 / 3.0f, 60, W / 3.0f, 160, [this, n]() {
                            prev_shot = shot;
                            shot = (shot + 1) % n;
                            shot_time = now_ms();
                            shot_auto = now_ms() + 8000;
                        }});
    }

    float iy = HERO - 96;
    gfx::rrect(28, iy - 4, 104, 104, 20, col::BG);
    if (icon)
        gfx::fit_round(icon, 32, iy, 96, 96, 14, ia);
    else
        gfx::placeholder_round(32, iy, 96, 96, 16, e.name, 34);

    float bw = 236, bx = W - 28 - bw, by = HERO - 70;
    float tx = 148;
    gfx::text(gfx::bold, tx, HERO - 34, 26, col::TEXT, gfx::ellipsize(gfx::bold, 26, e.name, (int)(bx - tx - 20)));
    std::string meta = category_title(e.cat);
    if (!e.author.empty())
        meta += " · " + e.author;
    if (!e.version.empty())
        meta += " · " + e.version;
    meta += " · " + e.id;
    gfx::text(gfx::regular, tx, HERO - 8, 14, col::MUTED, gfx::ellipsize(gfx::regular, 14, meta, (int)(bx - tx - 20)));

    auto t = tasks.find(e);
    Status st = status_of(e);
    bool busy = t && (t->state == TS_RUNNING || t->state == TS_QUEUED);
    if (busy) {
        gfx::rrect(bx, by, bw, 50, 25, col::SURFACE2);
        uint64_t total = t->progress.total, done = t->progress.done;
        float f = total ? (float)done / total : 0;
        if (f > 0) {
            gfx::rrect(bx, by, std::max(50.0f, bw * f), 50, 25, col::ACCENT);
        }
        std::string s = t->state == TS_QUEUED ? "В очереди" : t->get_stage();
        if (t->state == TS_RUNNING && total)
            s += " " + std::to_string((int)(f * 100)) + "%";
        gfx::text_center(gfx::bold, bx + bw / 2, by + 31, 15, col::TEXT, gfx::ellipsize(gfx::bold, 15, s, (int)bw - 24));
    } else {
        std::string label;
        unsigned bg = col::ACCENT;
        if (st == ST_INSTALLED && launchable(e))
            label = "Запустить";
        else if (st == ST_INSTALLED) {
            label = "Переустановить";
            bg = col::SURFACE2;
        } else if (st == ST_UPDATE)
            label = catalog.patch_for(e) ? "Обновить до " + catalog.patch_for(e)->version : "Обновить";
        else
            label = e.size ? "Установить · " + fmt_size(e.size) : "Установить";
        if (st == ST_UPDATE)
            bg = col::ORANGE;
        gfx::rrect(bx, by, bw, 50, 25, bg);
        if (!related.empty() && rel_sel < 0)
            gfx::rframe(bx - 3, by - 3, bw + 6, 56, 28, 2, col::TEXT);
        gfx::button(ok_icon, bx + 18, by + 15, 20);
        gfx::text_center(gfx::bold, bx + bw / 2 + 12, by + 31, 16, st == ST_UPDATE ? col::BG : col::TEXT, label);
    }
    hits.push_back({bx, by, bw, 50, [this]() { primary_action(); }});

    float cy = HERO + 14;
    float cx = 28;
    auto chip = [&](const std::string& s, unsigned c) {
        int w = gfx::width(gfx::regular, 13, s) + 22;
        gfx::rrect(cx, cy, w, 26, 13, col::SURFACE);
        gfx::text(gfx::regular, cx + 11, cy + 18, 13, c, s);
        cx += w + 8;
    };
    if (!e.lang.empty()) {
        std::string l = e.lang;
        for (auto& c : l)
            c = toupper(c);
        chip(l, col::TEXT);
    }
    if (!e.region.empty())
        chip(e.region, col::TEXT);
    if (!e.game_version.empty())
        chip("Для версии " + e.game_version, col::TEXT);
    if (e.cat == CAT_PORTS || e.cat == CAT_HOMEBREW) {
        if (!e.data.empty())
            chip("Data скачается и встанет сама", col::GREEN);
        else if (!e.data_path.empty())
            chip("Нужны свои файлы игры: " + e.data_path, col::ORANGE);
        else
            chip("Data не нужна", col::TEXT);
    }
    if (e.cat == CAT_TRANSLATIONS && lower(e.install).find("plugin") != std::string::npos)
        chip("Нужен плагин, ставится сам", col::TEXT);
    if (e.cat == CAT_PLUGINS && !e.tai_section.empty())
        chip(e.tai_section, col::TEXT);
    for (size_t i = 0; i < detail_deps.size(); i++)
        chip(detail_deps[i].name + (detail_dep_ok[i] ? ": есть" : detail_deps[i].url.empty() ? ": нет, см. описание" : ": поставится сам"),
             detail_dep_ok[i] ? col::GREEN : detail_deps[i].url.empty() ? col::RED : col::ORANGE);
    if (!e.alt_url.empty() && version_cmp(e.version, e.alt_version) < 0)
        chip("Есть новее у автора: " + e.alt_version + " (L, на свой риск)", col::ORANGE);
    if (st == ST_INSTALLED)
        chip("Установлено", col::GREEN);
    if (st == ST_UPDATE)
        chip("Есть обновление", col::ORANGE);
    if (t && t->state == TS_FAILED)
        chip(gfx::ellipsize(gfx::regular, 13, "Ошибка: " + t->get_error(), 520), col::RED);
    if (t && t->state == TS_DONE && !t->get_note().empty())
        chip(t->get_note(), col::ORANGE);

    float top = HERO + 54, bottom = related.empty() ? FOOTER_Y - 6 : FOOTER_Y - 112;
    float lh = 22;
    float maxs = std::max(0.0f, desc_lines.size() * lh - (bottom - top));
    desc_target = std::max(0.0f, std::min(maxs, desc_target));
    desc_scroll += (desc_target - desc_scroll) * 0.25f;
    vita2d_set_clip_rectangle(0, (int)top - 4, W, (int)bottom);
    vita2d_enable_clipping();
    for (size_t i = 0; i < desc_lines.size(); i++) {
        float y = top + 16 + i * lh - desc_scroll;
        if (y < top - lh || y > bottom + lh)
            continue;
        gfx::text(gfx::regular, 32, y, 15, RGBA8(214, 210, 232, 255), desc_lines[i]);
    }
    vita2d_disable_clipping();
    if (maxs > 0) {
        float vh = bottom - top;
        float bh = vh * vh / (desc_lines.size() * lh);
        float byy = top + (vh - bh) * (desc_scroll / maxs);
        gfx::rrect(W - 12, byy, 4, bh, 2, col::LINE);
    }
    draw_related();
}

void App::open_backups()
{
    backups = backup_list();
    if (backup_sel >= (int)backups.size())
        backup_sel = std::max(0, (int)backups.size() - 1);
    screen = SCR_BACKUPS;
}

void App::saves_menu(const Entry& e)
{
    std::string label = "Сохранения " + e.id + " (" + e.name + ")";
    std::vector<Backup> mine;
    for (auto& b : backup_list())
        if (b.label == label)
            mine.push_back(b);
    modal = Modal();
    modal.on = true;
    modal.title = "Сохранения " + e.name;
    modal.lines.push_back("Папка: " + savedata_path(e.id) + ", " + fmt_size(tree_size(savedata_path(e.id))));
    modal.lines.push_back(mine.empty() ? "Копий пока нет" : "Последняя копия: " + mine[0].date + ", всего копий: " + std::to_string(mine.size()));
    modal.lines.push_back("Восстанавливай на этой же консоли, игра должна быть закрыта");
    modal.yes = "Сделать копию";
    std::string path = savedata_path(e.id);
    modal.action = [this, label, path]() {
        std::string err;
        if (backup_create(label, {path}, err))
            show_toast("Копия сохранений готова");
        else
            show_toast(err);
    };
    if (!mine.empty()) {
        Backup last = mine[0];
        modal.alt = "Восстановить последнюю";
        modal.alt_action = [this, last]() {
            std::string err;
            show_toast(backup_restore(last, err) ? "Сохранения восстановлены" : err);
        };
    }
}

#define DAEMON_PATH (APP_DIR + "/vpkgj_daemon.suprx")

bool App::daemon_enabled()
{
    if (daemon_state < 0) {
        std::string a, b;
        fs::read("ux0:tai/config.txt", a);
        fs::read("ur0:tai/config.txt", b);
        daemon_state = (a + b).find("vpkgj_daemon.suprx") != std::string::npos && !fs::exists(APP_DIR + "/daemon_off");
    }
    return daemon_state == 1;
}

void App::daemon_notify(const std::string& text)
{
    if (!daemon_enabled())
        return;
    std::string q;
    fs::read(APP_DIR + "/notify.txt", q);
    std::string line = text;
    for (auto& c : line)
        if (c == '\n' || c == '\r')
            c = ' ';
    if (line.size() > 120)
        line = line.substr(0, 120);
    fs::write(APP_DIR + "/notify.txt", q + line + "\n");
}

void App::toggle_daemon()
{
    std::string err;
    backup_create("Перед переключением фоновой проверки: config.txt", {"ux0:tai/config.txt", "ur0:tai/config.txt"}, err);
    if (daemon_enabled()) {
        fs::write(APP_DIR + "/daemon_off", "1");
        tai_remove_plugin(DAEMON_PATH);
        daemon_state = 0;
        show_toast("Фоновая проверка выключена");
        return;
    }
    std::string data;
    if (!fs::read("app0:vpkgj_daemon.suprx", data) || !fs::write(DAEMON_PATH, data)) {
        show_toast("Не удалось скопировать демон");
        return;
    }
    sceIoRemove((APP_DIR + "/daemon_off").c_str());
    if (!tai_add_plugin("*main", DAEMON_PATH, err)) {
        show_toast(err);
        return;
    }
    daemon_state = 1;
    modal = Modal();
    modal.on = true;
    modal.title = "Фоновая проверка включена";
    modal.lines.push_back("Раз в 6 часов консоль сама проверит каталог и патчи игр,");
    modal.lines.push_back("даже когда VPKGJ закрыт, и пришлёт уведомление на LiveArea.");
    modal.lines.push_back("Работает на прошивках 3.60 и 3.65. Включится после перезагрузки.");
    modal.yes = "Перезагрузить";
    modal.action = [this]() {
        if (tasks.active())
            show_toast("Дождись окончания загрузок");
        else
            scePowerRequestColdReset();
    };
}

static const char* SPEED_NAMES[] = {"Без ограничения", "4 МБ/с", "2 МБ/с", "1 МБ/с", "512 КБ/с"};
static const uint64_t SPEED_VALUES[] = {0, 4ull << 20, 2ull << 20, 1ull << 20, 512ull << 10};

void App::load_prefs()
{
    std::string s;
    if (!fs::read(APP_DIR + "/prefs.txt", s))
        return;
    int v = 0;
    if (sscanf(s.c_str(), "speed=%d", &v) == 1 && v >= 0 && v < 5)
        speed_mode = v;
}

void App::save_prefs()
{
    fs::write(APP_DIR + "/prefs.txt", "speed=" + std::to_string(speed_mode) + "\n");
}

void App::apply_speed()
{
    net::set_speed_limit(SPEED_VALUES[speed_mode]);
}

static int count_lines(const std::string& s)
{
    int n = 0;
    for (char c : s)
        if (c == '\n')
            n++;
    return n;
}

void App::poll_bg()
{
    if (now_ms() - bg_polled < 1500)
        return;
    bg_polled = now_ms();
    std::string q, add, st;
    fs::read(APP_DIR + "/bgq.txt", q);
    fs::read(APP_DIR + "/bgq.add", add);
    bg_left = count_lines(q) + count_lines(add);
    bg_name.clear();
    bg_done = bg_total = 0;
    if (bg_left && fs::read(APP_DIR + "/bgstate.txt", st)) {
        size_t a = st.find('\t');
        size_t b = a == std::string::npos ? a : st.find('\t', a + 1);
        if (b != std::string::npos) {
            unsigned d = 0, t = 0;
            bg_name = st.substr(a + 1, b - a - 1);
            sscanf(st.c_str() + b + 1, "%u\t%u", &d, &t);
            bg_done = (uint64_t)d * 1024;
            bg_total = (uint64_t)t * 1024;
        }
    }
    std::string done;
    if (!fs::read(APP_DIR + "/bgdone.txt", done) || done.empty())
        return;
    sceIoRemove((APP_DIR + "/bgdone.work").c_str());
    if (sceIoRename((APP_DIR + "/bgdone.txt").c_str(), (APP_DIR + "/bgdone.work").c_str()) < 0)
        return;
    fs::read(APP_DIR + "/bgdone.work", done);
    sceIoRemove((APP_DIR + "/bgdone.work").c_str());
    int added = 0;
    size_t pos = 0;
    while (pos < done.size()) {
        size_t n = done.find('\n', pos);
        std::string key = trim(done.substr(pos, n == std::string::npos ? std::string::npos : n - pos));
        pos = n == std::string::npos ? done.size() : n + 1;
        size_t s1 = key.find('/');
        size_t s2 = s1 == std::string::npos ? s1 : key.find('/', s1 + 1);
        if (s2 == std::string::npos)
            continue;
        std::string cat = key.substr(0, s1), id = key.substr(s1 + 1, s2 - s1 - 1), ver = key.substr(s2 + 1);
        for (int c = 0; c < CAT_COUNT; c++) {
            if (cat != category_key((Category)c))
                continue;
            const Entry* best = nullptr;
            for (auto& e : catalog.items[c])
                if (e.id == id && (!best || e.version == ver))
                    best = &e;
            if (best) {
                tasks.add(*best, deps_of(*best));
                added++;
            }
        }
    }
    if (added)
        show_toast("Скачано в фоне, ставлю: " + std::to_string(added));
}

void App::ask_daemon()
{
    modal = Modal();
    modal.on = true;
    modal.title = "Нужна фоновая служба";
    modal.lines.push_back("Порты, хоумбрю и переводы качает в фоне демон VPKGJ.");
    modal.lines.push_back("Он работает даже когда приложение закрыто и шлёт уведомление,");
    modal.lines.push_back("а установка займёт пару секунд при следующем запуске VPKGJ.");
    modal.yes = "Включить";
    modal.action = [this]() { toggle_daemon(); };
}

void App::queue_background(const Entry& e)
{
    if (!daemon_enabled()) {
        ask_daemon();
        return;
    }
    std::vector<std::string> urls;
    auto split_by = [](const std::string& s, char sep) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s + sep) {
            if (c == sep) {
                if (!trim(cur).empty())
                    out.push_back(trim(cur));
                cur.clear();
            } else {
                cur += c;
            }
        }
        return out;
    };
    if (e.cat == CAT_TRANSLATIONS) {
        for (auto& part : split_by(e.url, ';'))
            urls.push_back(first_link(part));
    } else {
        urls.push_back(first_link(e.url));
        if (e.cat == CAT_PORTS || e.cat == CAT_HOMEBREW)
            for (auto& part : split_by(e.data, ';')) {
                std::string src = part;
                size_t gt = src.find('>');
                if (gt != std::string::npos)
                    src = trim(src.substr(0, gt));
                urls.push_back(first_link(src));
            }
    }
    std::string key = std::string(category_key(e.cat)) + "/" + e.id + "/" + (e.version.empty() ? "-" : e.version);
    std::string name = e.name;
    for (auto& c : name)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    std::string lines;
    int skipped = 0;
    for (auto& u : urls) {
        if (u.empty())
            continue;
        if (u.find("google.com") != std::string::npos) {
            skipped++;
            continue;
        }
        lines += key + "\t" + name + "\t" + u + "\n";
    }
    if (lines.empty()) {
        show_toast("Этот файл с Google Drive, его качает только приложение");
        return;
    }
    std::string q;
    fs::read(APP_DIR + "/bgq.add", q);
    fs::write(APP_DIR + "/bgq.add", q + lines);
    bg_polled = 0;
    show_toast(skipped ? "В фоне скачается часть файлов, остальное докачает VPKGJ" : "Скачается в фоне, можно закрыть VPKGJ");
}

void App::cancel_background()
{
    fs::write(APP_DIR + "/bgq.stop", "1");
    sceIoRemove((APP_DIR + "/bgq.add").c_str());
    bg_polled = 0;
    show_toast("Фоновые загрузки остановлены");
}

bool App::app_update_available()
{
    return !catalog.app_version.empty() && !catalog.app_url.empty() && version_cmp(VPKGJ_VERSION, catalog.app_version) < 0;
}

void App::ask_app_update(bool manual)
{
    if (!app_update_available()) {
        if (manual)
            show_toast(catalog.app_version.empty() ? "Не удалось узнать последнюю версию" : "У тебя последняя версия VPKGJ");
        return;
    }
    if (!manual && (modal.on || ignored_app(catalog.app_version)))
        return;
    app_prompted = true;
    modal = Modal();
    modal.on = true;
    modal.title = "Вышел VPKGJ " + catalog.app_version;
    modal.lines.push_back("Сейчас стоит " VPKGJ_VERSION ". Обновление скачается и встанет само,");
    modal.lines.push_back("после этого VPKGJ перезапустится.");
    std::string notes;
    for (char c : catalog.app_notes)
        if (c != '\r' && c != '#' && c != '*')
            notes += c;
    for (auto& l : gfx::wrap(gfx::regular, 15, trim(notes), 540))
        if (modal.lines.size() < 7)
            modal.lines.push_back(l);
    modal.yes = "Обновить";
    modal.action = [this]() {
        Entry e;
        e.cat = CAT_HOMEBREW;
        e.id = "VPKG00001";
        e.name = "VPKGJ " + catalog.app_version;
        e.version = catalog.app_version;
        e.url = catalog.app_url;
        e.install = "self";
        tasks.add(e);
        set_tab(TAB_DOWNLOADS);
        screen = SCR_BROWSE;
    };
    modal.alt = "Не напоминать";
    std::string v = catalog.app_version;
    modal.alt_action = [this, v]() {
        fs::write(APP_DIR + "/app_skip.txt", v);
        show_toast("Напомню, когда выйдет следующая версия");
    };
}

void App::check_reboot()
{
    if (modal.on)
        return;
    for (auto& t : tasks.list()) {
        if (t->state == TS_FAILED && !t->synced) {
            t->synced = true;
            daemon_notify("Не удалось установить: " + t->entry.name);
        }
        if (t->state == TS_DONE && !t->synced) {
            t->synced = true;
            daemon_notify("Установлено: " + t->entry.name);
            if (t->entry.cat == CAT_GAMES) {
                installed.refresh();
                start_refresh(false);
            } else {
                installed.refresh();
                refresh_updates();
            }
        }
        if (t->state == TS_DONE && t->restart && !t->notified) {
            t->notified = true;
            modal = Modal();
            modal.on = true;
            modal.title = "VPKGJ обновлён";
            modal.lines.push_back("Новая версия установлена, нужен перезапуск приложения.");
            if (tasks.active())
                modal.lines.push_back("Остальные загрузки продолжатся после перезапуска с места остановки.");
            modal.yes = "Перезапустить";
            modal.action = [this]() {
                tasks.stop();
                images.stop();
                sceAppMgrLoadExec("app0:eboot.bin", nullptr, nullptr);
            };
            return;
        }
        if (t->state == TS_DONE && t->reboot && !t->notified) {
            t->notified = true;
            modal = Modal();
            modal.on = true;
            modal.title = "Нужна перезагрузка";
            modal.lines.push_back(t->entry.name + " установлен.");
            modal.lines.push_back("Плагины включатся только после перезагрузки консоли.");
            if (tasks.active())
                modal.lines.push_back("Сначала дождись остальных загрузок.");
            modal.yes = "Перезагрузить";
            modal.action = [this]() {
                if (tasks.active())
                    show_toast("Дождись окончания загрузок");
                else
                    scePowerRequestColdReset();
            };
            return;
        }
    }
}

void App::draw_backups()
{
    gfx::text(gfx::bold, 32, 46, 24, col::TEXT, "Резервные копии");
    gfx::text_right(gfx::regular, W - 32, 44, 14, col::MUTED, "ux0:data/VPKGJ/backups");
    if (backups.empty()) {
        gfx::text_center(gfx::bold, W / 2, 260, 20, col::TEXT, "Копий пока нет");
        gfx::text_center(gfx::regular, W / 2, 290, 15, col::MUTED, "Они появятся перед установкой плагинов и переводов");
        gfx::text_center(gfx::regular, W / 2, 314, 15, col::MUTED, "Копию сохранений игры делай в её карточке кнопкой R");
        return;
    }
    float y = 70;
    int first = std::max(0, backup_sel - 4);
    for (int i = first; i < (int)backups.size() && y < FOOTER_Y - 60; i++) {
        auto& b = backups[i];
        bool on = i == backup_sel;
        gfx::rrect(28, y, W - 56, 62, 12, on ? col::SURFACE2 : col::SURFACE);
        if (on)
            gfx::rframe(28, y, W - 56, 62, 12, 2, col::ACCENT);
        gfx::text(gfx::bold, 46, y + 26, 15, col::TEXT, gfx::ellipsize(gfx::bold, 15, b.label, 700));
        std::string items;
        for (auto& it : b.items)
            items += (items.empty() ? "" : ", ") + it.first;
        gfx::text(gfx::regular, 46, y + 48, 12, col::MUTED, gfx::ellipsize(gfx::regular, 12, b.date + " · " + fmt_size(b.size) + " · " + items, 860));
        int idx = i;
        hits.push_back({28, y, (float)(W - 56), 62, [this, idx]() { backup_sel = idx; }});
        y += 70;
    }
}

void App::draw_viewer()
{
    hits.clear();
    gfx::rect(0, 0, W, H, RGBA8(0, 0, 0, 255));
    int n = (int)detail.shots.size();
    if (!n) {
        viewer = false;
        return;
    }
    float fade = std::min(1.0f, (now_ms() - shot_time) / 250.0f);
    if (prev_shot >= 0 && fade < 1) {
        vita2d_texture* p = images.peek(catalog.media_url(detail.shots[prev_shot]), IMG_FULL);
        if (p)
            gfx::fit(p, 0, 0, W, H, 1 - fade);
    }
    float a = 0;
    std::string url = catalog.media_url(detail.shots[shot]);
    vita2d_texture* t = images.get(url, &a, IMG_FULL);
    if (!t || a < 1) {
        vita2d_texture* low = images.peek(url, IMG_THUMB);
        if (low)
            gfx::fit(low, 0, 0, W, H, fade);
        else if (!t)
            gfx::spinner(W / 2, H / 2, 14, col::MUTED);
    }
    if (t)
        gfx::fit(t, 0, 0, W, H, std::min(a, fade));
    if (n > 1) {
        images.prefetch(catalog.media_url(detail.shots[(shot + 1) % n]), IMG_FULL);
        images.prefetch(catalog.media_url(detail.shots[(shot + n - 1) % n]), IMG_FULL);
    }
    std::string c = std::to_string(shot + 1) + " / " + std::to_string(n);
    int w = gfx::width(gfx::bold, 14, c) + 24;
    gfx::rrect(W - 20 - w, 16, w, 30, 15, gfx::alpha(col::BG, 0.7f));
    gfx::text(gfx::bold, W - 20 - w + 12, 36, 14, col::TEXT, c);
    gfx::rrect(20, H - 44, 300, 30, 15, gfx::alpha(col::BG, 0.7f));
    float x = gfx::hint(30, H - 39, back_icon, "Закрыть");
    if (n > 1)
        gfx::text(gfx::regular, x, H - 24, 14, col::MUTED, "← → листать");
    hits.push_back({0, 0, W / 3.0f, (float)H, [this, n]() {
                        prev_shot = shot;
                        shot = (shot + n - 1) % n;
                        shot_time = now_ms();
                    }});
    hits.push_back({W * 2 / 3.0f, 0, W / 3.0f, (float)H, [this, n]() {
                        prev_shot = shot;
                        shot = (shot + 1) % n;
                        shot_time = now_ms();
                    }});
    hits.push_back({W / 3.0f, 0, W / 3.0f, (float)H, [this]() { viewer = false; }});
}

void App::go_back()
{
    if (back_stack.empty()) {
        screen = SCR_BROWSE;
        return;
    }
    Entry prev = back_stack.back();
    back_stack.pop_back();
    open_detail(prev);
}

void App::draw_related()
{
    if (related.empty())
        return;
    const float cw = 228, ch = 74, gap = 12;
    float y = FOOTER_Y - 84;
    gfx::text(gfx::bold, 32, y - 12, 14, col::TEXT, "Связанное");
    gfx::text(gfx::regular, 32 + gfx::width(gfx::bold, 14, "Связанное") + 8, y - 12, 13, col::MUTED, std::to_string(related.size()));
    float target = rel_sel < 0 ? 0 : std::max(0.0f, (rel_sel - 2) * (cw + gap));
    rel_scroll += (target - rel_scroll) * 0.25f;
    vita2d_set_clip_rectangle(0, (int)y - 6, W, (int)(y + ch + 6));
    vita2d_enable_clipping();
    for (size_t i = 0; i < related.size(); i++) {
        float x = 28 + i * (cw + gap) - rel_scroll;
        if (x > W || x + cw < 0)
            continue;
        const Entry& r = related[i];
        bool on = (int)i == rel_sel;
        gfx::rrect(x, y, cw, ch, 14, on ? col::SURFACE2 : gfx::alpha(col::SURFACE, 0.92f));
        if (on)
            gfx::rframe(x, y, cw, ch, 14, 2, col::ACCENT);
        float a = 0;
        vita2d_texture* ic = images.get(icon_url(r), &a, IMG_THUMB);
        if (ic)
            gfx::fit_round(ic, x + 10, y + 10, 54, 54, 10, a);
        else
            gfx::placeholder_round(x + 10, y + 10, 54, 54, 10, r.name, 18);
        unsigned kc = related_kind[i] == "Русификатор" ? col::GREEN : related_kind[i] == "DLC" ? col::ORANGE : col::ACCENT;
        gfx::text(gfx::bold, x + 74, y + 28, 12, kc, related_kind[i]);
        gfx::text(gfx::bold, x + 74, y + 48, 14, col::TEXT, gfx::ellipsize(gfx::bold, 14, r.name, (int)cw - 84));
        Status st = installed.status(r);
        std::string sub = st == ST_INSTALLED ? "Установлено" : r.size ? fmt_size(r.size) : r.version;
        gfx::text(gfx::regular, x + 74, y + 65, 12, st == ST_INSTALLED ? col::GREEN : col::MUTED, sub);
        int idx = (int)i;
        hits.push_back({x, y, cw, ch, [this, idx]() {
                            if (rel_sel == idx) {
                                back_stack.push_back(detail);
                                Entry next = related[idx];
                                open_detail(next);
                            } else {
                                rel_sel = idx;
                            }
                        }});
    }
    vita2d_disable_clipping();
}

void App::draw_downloads()
{
    auto list = tasks.list();
    if (updates_count) {
        std::string u = "Доступно обновлений: " + std::to_string(updates_count);
        gfx::rrect(W - 28 - 330, GRID_TOP, 330, 34, 17, gfx::alpha(col::ORANGE, 0.18f));
        gfx::text(gfx::bold, W - 28 - 316, GRID_TOP + 22, 14, col::ORANGE, u);
        gfx::button(BTN_SQUARE, W - 28 - 110, GRID_TOP + 7, 20);
        gfx::text(gfx::regular, W - 28 - 84, GRID_TOP + 22, 13, col::TEXT, "обновить всё");
    }
    if (list.empty() && !bg_left) {
        gfx::mascot(W / 2 - 70, 112, 140, 0.95f);
        gfx::text_center(gfx::bold, W / 2, 300, 20, col::TEXT, "Загрузок нет");
        gfx::text_center(gfx::regular, W / 2, 328, 15, col::MUTED, "Выбери игру или порт: поставить сразу или скачать в фоне");
        gfx::text_center(gfx::regular, W / 2, 356, 14, col::MUTED, "Свободно на ux0: " + fmt_size(free_cached()));
        return;
    }
    if (dl_sel >= (int)list.size())
        dl_sel = std::max(0, (int)list.size() - 1);
    float y = GRID_TOP + 4;
    gfx::text(gfx::regular, 32, y + 12, 14, col::MUTED, "Свободно на ux0: " + fmt_size(free_cached()));
    y += 28;
    if (bg_left) {
        gfx::rrect(28, y, W - 56, 60, 14, gfx::alpha(col::ACCENT, 0.12f));
        gfx::rframe(28, y, W - 56, 60, 14, 1, gfx::alpha(col::ACCENT, 0.5f));
        gfx::spinner(56, y + 30, 9, col::ACCENT);
        std::string head = "В фоне через демон: файлов " + std::to_string(bg_left);
        gfx::text(gfx::bold, 80, y + 25, 15, col::TEXT, head);
        std::string sub = bg_name.empty() ? "Ждёт сеть или своей очереди" : bg_name;
        if (bg_total)
            sub += " · " + fmt_size(bg_done) + " из " + fmt_size(bg_total);
        else if (bg_done)
            sub += " · " + fmt_size(bg_done);
        gfx::text(gfx::regular, 80, y + 46, 13, col::MUTED, gfx::ellipsize(gfx::regular, 13, sub, 560));
        if (bg_total) {
            float f = (float)bg_done / bg_total;
            gfx::rrect(W - 46 - 160, y + 26, 160, 6, 3, col::LINE);
            gfx::rrect(W - 46 - 160, y + 26, std::max(6.0f, 160 * f), 6, 3, col::ACCENT);
        }
        y += 70;
    }
    int first = std::max(0, dl_sel - 4);
    uint64_t now = now_ms();
    for (int i = first; i < (int)list.size() && y < FOOTER_Y - 70; i++) {
        auto& t = list[i];
        bool on = i == dl_sel;
        gfx::rrect(28, y, W - 56, 72, 14, on ? col::SURFACE2 : col::SURFACE);
        if (on)
            gfx::rframe(28, y, W - 56, 72, 14, 2, col::ACCENT);
        float a = 0;
        vita2d_texture* icon = images.get(icon_url(t->entry), &a, IMG_THUMB);
        if (icon) {
            gfx::fit(icon, 42, y + 12, 48, 48, a);
            gfx::corners(42, y + 12, 48, 48, 10, on ? col::SURFACE2 : col::SURFACE);
        } else {
            gfx::rrect(42, y + 12, 48, 48, 10, col::LINE);
        }
        gfx::text(gfx::bold, 106, y + 30, 16, col::TEXT, gfx::ellipsize(gfx::bold, 16, t->entry.name, 520));
        uint64_t total = t->progress.total, done = t->progress.done;
        std::string status;
        unsigned sc = col::MUTED;
        int st = t->state;
        if (st == TS_RUNNING) {
            if (now - t->sample_time >= 1000) {
                if (t->sample_time && done >= t->sample_bytes)
                    t->speed = t->speed * 0.5 + (done - t->sample_bytes) * 1000.0 / (now - t->sample_time) * 0.5;
                t->sample_time = now;
                t->sample_bytes = done;
            }
            status = t->get_stage();
            if (total)
                status += " · " + fmt_size(done) + " из " + fmt_size(total);
            if (t->speed > 1024)
                status += " · " + fmt_size((uint64_t)t->speed) + "/с";
        } else if (st == TS_QUEUED) {
            status = "В очереди";
        } else if (st == TS_DONE) {
            status = t->get_note().empty() ? "Установлено" : "Установлено. " + t->get_note();
            sc = col::GREEN;
        } else if (st == TS_CANCELED) {
            status = "Отменено";
        } else {
            status = "Ошибка: " + t->get_error();
            sc = col::RED;
        }
        gfx::text(gfx::regular, 106, y + 52, 13, sc, gfx::ellipsize(gfx::regular, 13, status, 600));
        if (st == TS_RUNNING) {
            float f = total ? (float)done / total : 0;
            gfx::rrect(106, y + 60, 620, 4, 2, col::LINE);
            if (f > 0)
                gfx::rrect(106, y + 60, std::max(4.0f, 620 * f), 4, 2, col::ACCENT);
            gfx::text_right(gfx::bold, W - 46, y + 42, 18, col::TEXT, total ? std::to_string((int)(f * 100)) + "%" : "");
        }
        int idx = i;
        hits.push_back({28, y, (float)(W - 56), 72, [this, idx]() { dl_sel = idx; }});
        y += 82;
    }
}

void App::draw_settings()
{
    gfx::text(gfx::bold, 32, 46, 24, col::TEXT, "Настройки");
    struct Row {
        std::string title, value;
        unsigned vc;
        std::function<void()> fn;
    };
    bool upd = app_update_available();
    std::vector<Row> rows = {
        {"Обновление VPKGJ", upd ? "есть " + catalog.app_version + ", поставить" : "у тебя " VPKGJ_VERSION, upd ? col::ORANGE : col::MUTED,
         [this, upd]() {
             if (upd)
                 ask_app_update(true);
             else {
                 start_refresh(true);
                 show_toast("Проверяю обновления");
             }
         }},
        {"Репозиторий каталога", settings.repo, col::MUTED, [this]() {
             ime_purpose = IME_REPO;
             ime::open("Репозиторий каталога (user/repo)", settings.repo, 100);
         }},
        {"Ветка", settings.branch, col::MUTED, [this]() {
             ime_purpose = IME_BRANCH;
             ime::open("Ветка", settings.branch, 60);
         }},
        {"Обновить каталог", catalog.version.empty() ? "" : "версия " + catalog.version, col::MUTED, [this]() { start_refresh(true); }},
        {"Скорость загрузки", std::string("‹ ") + SPEED_NAMES[speed_mode] + " ›", speed_mode ? col::ACCENT : col::MUTED, [this]() {
             speed_mode = (speed_mode + 1) % 5;
             save_prefs();
             apply_speed();
         }},
        {"Фоновая служба", daemon_enabled() ? "включена" : "выключена", daemon_enabled() ? col::GREEN : col::MUTED, [this]() { toggle_daemon(); }},
        {"Резервные копии", "config.txt, rePatch, сейвы", col::MUTED, [this]() { open_backups(); }},
        {"Скачать все обложки", "прокрутка без подгрузки", col::MUTED, [this]() { start_preload(); }},
        {"Очистить кеш картинок", "", col::MUTED, [this]() {
             images.clear_disk();
             show_toast("Кеш картинок очищен");
         }},
        {"Удалить временные файлы", "", col::MUTED, [this]() {
             if (tasks.active()) {
                 show_toast("Дождись окончания загрузок");
                 return;
             }
             fs::rmtree(APP_DIR + "/dl");
             fs::rmtree("ux0:temp/vpkgj");
             fs::mkdirs(APP_DIR + "/dl");
             fs::mkdirs("ux0:temp/vpkgj");
             show_toast("Временные файлы удалены");
         }},
    };
    settings_actions.clear();
    if (settings_sel >= (int)rows.size())
        settings_sel = (int)rows.size() - 1;
    float y = 68, rw = 600;
    for (int i = 0; i < (int)rows.size(); i++) {
        bool on = i == settings_sel;
        gfx::rrect(28, y, rw, 40, 12, on ? col::SURFACE2 : gfx::alpha(col::SURFACE, 0.9f));
        if (on)
            gfx::rframe(28, y, rw, 40, 12, 2, col::ACCENT);
        gfx::text(on ? gfx::bold : gfx::regular, 46, y + 26, 15, col::TEXT, rows[i].title);
        gfx::text_right(gfx::regular, 28 + rw - 18, y + 26, 14, rows[i].vc, gfx::ellipsize(gfx::regular, 14, rows[i].value, 300));
        settings_actions.push_back(rows[i].fn);
        int idx = i;
        hits.push_back({28, y, rw, 40, [this, idx]() {
                            if (settings_sel == idx && idx < (int)settings_actions.size())
                                settings_actions[idx]();
                            settings_sel = idx;
                        }});
        y += 44;
    }
    draw_about(68);
}

void App::start_preload()
{
    std::vector<std::string> urls;
    for (int c = 0; c < CAT_COUNT; c++)
        for (auto& e : catalog.items[c]) {
            if (!e.icon.empty())
                urls.push_back(catalog.media_url(e.icon));
            if (!e.shots.empty())
                urls.push_back(catalog.media_url(e.shots[0]));
        }
    images.preload(urls);
    pre_t = now_ms();
    pre_b = 0;
    pre_speed = 0;
    if (images.pre.total == 0) {
        fs::write(APP_DIR + "/preload.txt", catalog.media + "|" + catalog.version);
        if (screen == SCR_PRELOAD)
            screen = SCR_BROWSE;
        return;
    }
    screen = SCR_PRELOAD;
}

void App::input_preload()
{
    Preload& p = images.pre;
    if (!pre_checked || p.running != 0)
        return;
    if (p.failed == 0)
        fs::write(APP_DIR + "/preload.txt", catalog.media + "|" + catalog.version);
    screen = SCR_BROWSE;
    show_toast(p.failed ? "Готово, не скачалось: " + std::to_string(p.failed.load()) + ", докачаю при следующем запуске" : "Все обложки скачаны");
}

void App::draw_preload()
{
    Preload& p = images.pre;
    uint64_t live = 0;
    for (auto& l : p.live)
        live += l.done;
    uint64_t bytes = p.bytes + live;
    uint64_t now = now_ms();
    if (now - pre_t >= 1000) {
        double sp = (double)(bytes > pre_b ? bytes - pre_b : 0) * 1000.0 / (double)(now - pre_t);
        pre_speed = pre_speed == 0 ? sp : pre_speed * 0.6 + sp * 0.4;
        pre_t = now;
        pre_b = bytes;
    }
    if (!pre_checked) {
        gfx::mascot(W / 2 - 75, 36, 150, 1);
        gfx::text_center(gfx::bold, W / 2, 222, 26, col::TEXT, "Готовлю VPKGJ");
        gfx::text_center(gfx::regular, W / 2, 250, 15, col::MUTED, "Загружаю каталог...");
        gfx::spinner(W / 2, 300, 12, col::ACCENT);
        gfx::text_center(gfx::regular, W / 2, 400, 13, col::MUTED, "Сделано специально для");
        gfx::text_center(gfx::bold, W / 2, 422, 17, col::ACCENT2, VPKGJ_FOR);
        gfx::text_center(gfx::regular, W / 2, 450, 13, col::MUTED, "by " VPKGJ_AUTHOR " · " VPKGJ_TG);
        return;
    }
    int total = std::max(1, p.total.load()), done = std::min(p.done.load(), total);
    float frac = (float)done / total;
    gfx::mascot(W / 2 - 75, 36, 150, 1);
    gfx::text_center(gfx::bold, W / 2, 222, 26, col::TEXT, "Готовлю VPKGJ");
    gfx::text_center(gfx::regular, W / 2, 250, 15, col::MUTED, "Качаю обложки и скриншоты один раз, дальше всё листается мгновенно");
    float bx = 180, bw = W - 360, by = 284;
    gfx::rrect(bx, by, bw, 18, 9, col::SURFACE2);
    if (frac > 0)
        gfx::rrect(bx, by, std::max(18.0f, bw * frac), 18, 9, col::ACCENT);
    gfx::text(gfx::bold, bx, by + 46, 18, col::TEXT, std::to_string((int)(frac * 100)) + "%");
    gfx::text_right(gfx::regular, bx + bw, by + 46, 15, col::MUTED, std::to_string(done) + " из " + std::to_string(p.total.load()));
    std::string info = fmt_size(bytes) + " · " + fmt_size((uint64_t)pre_speed) + "/с";
    if (pre_speed > 1024 && done < total) {
        uint64_t avg = done > 0 && bytes > 0 ? bytes / std::max(1, done) : 40 * 1024;
        int left = (int)((double)(total - done) * avg / pre_speed);
        info += " · осталось ~" + (left >= 60 ? std::to_string(left / 60) + " мин" : std::to_string(left) + " с");
    }
    if (p.failed)
        info += " · ошибок " + std::to_string(p.failed.load());
    gfx::text_center(gfx::regular, W / 2, by + 46, 15, col::ACCENT2, info);
    gfx::text_center(gfx::regular, W / 2, 400, 13, col::MUTED, "Сделано специально для");
    gfx::text_center(gfx::bold, W / 2, 422, 17, col::ACCENT2, VPKGJ_FOR);
    gfx::text_center(gfx::regular, W / 2, 450, 13, col::MUTED, "by " VPKGJ_AUTHOR " · " VPKGJ_TG);
    gfx::spinner(W / 2, 500, 10, col::ACCENT);
}

void App::draw_about(float y)
{
    float x = 646, w = W - 28 - x, h = FOOTER_Y - 12 - y;
    gfx::rrect(x, y, w, h, 18, gfx::alpha(col::SURFACE, 0.92f));
    gfx::rframe(x, y, w, h, 18, 1, col::LINE);
    gfx::vgrad(x + 1, y + 1, w - 2, 160, gfx::alpha(col::ACCENT2, 0.22f), gfx::alpha(col::ACCENT2, 0));
    gfx::mascot(x + w / 2 - 70, y + 14, 140, 1);
    float ty = y + 186;
    gfx::text_center(gfx::bold, x + w / 2, ty, 26, col::TEXT, "VPKGJ");
    gfx::text_center(gfx::regular, x + w / 2, ty + 22, 13, col::MUTED, "версия " VPKGJ_VERSION " · магазин для PS Vita");
    ty += 52;
    gfx::rrect(x + 16, ty, w - 32, 54, 14, gfx::alpha(col::BG, 0.6f));
    gfx::text(gfx::regular, x + 30, ty + 22, 12, col::MUTED, "Автор");
    gfx::text(gfx::bold, x + 30, ty + 42, 16, col::TEXT, VPKGJ_AUTHOR);
    gfx::text_right(gfx::bold, x + w - 30, ty + 42, 14, col::ACCENT, VPKGJ_TG);
    ty += 66;
    gfx::text_center(gfx::regular, x + w / 2, ty + 12, 13, col::MUTED, "Сделано специально для");
    gfx::text_center(gfx::bold, x + w / 2, ty + 32, 16, col::ACCENT2, VPKGJ_FOR);
    ty += 46;
    std::string pc = pkgj_config_path();
    std::vector<std::string> about = {
        "Свободно на ux0: " + fmt_size(free_cached()),
        pc.empty() ? "Списки pkgj не найдены" : "pkgj: игр " + std::to_string(catalog.pkgj_games) + ", DLC " + std::to_string(catalog.pkgj_dlcs),
        "github.com/" APP_REPO,
        "GPL-3.0 · по мотивам pkgj и VitaShell",
    };
    for (auto& l : about) {
        if (ty + 16 > y + h)
            break;
        gfx::text(gfx::regular, x + 20, ty + 14, 12, col::MUTED, gfx::ellipsize(gfx::regular, 12, l, (int)w - 40));
        ty += 20;
    }
}

void App::draw_footer()
{
    if (screen == SCR_DETAIL)
        gfx::rect(0, FOOTER_Y, W, H - FOOTER_Y, col::BG);
    float x = 28, y = FOOTER_Y + 8;
    if (modal.on) {
        x = gfx::hint(x, y, ok_icon, modal.yes);
        if (!modal.alt.empty())
            x = gfx::hint(x, y, BTN_TRIANGLE, modal.alt);
        gfx::hint(x, y, back_icon, "Закрыть");
        return;
    }
    if (screen == SCR_BROWSE && filters_open) {
        x = gfx::hint(x, y, ok_icon, "Изменить");
        x = gfx::hint(x, y, BTN_TRIANGLE, "Сбросить всё");
        gfx::hint(x, y, back_icon, "Закрыть");
        return;
    }
    if (screen == SCR_BROWSE && tab == TAB_UPDATES) {
        x = gfx::hint(x, y, ok_icon, "Обновить");
        if (updates_count)
            x = gfx::hint(x, y, BTN_SQUARE, "Обновить всё");
        x = gfx::hint(x, y, BTN_TRIANGLE, "Пропустить версию");
        gfx::hint(x, y, BTN_START, "Настройки");
        return;
    }
    if (screen == SCR_BROWSE && tab < CAT_COUNT) {
        x = gfx::hint(x, y, ok_icon, "Открыть");
        x = gfx::hint(x, y, BTN_TRIANGLE, "Поиск");
        x = gfx::hint(x, y, BTN_SELECT, "Фильтры");
        x = gfx::hint(x, y, BTN_SQUARE, "Обновить");
        if (!query.empty())
            x = gfx::hint(x, y, back_icon, "Сбросить поиск");
        gfx::hint(x, y, BTN_START, "Настройки");
    } else if (screen == SCR_BROWSE) {
        x = gfx::hint(x, y, ok_icon, "Отменить или повторить");
        if (updates_count)
            x = gfx::hint(x, y, BTN_SQUARE, "Обновить всё");
        x = gfx::hint(x, y, BTN_TRIANGLE, "Очистить готовые");
        if (bg_left)
            x = gfx::hint(x, y, BTN_SELECT, "Стоп фон");
        gfx::hint(x, y, BTN_START, "Настройки");
    } else if (screen == SCR_DETAIL && rel_sel >= 0) {
        x = gfx::hint(x, y, ok_icon, "Открыть");
        x = gfx::hint(x, y, back_icon, "Назад");
        gfx::text(gfx::regular, x, y + 15, 14, col::MUTED, "↑ к кнопке установки");
    } else if (screen == SCR_DETAIL) {
        Status st = installed.status(detail);
        auto t = tasks.find(detail);
        bool busy = t && (t->state == TS_RUNNING || t->state == TS_QUEUED);
        std::string ok = busy ? "Отменить" : st == ST_INSTALLED && launchable(detail) ? "Запустить" : "Установить";
        x = gfx::hint(x, y, ok_icon, ok);
        if (st != ST_NONE && !busy)
            x = gfx::hint(x, y, BTN_SQUARE, "Переустановить");
        if (!busy && !(detail.cat == CAT_PLUGINS && detail.url.empty()))
            x = gfx::hint(x, y, BTN_TRIANGLE, "В фоне");
        if (!detail.alt_url.empty() && version_cmp(detail.version, detail.alt_version) < 0 && !busy)
            x = gfx::hint(x, y, BTN_L, "Версия " + detail.alt_version);
        if (has_saves)
            x = gfx::hint(x, y, BTN_R, "Сохранения");
        x = gfx::hint(x, y, back_icon, "Назад");
        if (!detail.shots.empty())
            x = gfx::hint(x, y, BTN_SELECT, "Скриншоты");
        if (!related.empty())
            gfx::text(gfx::regular, x, y + 15, 14, col::MUTED, "↓ связанное");
        else if (detail.shots.size() > 1)
            gfx::text(gfx::regular, x, y + 15, 14, col::MUTED, "← → скриншоты");
    } else if (screen == SCR_BACKUPS) {
        x = gfx::hint(x, y, ok_icon, "Восстановить");
        x = gfx::hint(x, y, BTN_TRIANGLE, "Удалить");
        gfx::hint(x, y, back_icon, "Назад");
    } else {
        x = gfx::hint(x, y, ok_icon, "Выбрать");
        gfx::hint(x, y, back_icon, "Назад");
    }
}

void App::draw_modal()
{
    if (!modal.on)
        return;
    hits.push_back({0, 0, (float)W, (float)H, []() {}});
    gfx::rect(0, 0, W, H, gfx::alpha(RGBA8(0, 0, 0, 255), 0.6f));
    float w = 600;
    float h = 120 + modal.lines.size() * 24;
    float x = (W - w) / 2, y = (H - h) / 2 - 10;
    gfx::rrect(x, y, w, h, 20, col::SURFACE);
    gfx::text(gfx::bold, x + 28, y + 44, 20, col::TEXT, gfx::ellipsize(gfx::bold, 20, modal.title, (int)w - 56));
    float ly = y + 76;
    for (auto& l : modal.lines) {
        gfx::text(gfx::regular, x + 28, ly, 15, col::MUTED, gfx::ellipsize(gfx::regular, 15, l, (int)w - 56));
        ly += 24;
    }
    float by = y + h - 50;
    float bw = gfx::width(gfx::bold, 15, modal.yes) + 64;
    gfx::rrect(x + w - 28 - bw, by, bw, 36, 18, col::ACCENT);
    gfx::button(ok_icon, x + w - 28 - bw + 12, by + 8, 20);
    gfx::text(gfx::bold, x + w - 28 - bw + 42, by + 23, 15, col::TEXT, modal.yes);
    hits.push_back({x + w - 28 - bw, by, bw, 36, [this]() {
                        modal.on = false;
                        if (modal.action) {
                            auto fn = modal.action;
                            fn();
                            }
                    }});
    float right = x + w - 28 - bw - 12;
    if (!modal.alt.empty()) {
        float aw = gfx::width(gfx::regular, 15, modal.alt) + 64;
        float ax = right - aw;
        gfx::rrect(ax, by, aw, 36, 18, col::SURFACE2);
        gfx::button(BTN_TRIANGLE, ax + 12, by + 8, 20);
        gfx::text(gfx::regular, ax + 42, by + 23, 15, col::TEXT, modal.alt);
        hits.push_back({ax, by, aw, 36, [this]() {
                            modal.on = false;
                            if (modal.alt_action) {
                                auto fn = modal.alt_action;
                                fn();
                                }
                        }});
        right = ax - 12;
    }
    float cw = gfx::width(gfx::regular, 15, "Закрыть") + 60;
    float cxp = right - cw;
    gfx::rrect(cxp, by, cw, 36, 18, col::SURFACE2);
    gfx::button(back_icon, cxp + 12, by + 8, 20);
    gfx::text(gfx::regular, cxp + 40, by + 23, 15, col::TEXT, "Закрыть");
    hits.push_back({cxp, by, cw, 36, [this]() { modal.on = false; }});
}

void App::draw_toast()
{
    if (now_ms() >= toast_until)
        return;
    float left = (toast_until - now_ms()) / 300.0f;
    float a = std::min(1.0f, left);
    std::string s = gfx::ellipsize(gfx::regular, 15, toast, 760);
    int w = gfx::width(gfx::regular, 15, s) + 40;
    float x = (W - w) / 2.0f, y = FOOTER_Y - 56;
    gfx::rrect(x, y, w, 40, 20, gfx::alpha(col::SURFACE2, a));
    gfx::text(gfx::regular, x + 20, y + 26, 15, gfx::alpha(col::TEXT, a), s);
}

void App::draw()
{
    hits.clear();
    gfx::background();
    if (screen == SCR_PRELOAD) {
        draw_preload();
        if (modal.on) {
            hits.clear();
            draw_modal();
        }
        draw_toast();
        return;
    }
    if (screen == SCR_BROWSE) {
        if (tab == TAB_DOWNLOADS)
            draw_downloads();
        else if (tab == TAB_UPDATES)
            draw_updates();
        else
            draw_grid();
        draw_header();
        if (filters_open) {
            hits.clear();
            draw_filters();
        }
    } else if (screen == SCR_DETAIL && viewer) {
        draw_viewer();
        draw_toast();
        return;
    } else if (screen == SCR_DETAIL) {
        draw_detail();
    } else if (screen == SCR_BACKUPS) {
        draw_backups();
    } else {
        draw_settings();
    }
    draw_footer();
    if (modal.on) {
        hits.clear();
        draw_modal();
    }
    draw_toast();
}

void App::run()
{
    int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
    if (enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
        ok_btn = SCE_CTRL_CIRCLE;
        back_btn = SCE_CTRL_CROSS;
        ok_icon = BTN_CIRCLE;
        back_icon = BTN_CROSS;
    }
    settings.load();
    installed.load();
    if (catalog.load_cache())
        loaded_once = true;
    if (catalog.media.empty())
        catalog.media = settings.base() + "media/";
    catalog.load_pkgj();
    catalog.apply_covers();
    catalog.load_updates();
    catalog.prepare();
    load_views();
    load_prefs();
    apply_speed();
    rebuild();
    if (fs::exists(DAEMON_PATH)) {
        std::string a, b;
        if (fs::read("app0:vpkgj_daemon.suprx", a) && fs::read(DAEMON_PATH, b) && a != b)
            fs::write(DAEMON_PATH, a);
    }
    images.start();
    tasks.start(&installed);
    start_refresh(false);

    for (;;) {
        apply_refresh();
        check_reboot();
        poll_bg();
        images.set_busy(tasks.active() > 0);
        if (ime::active()) {
            std::string text;
            int r = ime::poll(text);
            if (r == 1)
                ime_done(text);
            else if (r == -1)
                ime_purpose = IME_NONE;
        } else {
            input();
        }
        if (tasks.active())
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        static uint64_t last_refresh = 0;
        if (now_ms() - last_refresh > 2000) {
            if (screen == SCR_DETAIL)
                check_deps();
            last_refresh = now_ms();
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        draw();
        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        images.tick();
    }
}

int main()
{
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    SceAppUtilInitParam ip;
    SceAppUtilBootParam bp;
    memset(&ip, 0, sizeof(ip));
    memset(&bp, 0, sizeof(bp));
    sceAppUtilInit(&ip, &bp);
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    fs::mkdirs(APP_DIR);
    if (fs::size(APP_DIR + "/log.txt") > 256 * 1024)
        sceIoRemove((APP_DIR + "/log.txt").c_str());
    log_line("VPKGJ %s start", VPKGJ_VERSION);

    vita2d_init_advanced(4 * 1024 * 1024);
    vita2d_set_clear_color(col::BG);
    vita2d_set_vblank_wait(1);
    gfx::init();
    net::init();
    promoter_init();

    static App app;
    app.run();

    promoter_fini();
    net::fini();
    gfx::fini();
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
