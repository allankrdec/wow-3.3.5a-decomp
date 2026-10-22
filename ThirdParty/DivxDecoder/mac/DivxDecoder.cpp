// DivxDecoder for the macOS build. Windows uses DivxDecoder.dll (Win32) or Tools/XvidDecoder (x64), neither of
// which exists for arm64 yet. Every call fails, so CSimpleMovieFrame skips the movie.
// TODO_MAC (E7): build Tools/XvidDecoder against an arm64 xvidcore.

#include "ThirdParty/DivxDecoder/DivxDecoder.h"

extern "C" {

int32_t DivxDecode(int32_t decoder, DivxDecodeFrame* frame, int32_t flags) {
    return 1;
}

int32_t InitializeDivxDecoder(int32_t decoder, int32_t width, int32_t height) {
    return 1;
}

int32_t SetOutputFormat(int32_t decoder, int32_t format, int32_t width, int32_t height) {
    return 1;
}

int32_t UnInitializeDivxDecoder(int32_t decoder) {
    return 0;
}

}
