#include "ime.hpp"

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>

#include <cstring>

namespace ime {

static uint16_t title16[128];
static uint16_t initial16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static uint16_t input16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static bool running = false;

static void to16(const std::string& s, uint16_t* out, size_t max)
{
    size_t n = 0, i = 0;
    while (i < s.size() && n + 1 < max) {
        unsigned char c = s[i];
        uint32_t cp;
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c >> 5) == 6 && i + 1 < s.size()) {
            cp = ((c & 0x1f) << 6) | (s[i + 1] & 0x3f);
            i += 2;
        } else if ((c >> 4) == 14 && i + 2 < s.size()) {
            cp = ((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f);
            i += 3;
        } else {
            i += 1;
            continue;
        }
        out[n++] = (uint16_t)cp;
    }
    out[n] = 0;
}

static std::string to8(const uint16_t* in)
{
    std::string out;
    for (; *in; in++) {
        uint32_t cp = *in;
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xc0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3f));
        } else {
            out += (char)(0xe0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3f));
            out += (char)(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

bool open(const std::string& title, const std::string& initial, int max_len)
{
    if (running)
        return false;
    to16(title, title16, 128);
    to16(initial, initial16, SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1);
    memset(input16, 0, sizeof(input16));
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    p.title = title16;
    p.maxTextLength = max_len;
    p.initialText = initial16;
    p.inputTextBuffer = input16;
    if (sceImeDialogInit(&p) < 0)
        return false;
    running = true;
    return true;
}

bool active()
{
    return running;
}

int poll(std::string& out)
{
    if (!running)
        return 0;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0;
    SceImeDialogResult r;
    memset(&r, 0, sizeof(r));
    sceImeDialogGetResult(&r);
    sceImeDialogTerm();
    running = false;
    if (r.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        out = to8(input16);
        return 1;
    }
    return -1;
}

}
