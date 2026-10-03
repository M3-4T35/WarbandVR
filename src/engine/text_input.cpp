#include "engine/text_input.h"
#include "util/log.h"
#include "util/pattern.h"

#include <atomic>
#include <cstdint>

#define LOG_ENGINE(...) ::vrlog::Write("[engine]", __VA_ARGS__)

namespace engine {
namespace {

// rglTextBoxWidget::loseFocus (et le destructeur) :
//   mov byte ptr [g_windowManager+0x32A0], 0   ; drapeau « saisie de texte »
//   mov qword ptr [g_windowManager+0x18], 0    ; zone de texte qui a le focus
//   mov rax, [rcx] ; mov edx, 10000h
constexpr const char* kTextFlagSig = "C6 05 ?? ?? ?? ?? 00 48 C7 05 ?? ?? ?? ?? 00 00 00 00 48 8B 01 BA 00 00 01 00";

std::atomic<const volatile uint8_t*> g_textFlag{nullptr};

} // namespace

bool InitTextInput()
{
    // C6 05 disp32 imm8 : instruction de 7 octets, déplacement à +2
    auto* flag = static_cast<const volatile uint8_t*>(pattern::FindGlobal("drapeau de saisie de texte", kTextFlagSig, 2, 7));
    if (!flag) {
        LOG_ENGINE("clavier virtuel automatique desactive (signature manquante)");
        return false;
    }
    g_textFlag = flag;
    return true;
}

bool TextInputActive()
{
    const volatile uint8_t* flag = g_textFlag;
    return flag && *flag != 0;
}

} // namespace engine
