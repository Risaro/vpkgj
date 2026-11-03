#include "gfx.hpp"

#include "util.hpp"

#include <cmath>
#include <map>

namespace gfx {

static std::string font_data[2];
static std::map<int, vita2d_font*> fonts;

bool init()
{
    fs::read("app0:assets/Inter-Regular.ttf", font_data[regular]);
    fs::read("app0:assets/Inter-SemiBold.ttf", font_data[bold]);
    if (font_data[bold].empty())
        font_data[bold] = font_data[regular];
    return !font_data[regular].empty();
}

void fini()
{
    for (auto& kv : fonts)
        vita2d_free_font(kv.second);
    fonts.clear();
}

static vita2d_font* get(Font f, int size)
{
    int key = f * 1000 + size;
    auto it = fonts.find(key);
    if (it != fonts.end())
        return it->second;
    vita2d_font* v = vita2d_load_font_mem(font_data[f].data(), font_data[f].size());
    fonts[key] = v;
    return v;
}

unsigned alpha(unsigned c, float a)
{
    if (a < 0)
        a = 0;
    if (a > 1)
        a = 1;
    unsigned ca = (c >> 24) & 0xff;
    return (c & 0x00ffffff) | ((unsigned)(ca * a) << 24);
}

static vita2d_color_vertex* verts(size_t n)
{
    return (vita2d_color_vertex*)vita2d_pool_memalign(n * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
}

static void set(vita2d_color_vertex& v, float x, float y, unsigned c)
{
    v.x = x;
    v.y = y;
    v.z = 0.5f;
    v.color = c;
}

void rect(float x, float y, float w, float h, unsigned c)
{
    if (w <= 0 || h <= 0)
        return;
    vita2d_draw_rectangle(x, y, w, h, c);
}

static void fan(float cx, float cy, float px, float py, float r, float a0, unsigned c, bool inverse)
{
    const int seg = 10;
    vita2d_color_vertex* v = verts(seg + 2);
    if (!v)
        return;
    if (inverse)
        set(v[0], px, py, c);
    else
        set(v[0], cx, cy, c);
    for (int i = 0; i <= seg; i++) {
        float a = a0 + (float)M_PI_2 * i / seg;
        set(v[i + 1], cx + cosf(a) * r, cy + sinf(a) * r, c);
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, seg + 2);
}

void rrect(float x, float y, float w, float h, float r, unsigned c)
{
    if (w <= 0 || h <= 0)
        return;
    r = fminf(r, fminf(w, h) / 2);
    if (r < 1) {
        rect(x, y, w, h, c);
        return;
    }
    rect(x + r, y, w - 2 * r, h, c);
    rect(x, y + r, r, h - 2 * r, c);
    rect(x + w - r, y + r, r, h - 2 * r, c);
    fan(x + r, y + r, 0, 0, r, (float)M_PI, c, false);
    fan(x + w - r, y + r, 0, 0, r, (float)M_PI * 1.5f, c, false);
    fan(x + w - r, y + h - r, 0, 0, r, 0, c, false);
    fan(x + r, y + h - r, 0, 0, r, (float)M_PI_2, c, false);
}

void rframe(float x, float y, float w, float h, float r, float t, unsigned c)
{
    const int seg = 8;
    float cx[4] = {x + w - r, x + r, x + r, x + w - r};
    float cy[4] = {y + h - r, y + h - r, y + r, y + r};
    int n = 4 * (seg + 1) + 1;
    vita2d_color_vertex* v = verts(n * 2);
    if (!v)
        return;
    int k = 0;
    for (int q = 0; q < 4; q++) {
        for (int i = 0; i <= seg; i++) {
            float a = (float)M_PI_2 * q + (float)M_PI_2 * i / seg;
            float ca = cosf(a), sa = sinf(a);
            set(v[k++], cx[q] + ca * r, cy[q] + sa * r, c);
            set(v[k++], cx[q] + ca * (r - t), cy[q] + sa * (r - t), c);
        }
    }
    v[k++] = v[0];
    v[k++] = v[1];
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, k);
}

void corners(float x, float y, float w, float h, float r, unsigned c, bool top, bool bottom)
{
    if (top) {
        fan(x + r, y + r, x, y, r, (float)M_PI, c, true);
        fan(x + w - r, y + r, x + w, y, r, (float)M_PI * 1.5f, c, true);
    }
    if (bottom) {
        fan(x + w - r, y + h - r, x + w, y + h, r, 0, c, true);
        fan(x + r, y + h - r, x, y + h, r, (float)M_PI_2, c, true);
    }
}

void vgrad(float x, float y, float w, float h, unsigned top, unsigned bottom)
{
    vita2d_color_vertex* v = verts(4);
    if (!v)
        return;
    set(v[0], x, y, top);
    set(v[1], x + w, y, top);
    set(v[2], x, y + h, bottom);
    set(v[3], x + w, y + h, bottom);
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void hgrad(float x, float y, float w, float h, unsigned left, unsigned right)
{
    vita2d_color_vertex* v = verts(4);
    if (!v)
        return;
    set(v[0], x, y, left);
    set(v[1], x + w, y, right);
    set(v[2], x, y + h, left);
    set(v[3], x + w, y + h, right);
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void line(float x0, float y0, float x1, float y1, float t, unsigned c)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0)
        return;
    float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    vita2d_color_vertex* v = verts(4);
    if (!v)
        return;
    set(v[0], x0 + nx, y0 + ny, c);
    set(v[1], x1 + nx, y1 + ny, c);
    set(v[2], x0 - nx, y0 - ny, c);
    set(v[3], x1 - nx, y1 - ny, c);
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void circle(float x, float y, float r, unsigned c)
{
    const int seg = 28;
    vita2d_color_vertex* v = verts(seg + 2);
    if (!v)
        return;
    set(v[0], x, y, c);
    for (int i = 0; i <= seg; i++) {
        float a = 2 * (float)M_PI * i / seg;
        set(v[i + 1], x + cosf(a) * r, y + sinf(a) * r, c);
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, seg + 2);
}

void text(Font f, float x, float y, int size, unsigned c, const std::string& s)
{
    vita2d_font* v = get(f, size);
    if (v && !s.empty())
        vita2d_font_draw_text(v, (int)x, (int)y, c, size, s.c_str());
}

int width(Font f, int size, const std::string& s)
{
    vita2d_font* v = get(f, size);
    return !v || s.empty() ? 0 : vita2d_font_text_width(v, size, s.c_str());
}

void text_right(Font f, float x, float y, int size, unsigned c, const std::string& s)
{
    text(f, x - width(f, size, s), y, size, c, s);
}

void text_center(Font f, float x, float y, int size, unsigned c, const std::string& s)
{
    text(f, x - width(f, size, s) / 2.0f, y, size, c, s);
}

static size_t prev_cp(const std::string& s, size_t i)
{
    if (i == 0)
        return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80)
        i--;
    return i;
}

std::string ellipsize(Font f, int size, const std::string& s, int maxw)
{
    if (width(f, size, s) <= maxw)
        return s;
    std::string cut = s;
    while (!cut.empty()) {
        cut.erase(prev_cp(cut, cut.size()));
        std::string t = trim(cut) + "…";
        if (width(f, size, t) <= maxw)
            return t;
    }
    return "…";
}

std::vector<std::string> wrap(Font f, int size, const std::string& s, int maxw)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string cur;
        size_t i = 0;
        while (i <= para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            std::string test = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && width(f, size, test) > maxw) {
                out.push_back(cur);
                cur = word;
            } else {
                cur = test;
            }
            while (width(f, size, cur) > maxw && cur.size() > 1) {
                std::string head = cur;
                while (head.size() > 1 && width(f, size, head) > maxw)
                    head.erase(prev_cp(head, head.size()));
                out.push_back(head);
                cur = cur.substr(head.size());
            }
            if (sp == std::string::npos)
                break;
            i = sp + 1;
        }
        out.push_back(cur);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    while (!out.empty() && trim(out.back()).empty())
        out.pop_back();
    return out;
}

