#include "catalog.hpp"
#include "gfx.hpp"
#include "images.hpp"
#include "ime.hpp"
#include "net.hpp"
#include "promote.hpp"
#include "tasks.hpp"
#include "util.hpp"

#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>

#include <psp2/io/fcntl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>

#define VPKGJ_VERSION "1.0"

int _newlib_heap_size_user = 160 * 1024 * 1024;

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
static const int TAB_DOWNLOADS = CAT_COUNT;
static const int TAB_COUNT = CAT_COUNT + 1;

enum Screen { SCR_BROWSE, SCR_DETAIL, SCR_SETTINGS };
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
};

struct App {
    Settings settings;
    Catalog catalog;
    Installed installed;
    Tasks tasks;
    Images images;

    Screen screen = SCR_BROWSE;
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

    int settings_sel = 0;
    int dl_sel = 0;

    float fx = 0, fy = 0, fw = 0, fh = 0;
    bool focus_init = false;

    Modal modal;
    std::string toast;
    uint64_t toast_until = 0;
    ImePurpose ime_purpose = IME_NONE;

    std::vector<Hit> hits;

    std::mutex refresh_m;
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
    std::string icon_url(const Entry& e);
    std::vector<Entry>& list(int c) { return catalog.items[c]; }
    const Entry* selected();
    void open_detail(const Entry& e);
    void primary_action();
    void ask_install(const Entry& e, bool reinstall);
    void launch(const Entry& e);
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

void App::rebuild()
{
    std::string q = lower(trim(query));
    for (int c = 0; c < CAT_COUNT; c++) {
        filtered[c].clear();
        for (int i = 0; i < (int)catalog.items[c].size(); i++)
            if (q.empty() || catalog.items[c][i].search.find(q) != std::string::npos)
                filtered[c].push_back(i);
        if (sel[c] >= (int)filtered[c].size())
            sel[c] = std::max(0, (int)filtered[c].size() - 1);
    }
}

std::string App::icon_url(const Entry& e)
{
    if (!e.icon.empty())
        return catalog.media_url(e.icon);
    if (e.cat == CAT_GAMES && !e.dlc && installed.status(e) != ST_NONE)
        return "ux0:app/" + e.id + "/sce_sys/icon0.png";
    return "";
}

const Entry* App::selected()
{
    if (tab >= CAT_COUNT || filtered[tab].empty())
        return nullptr;
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
    c->load_pkgj();
    {
        std::lock_guard<std::mutex> lock(a->refresh_m);
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
    SceUID th = sceKernelCreateThread("vpkgj_refresh", refresh_thread, 0x10000100, 128 * 1024, 0, 0, nullptr);
    App* self = this;
    sceKernelStartThread(th, sizeof(self), &self);
}

void App::apply_refresh()
{
    if (refreshing != 2)
        return;
    refreshing = 0;
    std::lock_guard<std::mutex> lock(refresh_m);
    if (fresh) {
        bool changed = fresh->version != catalog.version;
        catalog = std::move(*fresh);
        fresh.reset();
        loaded_once = loaded_once || refresh_err.empty();
        rebuild();
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
    shot = 0;
    prev_shot = -1;
    shot_time = now_ms();
    shot_auto = now_ms() + 6000;
    desc_scroll = desc_target = 0;
    std::string d = e.description.empty() ? "Описание пока не добавлено." : e.description;
    desc_lines = gfx::wrap(gfx::regular, 15, d, W - 64);
    screen = SCR_DETAIL;
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
    modal.yes = reinstall ? "Переустановить" : "Установить";
    Entry copy = e;
    modal.action = [this, copy]() {
        tasks.add(copy);
        show_toast("Добавлено в загрузки");
    };
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
    Status st = installed.status(e);
    if (st == ST_INSTALLED && launchable(e)) {
        launch(e);
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
            if (modal.action)
                modal.action();
        } else if (pressed & back_btn) {
            modal.on = false;
        }
        return;
    }

    if (screen == SCR_BROWSE) {
        if (pressed & SCE_CTRL_LTRIGGER)
            set_tab(tab - 1);
        if (pressed & SCE_CTRL_RTRIGGER)
            set_tab(tab + 1);
        if (pressed & (SCE_CTRL_START | SCE_CTRL_SELECT)) {
            screen = SCR_SETTINGS;
            return;
        }
        if (tab == TAB_DOWNLOADS) {
            auto list = tasks.list();
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
                    tasks.add(t->entry);
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
                open_detail(*e);
        }
        return;
    }

    if (screen == SCR_DETAIL) {
        int n = (int)detail.shots.size();
        if (n > 1 && (act & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {
            prev_shot = shot;
            shot = (shot + ((act & SCE_CTRL_RIGHT) ? 1 : n - 1)) % n;
            shot_time = now_ms();
            shot_auto = now_ms() + 8000;
        }
        if (act & (SCE_CTRL_DOWN | RS_DOWN))
            desc_target += 44;
        if (act & (SCE_CTRL_UP | RS_UP))
            desc_target -= 44;
        if (pressed & back_btn)
            screen = SCR_BROWSE;
        if (pressed & ok_btn)
            primary_action();
        if ((pressed & SCE_CTRL_SQUARE) && installed.status(detail) != ST_NONE) {
            auto t = tasks.find(detail);
            if (!t || (t->state != TS_RUNNING && t->state != TS_QUEUED))
                ask_install(detail, true);
        }
        return;
    }

    if (screen == SCR_SETTINGS) {
        if (act & SCE_CTRL_UP)
            settings_sel = std::max(0, settings_sel - 1);
        if (act & SCE_CTRL_DOWN)
            settings_sel = std::min(4, settings_sel + 1);
        if (pressed & (back_btn | SCE_CTRL_START | SCE_CTRL_SELECT)) {
            screen = SCR_BROWSE;
            return;
        }
        if (pressed & ok_btn) {
            switch (settings_sel) {
            case 0:
                ime_purpose = IME_REPO;
                ime::open("Репозиторий каталога (user/repo)", settings.repo, 100);
                break;
            case 1:
                ime_purpose = IME_BRANCH;
                ime::open("Ветка", settings.branch, 60);
                break;
            case 2:
                start_refresh(true);
                break;
            case 3:
                images.clear_disk();
                show_toast("Кеш картинок очищен");
                break;
            case 4:
                if (tasks.active()) {
                    show_toast("Дождись окончания загрузок");
                } else {
                    fs::rmtree(APP_DIR + "/dl");
                    fs::rmtree("ux0:temp/vpkgj");
                    fs::mkdirs(APP_DIR + "/dl");
                    fs::mkdirs("ux0:temp/vpkgj");
                    show_toast("Временные файлы удалены");
                }
                break;
            }
        }
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
    gfx::rect(0, 0, W, HEADER_H, col::BG);
    gfx::text(gfx::bold, 28, 41, 24, col::TEXT, "VPKGJ");
    gfx::circle(108, 33, 3.5f, col::ACCENT);
    float x = 132;
    gfx::button(BTN_L, x, 23, 20);
    x += 34;
    for (int t = 0; t < TAB_COUNT; t++) {
        std::string label = t == TAB_DOWNLOADS ? "Загрузки" : category_title((Category)t);
        int n = t == TAB_DOWNLOADS ? tasks.active() : 0;
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
    gfx::vgrad(0, HEADER_H, W, 14, col::BG, gfx::alpha(col::BG, 0));
}

void App::draw_card(const Entry& e, float x, float y)
{
    gfx::rrect(x, y, CARD_W, CARD_H, 14, col::SURFACE);
    float a = 0;
    vita2d_texture* img = nullptr;
    vita2d_texture* icon = images.get(icon_url(e), &a);
    float ia = a;
    if (!e.shots.empty())
        img = images.get(catalog.media_url(e.shots[0]), &a);
    if (img) {
        gfx::cover(img, x, y, CARD_W, CARD_IMG_H, a);
        if (icon) {
            gfx::rrect(x + 10, y + CARD_IMG_H - 50, 44, 44, 8, gfx::alpha(col::BG, 0.6f * ia));
            gfx::fit(icon, x + 12, y + CARD_IMG_H - 48, 40, 40, ia);
        }
    } else {
        gfx::vgrad(x, y, CARD_W, CARD_IMG_H, col::SURFACE2, col::SURFACE);
        if (icon) {
            gfx::fit(icon, x + CARD_W / 2 - 40, y + CARD_IMG_H / 2 - 40, 80, 80, ia);
        } else {
            std::string first = e.name.substr(0, (unsigned char)e.name[0] >= 0xc0 ? 2 : 1);
            gfx::text_center(gfx::bold, x + CARD_W / 2, y + CARD_IMG_H / 2 + 16, 44, col::LINE, first);
        }
    }
    gfx::corners(x, y, CARD_W, CARD_H, 14, col::BG, true, false);
    gfx::vgrad(x, y + CARD_IMG_H - 1, CARD_W, 1, col::LINE, col::LINE);

    float bx = x + CARD_W - 8;
    auto t = tasks.find(e);
    Status st = installed.status(e);
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
    }
    if (!e.lang.empty()) {
        std::string l = e.lang;
        for (auto& c : l)
            c = toupper(c);
        int w = gfx::width(gfx::bold, 11, l) + 14;
        gfx::rrect(x + 8, y + 8, w, 20, 10, gfx::alpha(col::BG, 0.75f));
        gfx::text(gfx::bold, x + 15, y + 22, 11, col::TEXT, l);
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
        float cy = 280;
        if (refreshing == 1 && !loaded_once) {
            gfx::spinner(W / 2, cy - 20, 12, col::ACCENT);
            gfx::text_center(gfx::regular, W / 2, cy + 22, 16, col::MUTED, "Загружаю каталог...");
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
    for (int i = 0; i < (int)f.size(); i++) {
        float x = GRID_X + (i % COLS) * (CARD_W + GAP_X);
        float y = top + (i / COLS) * (CARD_H + GAP_Y);
        if (y + CARD_H < HEADER_H - 20 || y > FOOTER_Y + 10)
            continue;
        const Entry& e = catalog.items[tab][f[i]];
        draw_card(e, x, y);
        int idx = i;
        hits.push_back({x, y, (float)CARD_W, (float)CARD_H, [this, idx]() {
                            if (sel[tab] == idx) {
                                const Entry* e = selected();
                                if (e)
                                    open_detail(*e);
                            } else {
                                sel[tab] = idx;
                                ensure_visible();
                            }
                        }});
    }
    vita2d_disable_clipping();
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
    const float HERO = 300;
    int n = (int)e.shots.size();
    if (n > 1 && now_ms() > shot_auto && !touching) {
        prev_shot = shot;
        shot = (shot + 1) % n;
        shot_time = now_ms();
        shot_auto = now_ms() + 6000;
    }
    float fade = std::min(1.0f, (now_ms() - shot_time) / 350.0f);
    float ia = 0;
    vita2d_texture* icon = images.get(icon_url(e), &ia);
    if (n) {
        if (prev_shot >= 0 && fade < 1) {
            vita2d_texture* p = images.get(catalog.media_url(e.shots[prev_shot]));
            if (p)
                gfx::cover(p, 0, 0, W, HERO, 1);
        }
        float a = 0;
        vita2d_texture* cur = images.get(catalog.media_url(e.shots[shot]), &a);
        if (cur)
            gfx::cover(cur, 0, 0, W, HERO, std::min(a, prev_shot >= 0 ? fade : 1.0f));
        else
            gfx::spinner(W / 2, HERO / 2, 12, col::MUTED);
        for (int i = 1; i < 3 && i < n; i++)
            images.get(catalog.media_url(e.shots[(shot + i) % n]));
    } else {
        gfx::vgrad(0, 0, W, HERO, col::SURFACE2, col::BG);
        if (icon)
            gfx::fit(icon, W / 2 - 80, 50, 160, 160, ia * 0.35f);
    }
    gfx::vgrad(0, 0, W, 90, gfx::alpha(col::BG, 0.75f), gfx::alpha(col::BG, 0));
    gfx::vgrad(0, HERO - 150, W, 150, gfx::alpha(col::BG, 0), col::BG);
    gfx::rect(0, HERO, W, H - HERO, col::BG);

    gfx::rrect(20, 16, 98, 34, 17, gfx::alpha(col::BG, 0.7f));
    gfx::button(back_icon, 28, 23, 20);
    gfx::text(gfx::regular, 56, 38, 14, col::TEXT, "Назад");
    hits.push_back({20, 16, 98, 34, [this]() { screen = SCR_BROWSE; }});

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
        hits.push_back({W * 2 / 3.0f, 60, W / 3.0f, 160, [this, n]() {
                            prev_shot = shot;
                            shot = (shot + 1) % n;
                            shot_time = now_ms();
                            shot_auto = now_ms() + 8000;
                        }});
    }

    float iy = HERO - 96;
    gfx::rrect(28, iy - 4, 104, 104, 20, col::BG);
    if (icon) {
        gfx::fit(icon, 32, iy, 96, 96, ia);
        gfx::corners(32, iy, 96, 96, 16, col::BG);
    } else {
        gfx::rrect(32, iy, 96, 96, 16, col::SURFACE2);
        std::string first = e.name.substr(0, (unsigned char)e.name[0] >= 0xc0 ? 2 : 1);
        gfx::text_center(gfx::bold, 80, iy + 64, 40, col::MUTED, first);
    }

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
    Status st = installed.status(e);
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
            label = "Обновить";
        else
            label = e.size ? "Установить · " + fmt_size(e.size) : "Установить";
        if (st == ST_UPDATE)
            bg = col::ORANGE;
        gfx::rrect(bx, by, bw, 50, 25, bg);
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
    if (e.cat == CAT_PORTS || e.cat == CAT_HOMEBREW)
        chip(e.data.empty() ? "Без data" : "vpk + data", col::TEXT);
    if (e.cat == CAT_TRANSLATIONS && lower(e.install).find("plugin") != std::string::npos)
        chip("Нужен плагин, ставится сам", col::TEXT);
    if (e.cat == CAT_PLUGINS && !e.tai_section.empty())
        chip(e.tai_section, col::TEXT);
    if (st == ST_INSTALLED)
        chip("Установлено", col::GREEN);
    if (st == ST_UPDATE)
        chip("Есть обновление", col::ORANGE);
    if (t && t->state == TS_FAILED)
        chip(gfx::ellipsize(gfx::regular, 13, "Ошибка: " + t->get_error(), 520), col::RED);
    if (t && t->state == TS_DONE && !t->note.empty())
        chip(t->note, col::ORANGE);

    float top = HERO + 54, bottom = FOOTER_Y - 6;
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
        gfx::text(gfx::regular, 32, y, 15, RGBA8(205, 209, 218, 255), desc_lines[i]);
    }
    vita2d_disable_clipping();
    if (maxs > 0) {
        float vh = bottom - top;
        float bh = vh * vh / (desc_lines.size() * lh);
        float byy = top + (vh - bh) * (desc_scroll / maxs);
        gfx::rrect(W - 12, byy, 4, bh, 2, col::LINE);
    }
}

void App::draw_downloads()
{
    auto list = tasks.list();
    if (list.empty()) {
        gfx::text_center(gfx::bold, W / 2, 270, 20, col::TEXT, "Загрузок нет");
        gfx::text_center(gfx::regular, W / 2, 298, 15, col::MUTED, "Выбери игру или порт и нажми Установить");
        gfx::text_center(gfx::regular, W / 2, 330, 14, col::MUTED, "Свободно на ux0: " + fmt_size(fs::free_space("ux0:")));
        return;
    }
    if (dl_sel >= (int)list.size())
        dl_sel = (int)list.size() - 1;
    float y = GRID_TOP + 4;
    gfx::text(gfx::regular, 32, y + 12, 14, col::MUTED, "Свободно на ux0: " + fmt_size(fs::free_space("ux0:")));
    y += 28;
    int first = std::max(0, dl_sel - 4);
    uint64_t now = now_ms();
    for (int i = first; i < (int)list.size() && y < FOOTER_Y - 70; i++) {
        auto& t = list[i];
        bool on = i == dl_sel;
        gfx::rrect(28, y, W - 56, 72, 14, on ? col::SURFACE2 : col::SURFACE);
        if (on)
            gfx::rframe(28, y, W - 56, 72, 14, 2, col::ACCENT);
        float a = 0;
        vita2d_texture* icon = images.get(icon_url(t->entry), &a);
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
            status = t->note.empty() ? "Установлено" : "Установлено. " + t->note;
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
    gfx::text_right(gfx::regular, W - 32, 44, 14, col::MUTED, "VPKGJ " VPKGJ_VERSION);
    struct Row {
        std::string title, value;
    };
    std::vector<Row> rows = {
        {"Репозиторий каталога", settings.repo},
        {"Ветка", settings.branch},
        {"Обновить каталог", catalog.version.empty() ? "" : "версия " + catalog.version},
        {"Очистить кеш картинок", ""},
        {"Удалить временные файлы загрузок", ""},
    };
    std::string pc = pkgj_config_path();
    float y = 76;
    for (int i = 0; i < (int)rows.size(); i++) {
        bool on = i == settings_sel;
        gfx::rrect(28, y, W - 56, 52, 12, on ? col::SURFACE2 : col::SURFACE);
        if (on)
            gfx::rframe(28, y, W - 56, 52, 12, 2, col::ACCENT);
        gfx::text(gfx::regular, 48, y + 32, 16, col::TEXT, rows[i].title);
        gfx::text_right(gfx::regular, W - 48, y + 32, 15, col::MUTED, rows[i].value);
        int idx = i;
        hits.push_back({28, y, (float)(W - 56), 52, [this, idx]() { settings_sel = idx; }});
        y += 60;
    }
    y += 8;
    std::vector<std::string> about = {
        "Свободно на ux0: " + fmt_size(fs::free_space("ux0:")) + " · плагины: " + tai_dir() + "/config.txt",
        pc.empty() ? "Списки pkgj: не найден ux0:pkgj/config.txt"
                   : "Списки pkgj из " + pc + ": игр " + std::to_string(catalog.pkgj_games) + ", DLC " + std::to_string(catalog.pkgj_dlcs),
        "Каталог: github.com/" + settings.repo + " · приложение: github.com/Risaro/vpkgj",
        "Лицензия GPL-3.0. Установка PKG по мотивам pkgj, установка vpk по мотивам VitaShell и VitaDB Downloader.",
    };
    for (auto& s : about) {
        gfx::text(gfx::regular, 32, y + 16, 13, col::MUTED, gfx::ellipsize(gfx::regular, 13, s, W - 64));
        y += 22;
    }
}

void App::draw_footer()
{
    gfx::vgrad(0, FOOTER_Y - 18, W, 18, gfx::alpha(col::BG, 0), col::BG);
    gfx::rect(0, FOOTER_Y, W, H - FOOTER_Y, col::BG);
    float x = 28, y = FOOTER_Y + 8;
    if (modal.on) {
        x = gfx::hint(x, y, ok_icon, modal.yes);
        gfx::hint(x, y, back_icon, "Закрыть");
        return;
    }
    if (screen == SCR_BROWSE && tab < CAT_COUNT) {
        x = gfx::hint(x, y, ok_icon, "Открыть");
        x = gfx::hint(x, y, BTN_TRIANGLE, "Поиск");
        x = gfx::hint(x, y, BTN_SQUARE, "Обновить");
        if (!query.empty())
            x = gfx::hint(x, y, back_icon, "Сбросить поиск");
        gfx::hint(x, y, BTN_START, "Настройки");
    } else if (screen == SCR_BROWSE) {
        x = gfx::hint(x, y, ok_icon, "Отменить или повторить");
        x = gfx::hint(x, y, BTN_TRIANGLE, "Очистить готовые");
        gfx::hint(x, y, BTN_START, "Настройки");
    } else if (screen == SCR_DETAIL) {
        Status st = installed.status(detail);
        auto t = tasks.find(detail);
        bool busy = t && (t->state == TS_RUNNING || t->state == TS_QUEUED);
        std::string ok = busy ? "Отменить" : st == ST_INSTALLED && launchable(detail) ? "Запустить" : "Установить";
        x = gfx::hint(x, y, ok_icon, ok);
        if (st != ST_NONE && !busy)
            x = gfx::hint(x, y, BTN_SQUARE, "Переустановить");
        x = gfx::hint(x, y, back_icon, "Назад");
        if (detail.shots.size() > 1)
            gfx::text(gfx::regular, x, y + 15, 14, col::MUTED, "← → скриншоты");
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
                        if (modal.action)
                            modal.action();
                    }});
    float cw = gfx::width(gfx::regular, 15, "Закрыть") + 60;
    float cxp = x + w - 28 - bw - 12 - cw;
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
    if (screen == SCR_BROWSE) {
        if (tab == TAB_DOWNLOADS)
            draw_downloads();
        else
            draw_grid();
        draw_header();
    } else if (screen == SCR_DETAIL) {
        draw_detail();
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
    rebuild();
    images.start();
    tasks.start(&installed);
    start_refresh(false);

    for (;;) {
        apply_refresh();
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
            installed.refresh();
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
