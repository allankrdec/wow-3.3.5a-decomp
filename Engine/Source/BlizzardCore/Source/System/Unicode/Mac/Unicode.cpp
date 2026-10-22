// macOS counterpart of Unicode/Win64/Unicode.cpp. Native names are UTF-8 already, so the conversion only turns
// the game's Windows separators into POSIX ones ("Data\\enUS" -> "Data/enUS").

#include <cstdint>
#include <cstring>
#include "Engine/Source/BlizzardCore/Source/System/Unicode/Unicode.h"

// Win64: 0x00433650
Blizzard::StringConversion_CodepageToUTF8::ToNative::ToNative(const char* str) : Blizzard::Memory::TempBuffer<char, 300>((str ? static_cast<uint32_t>(strlen(str)) : 0) + 1) {
    uint32_t length = this->m_count - 1;

    for (uint32_t i = 0; i < length; i++) {
        this->m_data[i] = str[i] == '\\' ? '/' : str[i];
    }

    this->m_data[length] = '\0';
}
