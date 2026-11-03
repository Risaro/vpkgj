#pragma once

#include <vita2d.h>

#include <string>
#include <vector>

namespace col {
constexpr unsigned BG = RGBA8(13, 14, 18, 255);
constexpr unsigned SURFACE = RGBA8(24, 26, 33, 255);
constexpr unsigned SURFACE2 = RGBA8(36, 39, 49, 255);
constexpr unsigned LINE = RGBA8(48, 52, 64, 255);
constexpr unsigned TEXT = RGBA8(242, 243, 247, 255);
constexpr unsigned MUTED = RGBA8(140, 146, 160, 255);
constexpr unsigned ACCENT = RGBA8(76, 141, 255, 255);
constexpr unsigned ACCENT_DIM = RGBA8(76, 141, 255, 60);
constexpr unsigned GREEN = RGBA8(52, 199, 140, 255);
constexpr unsigned ORANGE = RGBA8(255, 159, 67, 255);
constexpr unsigned RED = RGBA8(255, 99, 99, 255);
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

void button(Btn b, float x, float y, float size);
float hint(float x, float y, Btn b, const std::string& label);
void spinner(float x, float y, float r, unsigned c);

}
