// Stage E7 test: the DivxDecoder API (Tools/XvidDecoder over the arm64 C-only xvidcore) on a real game movie.
//
//   MovieTest <game folder> [movie] [frames] [out.ppm]
//
//   game folder  the folder that contains Data/
//   movie        path under Data/ without ".avi" (default enUS/Interface/Cinematics/WOW_Wrathgate_1024)
//   frames       how many frames to decode (default 100)
//   out.ppm      writes the brightest decoded frame there (optional)
//
// Parses the AVI the same way CSimpleMovieFrame::ParseAVIFile does (RIFF/AVI, LIST hdrl + strh/strf, LIST movi,
// idx1 "##dc" entries), then calls InitializeDivxDecoder / SetOutputFormat(1) / DivxDecode / UnInitializeDivxDecoder
// as CSimpleMovieFrame::OpenVideo and DecodeFrame do, with a BGRA buffer of width * height * 4.
// Passes when every frame decodes and at least one frame is not black (movies fade in and out of black). Exit code 0 when everything passed.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "ThirdParty/DivxDecoder/DivxDecoder.h"

static uint32_t U32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: MovieTest <game folder> [movie] [frames] [out.ppm]\n");
        return 2;
    }

    std::string movie = argc > 2 ? argv[2] : "enUS/Interface/Cinematics/WOW_Wrathgate_1024";
    int32_t frames = argc > 3 ? std::atoi(argv[3]) : 100;
    const char* outPath = argc > 4 ? argv[4] : nullptr;
    std::string path = std::string(argv[1]) + "/Data/" + movie + ".avi";

    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        std::printf("FAIL cannot open %s\n", path.c_str());
        return 1;
    }

    std::vector<char> header;
    std::vector<char> index;
    long moviOffset = 0;
    char chunk[12];

    if (std::fread(chunk, 1, 12, file) != 12 || std::memcmp(chunk, "RIFF", 4) || std::memcmp(chunk + 8, "AVI ", 4)) {
        std::printf("FAIL %s is not a RIFF AVI\n", path.c_str());
        return 1;
    }

    while (std::fread(chunk, 1, 8, file) == 8) {
        uint32_t size = U32(chunk + 4);

        if (!std::memcmp(chunk, "LIST", 4)) {
            if (std::fread(chunk, 1, 4, file) != 4) {
                break;
            }
            size -= 4;

            if (!std::memcmp(chunk, "hdrl", 4)) {
                header.resize(size);
                if (std::fread(header.data(), 1, size, file) != size) {
                    break;
                }
            } else if (!std::memcmp(chunk, "movi", 4)) {
                moviOffset = std::ftell(file);
                std::fseek(file, size, SEEK_CUR);
            } else {
                std::fseek(file, size, SEEK_CUR);
            }
        } else if (!std::memcmp(chunk, "idx1", 4)) {
            index.resize(size);
            if (std::fread(index.data(), 1, size, file) != size) {
                break;
            }
        } else {
            std::fseek(file, size, SEEK_CUR);
        }
    }

    int32_t width = 0;
    int32_t height = 0;
    int32_t frameCount = 0;
    float fps = 0.0f;
    char handler[5] = {};
    int32_t streamIndex = 0;
    int32_t videoStream = -1;
    int32_t audioStream = -1;

    for (uint32_t offset = 0; offset + 8 <= header.size();) {
        const char* h = header.data();
        if (!std::memcmp(h + offset, "LIST", 4)) {
            offset += 12;
            continue;
        }

        uint32_t chunkSize = U32(h + offset + 4);

        if (!std::memcmp(h + offset, "strh", 4)) {
            offset += 8;
            if (!std::memcmp(h + offset, "vids", 4)) {
                std::memcpy(handler, h + offset + 4, 4);
                fps = static_cast<float>(U32(h + offset + 24)) / static_cast<float>(U32(h + offset + 20));
                videoStream = streamIndex;
                frameCount = static_cast<int32_t>(U32(h + offset + 32));
            }
            if (!std::memcmp(h + offset, "auds", 4)) {
                audioStream = streamIndex;
            }
            streamIndex++;
        } else {
            bool isFormat = !std::memcmp(h + offset, "strf", 4);
            offset += 8;
            if (isFormat && videoStream >= 0 && audioStream < 0) {
                width = static_cast<int32_t>(U32(h + offset + 4));
                height = static_cast<int32_t>(U32(h + offset + 8));
            }
        }

        offset += chunkSize;
    }

    std::vector<std::vector<uint8_t>> video;
    size_t audioBytes = 0;

    for (size_t i = 0; i + 16 <= index.size(); i += 16) {
        uint32_t size = U32(&index[i + 12]);
        if (index[i + 2] == 'd') {
            std::vector<uint8_t> data(size);
            std::fseek(file, static_cast<long>(U32(&index[i + 8])) + moviOffset + 4, SEEK_SET);
            if (size && std::fread(data.data(), 1, size, file) != size) {
                std::printf("FAIL short read in the movi list\n");
                return 1;
            }
            video.push_back(std::move(data));
        }
        if (index[i + 2] == 'w') {
            audioBytes += size;
        }
    }

    std::fclose(file);

    std::printf("%s: %dx%d, codec %s, %.3f fps, %d frames in strh, %zu video chunks, %zu audio bytes\n",
        path.c_str(), width, height, handler, fps, frameCount, video.size(), audioBytes);

    if (width <= 0 || height <= 0 || video.empty()) {
        std::printf("FAIL no video stream\n");
        return 1;
    }

    if (frames > static_cast<int32_t>(video.size())) {
        frames = static_cast<int32_t>(video.size());
    }

    const int32_t decoder = 1;
    if (InitializeDivxDecoder(decoder, width, height) || SetOutputFormat(decoder, 1, width, height)) {
        std::printf("FAIL InitializeDivxDecoder / SetOutputFormat\n");
        return 1;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(width) * height * 4);
    std::vector<uint8_t> best;
    size_t pixels = buffer.size() / 4;
    size_t bestNonBlack = 0;
    int32_t bestFrame = -1;
    uint64_t bestSum = 0;
    double decodeMs = 0.0;
    int32_t failures = 0;
    for (int32_t i = 0; i < frames; i++) {
        DivxDecodeFrame frame = {};
        frame.m_output = buffer.data();
        frame.m_input = video[i].data();
        frame.m_inputSize = static_cast<uint32_t>(video[i].size());
        frame.m_render = 1;

        auto start = std::chrono::steady_clock::now();
        int32_t error = DivxDecode(decoder, &frame, 0);
        decodeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        if (error) {
            std::printf("FAIL DivxDecode frame %d (%zu bytes)\n", i, video[i].size());
            failures++;
            continue;
        }

        uint64_t sum = 0;
        size_t nonBlack = 0;
        for (size_t p = 0; p < buffer.size(); p += 4) {
            uint32_t luma = buffer[p] + buffer[p + 1] + buffer[p + 2];
            sum += luma;
            nonBlack += luma > 48;
        }

        if (bestFrame < 0 || nonBlack > bestNonBlack) {
            bestNonBlack = nonBlack;
            bestSum = sum;
            bestFrame = i;
            best = buffer;
        }
    }

    if (UnInitializeDivxDecoder(decoder)) {
        std::printf("FAIL UnInitializeDivxDecoder\n");
        failures++;
    }

    double nonBlackPct = 100.0 * bestNonBlack / pixels;

    std::printf("decoded %d frames in %.1f ms (%.2f ms/frame); brightest frame %d: mean B+G+R %.1f, %.1f%% pixels not black\n",
        frames, decodeMs, decodeMs / frames, bestFrame, static_cast<double>(bestSum) / pixels, nonBlackPct);

    if (nonBlackPct < 5.0) {
        std::printf("FAIL every decoded frame is black\n");
        failures++;
    }

    if (outPath && bestFrame >= 0) {
        // The buffer is BGRA, bottom-up (XVID_CSP_VFLIP), as the movie textures expect; the PPM is top-down RGB.
        FILE* out = std::fopen(outPath, "wb");
        if (out) {
            std::fprintf(out, "P6\n%d %d\n255\n", width, height);
            for (int32_t y = height - 1; y >= 0; y--) {
                const uint8_t* row = &best[static_cast<size_t>(y) * width * 4];
                for (int32_t x = 0; x < width; x++) {
                    uint8_t rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0] };
                    std::fwrite(rgb, 1, 3, out);
                }
            }
            std::fclose(out);
            std::printf("wrote %s\n", outPath);
        }
    }

    std::printf(failures ? "MovieTest: %d failure(s)\n" : "MovieTest: all passed\n", failures);
    return failures ? 1 : 0;
}
