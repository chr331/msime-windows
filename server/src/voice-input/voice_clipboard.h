#pragma once

#include <windows.h>
#include <string>

namespace VoiceInput
{
// Allocates and fills the complete UTF-16 payload before opening the shared clipboard. A failure to
// allocate, open, empty or set the data leaves any existing clipboard content untouched whenever
// the platform permits it, and never clears the clipboard merely because allocation failed.
bool CopyUnicodeTextToClipboard(HWND owner, const std::wstring &text);
} // namespace VoiceInput
