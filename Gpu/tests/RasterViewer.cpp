// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/RasterViewerRenderer.h"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace Natron::Gpu;
int main()
{
    try {
        const std::array<float, 16> pixels{1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, .2f, .4f, .6f, 1};
        RasterViewerInput a;
        a.pixels = reinterpret_cast<const unsigned char *>(pixels.data());
        a.width = a.height = 2;
        a.storage = ViewerStorage::RGBA32F;
        a.display.bounds = a.display.clip = a.display.format = {0, 0, 2, 2};
        a.display.transform = false;
        ViewerSettings scene;
        scene.viewport = {0, 0, 2, 2};
        std::vector<float> result;
        unsigned checks = 0;
        auto expect = [&](size_t at, const std::array<float, 4> &value) {
            for (size_t c = 0; c < 4; ++c)
                if (std::abs(result[at * 4 + c] - value[c]) > 1e-6f) {
                    throw std::runtime_error("Raster pixel mismatch");
                }

            ++checks;
        };
        renderRasterViewer({a, {}}, scene, 2, 2, result);
        expect(0, {0, 0, 1, 1});
        expect(1, {.2f, .4f, .6f, 1});
        expect(2, {1, 0, 0, 1});
        expect(3, {0, 1, 0, 1});
        a.display.clip = {0, 0, 1, 2};
        renderRasterViewer({a, {}}, scene, 2, 2, result);
        expect(1, {0, 0, 0, 1});
        expect(3, {0, 0, 0, 1});
        a.display.clip = a.display.bounds;
        a.display.linearFilter = true;
        renderRasterViewer({a, {}}, scene, 1, 1, result);
        expect(0, {.3f, .35f, .4f, 1});
        const std::array<unsigned char, 16> bgra{0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};
        a.pixels = bgra.data();
        a.storage = ViewerStorage::BGRA8;
        a.display.linearFilter = false;
        renderRasterViewer({a, {}}, scene, 2, 2, result);
        expect(0, {0, 0, 1, 1});
        expect(3, {0, 1, 0, 1});
        const std::array<float, 4> half{.2f, .3f, .4f, .5f};
        RasterViewerInput b = a;
        b.pixels = reinterpret_cast<const unsigned char *>(half.data());
        b.width = b.height = 1;
        b.storage = ViewerStorage::RGBA32F;
        b.display.premult = ViewerPremult::Straight;
        scene.background = {.2f, .4f, .6f, 1};
        scene.checkerA = scene.background;
        scene.checkerboard = true;
        scene.checkerSize = 8;
        renderRasterViewer({b, {}}, scene, 1, 1, result);
        expect(0, {.2f, .35f, .5f, .75f});
        b.display.premult = ViewerPremult::Premultiplied;
        renderRasterViewer({b, {}}, scene, 1, 1, result);
        expect(0, {.3f, .5f, .7f, 1});
        scene.checkerboard = false;
        scene.operation = ViewerOperator::WipeUnder;
        scene.wipe = {1, 1, 0, 1};
        renderRasterViewer({a, b}, scene, 2, 2, result);
        expect(0, {0, 0, 1, 1});
        expect(1, half);
        expect(2, {1, 0, 0, 1});
        expect(3, half);
        std::array<float, 4> invalid{std::numeric_limits<float>::quiet_NaN(), 0, 0, 1};
        b.pixels = reinterpret_cast<const unsigned char *>(invalid.data());
        scene.operation = ViewerOperator::None;
        renderRasterViewer({b, {}}, scene, 1, 1, result);
        expect(0, {1, 0, 1, 1});
        scene.preserveDestination = true;
        bool rejected = false;

        try {
            renderRasterViewer(std::array<RasterViewerInput, 2> {}, scene, 3, 3, result);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }

        if (!rejected) {
            throw std::runtime_error("Raster overlay accepted an uninitialized destination");
        }

        ++checks;
        ViewerFrame tileFrame(nullptr, {-2, -1, 2, 1}, ViewerStorage::BGRA8);
        const std::array<unsigned char, 8> tilePixels{8, 17, 29, 255, 33, 47, 61, 128};
        tileFrame.uploadTile({-1, 0, 1, 1}, tilePixels.data(), tilePixels.size());

        if (tileFrame.image() || tileFrame.pixels().size() != 32 ||
            std::memcmp(tileFrame.pixels().data() + 20, tilePixels.data(), 8) != 0 ||
            tileFrame.pixels()[16] || tileFrame.pixels()[28]) {
            throw std::runtime_error("Raster cache tile origin, bytes or padding changed");
        }

        ++checks;
        rejected = false;

        try {
            tileFrame.uploadTile({-3, 0, 1, 1}, tilePixels.data(), tilePixels.size());
        } catch (const std::invalid_argument &) {
            rejected = true;
        }

        if (!rejected) {
            throw std::runtime_error("Raster cache tile exceeded frame bounds");
        }

        ++checks;
        ViewerFrame floatFrame(nullptr, {5, -3, 6, -2}, ViewerStorage::RGBA32F);
        floatFrame.uploadTile({5, -3, 6, -2}, invalid.data(), sizeof(invalid));

        if (std::memcmp(floatFrame.pixels().data(), invalid.data(), sizeof(invalid))) {
            throw std::runtime_error("Raster float frame changed non-finite pixel bits");
        }

        ++checks;
        scene.preserveDestination = false;
        auto rejectScene = [&](const ViewerSettings & value) {
            bool rejected = false;

            try {
                renderRasterViewer({b, {}}, value, 2, 2, result);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }

            if (!rejected) {
                throw std::runtime_error("Invalid raster scene accepted");
            }

            ++checks;
        };
        auto wrong = scene;
        wrong.operation = static_cast<ViewerOperator>(9);
        rejectScene(wrong);
        wrong = scene;
        wrong.wipe[3] = 1.1f;
        rejectScene(wrong);
        wrong = scene;
        wrong.checkerA[0] = std::numeric_limits<float>::infinity();
        rejectScene(wrong);
        wrong = scene;
        wrong.checkerSize = 0;
        rejectScene(wrong);
        wrong = scene;
        wrong.viewport[0] = std::numeric_limits<float>::quiet_NaN();
        rejectScene(wrong);
        auto rejectInput = [&](const RasterViewerInput & value) {
            bool rejected = false;

            try {
                renderRasterViewer({value, {}}, scene, 2, 2, result);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }

            if (!rejected) {
                throw std::runtime_error("Invalid raster input accepted");
            }

            ++checks;
        };
        auto bad = b;
        bad.display.gamma = std::numeric_limits<float>::quiet_NaN();
        rejectInput(bad);
        bad = b;
        bad.display.lut = static_cast<ViewerLut>(4);
        rejectInput(bad);
        bad = b;
        bad.display.premult = static_cast<ViewerPremult>(3);
        rejectInput(bad);
        bad = b;
        bad.display.pixelGrid = {-1, 1, 0, 0};
        rejectInput(bad);
        bad = b;
        bad.display.pixelGrid = {1, 0, 0, 0};
        rejectInput(bad);
        bad = b;
        bad.display.clip = {2, 0, 1, 2};
        rejectInput(bad);
        bad = b;
        bad.storage = static_cast<ViewerStorage>(2);
        rejectInput(bad);
        bad = b;
        bad.width = 0;
        rejectInput(bad);
#ifdef _WIN32

        if (GetModuleHandleW(L"vulkan-1.dll") || GetModuleHandleW(L"opengl32.dll")) {
            throw std::runtime_error("CPU raster test loaded a graphics driver");
        }

#endif
        std::cout << "Raster Viewer: " << checks << " independent pixel/scene checks passed; no GPU loader.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
