#include "voice_clipboard.h"

#include <cstring>

namespace VoiceInput
{
bool CopyUnicodeTextToClipboard(HWND owner, const std::wstring &text)
{
    if (text.empty())
        return false;

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
        return false;

    void *destination = GlobalLock(memory);
    if (!destination)
    {
        GlobalFree(memory);
        return false;
    }
    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);

    if (!OpenClipboard(owner))
    {
        GlobalFree(memory);
        return false;
    }
    if (!EmptyClipboard())
    {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }
    if (!SetClipboardData(CF_UNICODETEXT, memory))
    {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}
} // namespace VoiceInput
