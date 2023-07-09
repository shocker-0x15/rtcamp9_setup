#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string_view>
#include <chrono>
#include <thread>

// https://github.com/richgel999/fpng
#include "fpng.h"

int32_t main(int32_t argc, const char* argv[]) {
    // レンダラー起動時間を取得。
    using clock = std::chrono::high_resolution_clock;
    const clock::time_point appStartTp = clock::now();

    uint32_t startFrameIndex = 0;
    uint32_t endFrameIndex = 0;
    for (int argIdx = 1; argIdx < argc; ++argIdx) {
        std::string_view arg = argv[argIdx];
        if (arg == "--frame-range") {
            if (argIdx + 2 >= argc) {
                printf("--frame-range requires a frame number pair to start and end.\n");
                return -1;
            }
            startFrameIndex = static_cast<uint32_t>(atoi(argv[argIdx + 1]));
            endFrameIndex = static_cast<uint32_t>(atoi(argv[argIdx + 2]));
            argIdx += 2;
        }
        else {
            printf("Unknown argument %s.\n", argv[argIdx]);
            return -1;
        }
    }

    if (endFrameIndex < startFrameIndex) {
        printf("Invalid frame range.\n");
        return -1;
    }



    using namespace fpng;
    fpng_init();

    struct RGBA {
        uint32_t r : 8;
        uint32_t g : 8;
        uint32_t b : 8;
        uint32_t a : 8;
    };
    constexpr uint32_t width = 256;
    constexpr uint32_t height = 256;
    std::vector<RGBA> pixels(height * width);

    for (uint32_t frameIndex = startFrameIndex; frameIndex <= endFrameIndex; ++frameIndex) {
        const clock::time_point frameStartTp = clock::now();
        printf("Frame %u ... ", frameIndex);

        // 高度なレンダリング...
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                RGBA v;
                v.r = x;
                v.g = y;
                v.b = frameIndex;
                v.a = 255;
                const int32_t idx = y * width + x;
                pixels[idx] = v;
            }
        }

        // 起動からの時刻とフレーム時間を計算。
        const clock::time_point now = clock::now();
        const clock::duration frameTime = now - frameStartTp;
        const clock::duration totalTime = now - appStartTp;
        printf(
            "Done: %.3f [ms] (total: %.3f [s])\n",
            std::chrono::duration_cast<std::chrono::microseconds>(frameTime).count() * 1e-3f,
            std::chrono::duration_cast<std::chrono::milliseconds>(totalTime).count() * 1e-3f);

        // 3桁連番で画像出力。
        char filename[256];
        sprintf_s(filename, "%03u.png", frameIndex);
        fpng_encode_image_to_file(filename, pixels.data(), width, height, 4, 0);
    }

    return 0;
}