void cover(vita2d_texture* t, float x, float y, float w, float h, float a)
{
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
    if (tw <= 0 || th <= 0)
        return;
    float s = fmaxf(w / tw, h / th);
    float cw = w / s, ch = h / s;
    float cx = (tw - cw) / 2, cy = (th - ch) / 2;
    vita2d_draw_texture_tint_part_scale(t, x, y, cx, cy, cw, ch, s, s, RGBA8(255, 255, 255, (int)(255 * a)));
}

void fit(vita2d_texture* t, float x, float y, float w, float h, float a)
{
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
    if (tw <= 0 || th <= 0)
        return;
    float s = fminf(w / tw, h / th);
    vita2d_draw_texture_tint_scale(t, x + (w - tw * s) / 2, y + (h - th * s) / 2, s, s, RGBA8(255, 255, 255, (int)(255 * a)));
}

void button(Btn b, float x, float y, float size)
{
    float r = size / 2;
    float cx = x + r, cy = y + r;
    float t = fmaxf(1.6f, size / 11);
    float g = size * 0.24f;
    switch (b) {
    case BTN_CROSS: {
        unsigned c = RGBA8(124, 178, 232, 255);
        circle(cx, cy, r, col::SURFACE2);
        line(cx - g, cy - g, cx + g, cy + g, t, c);
        line(cx - g, cy + g, cx + g, cy - g, t, c);
        break;
    }
    case BTN_CIRCLE: {
        unsigned c = RGBA8(255, 102, 102, 255);
        circle(cx, cy, r, col::SURFACE2);
        circle(cx, cy, g + t / 2, c);
        circle(cx, cy, g - t / 2, col::SURFACE2);
        break;
    }
    case BTN_TRIANGLE: {
        unsigned c = RGBA8(64, 226, 160, 255);
        circle(cx, cy, r, col::SURFACE2);
        float h = g * 1.1f;
        line(cx, cy - h, cx + h, cy + h * 0.65f, t, c);
        line(cx + h, cy + h * 0.65f, cx - h, cy + h * 0.65f, t, c);
        line(cx - h, cy + h * 0.65f, cx, cy - h, t, c);
        break;
    }
    case BTN_SQUARE: {
        unsigned c = RGBA8(255, 105, 248, 255);
        circle(cx, cy, r, col::SURFACE2);
        line(cx - g, cy - g, cx + g, cy - g, t, c);
        line(cx + g, cy - g, cx + g, cy + g, t, c);
        line(cx + g, cy + g, cx - g, cy + g, t, c);
        line(cx - g, cy + g, cx - g, cy - g, t, c);
        break;
    }
    default: {
        const char* label = b == BTN_L ? "L" : b == BTN_R ? "R" : b == BTN_START ? "START" : "SELECT";
        int fs = b == BTN_L || b == BTN_R ? (int)(size * 0.6f) : (int)(size * 0.42f);
        float w = fmaxf(size * 1.3f, width(bold, fs, label) + size * 0.7f);
        rrect(x, y, w, size, r, col::SURFACE2);
        text_center(bold, x + w / 2, cy + fs * 0.36f, fs, col::TEXT, label);
        break;
    }
    }
}

float hint(float x, float y, Btn b, const std::string& label)
{
    const float size = 20;
    button(b, x, y, size);
    float bw = size;
    if (b == BTN_L || b == BTN_R || b == BTN_START || b == BTN_SELECT) {
        const char* l = b == BTN_L ? "L" : b == BTN_R ? "R" : b == BTN_START ? "START" : "SELECT";
        int fs = b == BTN_L || b == BTN_R ? (int)(size * 0.6f) : (int)(size * 0.42f);
        bw = fmaxf(size * 1.3f, width(bold, fs, l) + size * 0.7f);
    }
    text(regular, x + bw + 7, y + 15, 14, col::MUTED, label);
    return x + bw + 7 + width(regular, 14, label) + 20;
}

void spinner(float x, float y, float r, unsigned c)
{
    float t = (now_ms() % 1000) / 1000.0f * 2 * (float)M_PI;
    for (int i = 0; i < 8; i++) {
        float a = t + i * (float)M_PI / 4;
        circle(x + cosf(a) * r, y + sinf(a) * r, 2.2f, alpha(c, (i + 1) / 8.0f));
    }
}

}
