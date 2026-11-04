#pragma once

#include <vita2d.h>

#include <string>
#include <vector>

namespace col {
constexpr unsigned BG = RGBA8(14, 12, 24, 255);
constexpr unsigned SURFACE = RGBA8(27, 24, 42, 255);
constexpr unsigned SURFACE2 = RGBA8(41, 36, 62, 255);
constexpr unsigned LINE = RGBA8(56, 50, 82, 255);
constexpr unsigned TEXT = RGBA8(244, 242, 252, 255);
constexpr unsigned MUTED = RGBA8(150, 144, 176, 255);
constexpr unsigned ACCENT = RGBA8(139, 92, 246, 255);
constexpr unsigned ACCENT2 = RGBA8(244, 114, 182, 255);
constexpr unsigned ACCENT_DIM = RGBA8(139, 92, 246, 60);
constexpr unsigned GREEN = RGBA8(52, 211, 153, 255);
constexpr unsigned ORANGE = RGBA8(251, 170, 80, 255);
constexpr unsigned RED = RGBA8(248, 113, 113, 255);
}

enum Btn { BTN_CROSS, BTN_CIRCLE, BTN_TRIANGLE, BTN_SQUARE, BTN_L, BTN_R, BTN_START, BTN_SELECT };

namespace gfx {

enum Font { regular, bold };

bool init();
void fini();

unsigned alpha(unsigned color, float a);
void rect(float x, float y, float w, float h, unsigned c);
void rrect(float x, float y, float w, float h, float r, unsigned c);
void rframe(float x, float y, float w, float h, float r, float t, unsigned c);
void corners(float x, float y, float w, float h, float r, unsigned c, bool top = true, bool bottom = true);
void vgrad(float x, float y, float w, float h, unsigned top, unsigned bottom);
void hgrad(float x, float y, float w, float h, unsigned left, unsigned right);
void line(float x0, float y0, float x1, float y1, float t, unsigned c);
void circle(float x, float y, float r, unsigned c);

void text(Font f, float x, float y, int size, unsigned c, const std::string& s);
void text_right(Font f, float x, float y, int size, unsigned c, const std::string& s);
void text_center(Font f, float x, float y, int size, unsigned c, const std::string& s);
int width(Font f, int size, const std::string& s);
std::string ellipsize(Font f, int size, const std::string& s, int maxw);
std::vector<std::string> wrap(Font f, int size, const std::string& s, int maxw);

void cover(vita2d_texture* t, float x, float y, float w, float h, float a = 1);
void fit(vita2d_texture* t, float x, float y, float w, float h, float a = 1);

void cover_round(vita2d_texture* t, float x, float y, float w, float h, float r, bool top, bool bottom, float a = 1);
void fit_round(vita2d_texture* t, float x, float y, float w, float h, float r, float a = 1);
void grad_round(float x, float y, float w, float h, float r, bool top, bool bottom, unsigned c1, unsigned c2);
void background();
void placeholder(float x, float y, float w, float h, const std::string& name, int size);
void placeholder_round(float x, float y, float w, float h, float r, const std::string& name, int size);
void button(Btn b, float x, float y, float size);
float hint(float x, float y, Btn b, const std::string& label);
void spinner(float x, float y, float r, unsigned c);
void mascot(float x, float y, float size, float a);
void logo(float x, float y, float size);

}
