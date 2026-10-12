/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstring>
#include <cstdint>
#include <vector>
#include <cmath>
#include <limits>
#include <sstream>
#include <boost/archive/xml_iarchive.hpp>
#include <boost/archive/xml_oarchive.hpp>
#include <gtest/gtest.h>

#include "Engine/Image.h"
#include "Engine/ViewIdx.h"
#include "Engine/ImageSerialization.h"

NATRON_NAMESPACE_USING

namespace
{
template <typename Pixel>
void checkMipmapEdges(ImageBitDepthEnum depth)
{
    for (const auto& bounds : {
             RectI(0, 0, 7, 5), RectI(-3, -5, 4, 2), RectI(2, 4, 3, 11),
             RectI(2, 4, 9, 5), RectI(-3, -5, -2, 2), RectI(-3, -5, 4, -4),
             RectI(3, 5, 4, 6), RectI(-3, -5, -2, -4), RectI(0, 0, 8, 6)
         }) {
        const RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);
        Image source(ImagePlaneDesc::getRGBAComponents(), rod, bounds, 0, 1., depth,
                     eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, true);
        std::vector<Pixel> reference(size_t(bounds.area()) * 4);
        {
            auto pixels = source.getWriteRights();

            for (int y = bounds.y1; y < bounds.y2; ++y) for (int x = bounds.x1; x < bounds.x2; ++x) {
                    auto* pixel = reinterpret_cast<Pixel *>(pixels.pixelAt(x, y));

                    for (int c = 0; c < 4; ++c) {
                        size_t index = (size_t(y - bounds.y1) * bounds.width() + x - bounds.x1) * 4 + c;
                        reference[index] = pixel[c] = Pixel(16 + index % 30);
                    }
                }
        }
        source.markForRendered(bounds);
        RectI previous = bounds;

        for (unsigned level = 1; level <= 3; ++level) {
            const RectI target = previous.downscalePowerOfTwoSmallestEnclosing(1);
            std::vector<Pixel> reduced(size_t(target.area()) * 4);

            for (int y = target.y1; y < target.y2; ++y) for (int x = target.x1; x < target.x2; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        float sum = 0;
                        int count = 0;

                        for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                                const int sx = x * 2 + dx, sy = y * 2 + dy;

                                if (sx >= previous.x1 && sx < previous.x2 && sy >= previous.y1 && sy < previous.y2) {
                                    sum += reference[(size_t(sy - previous.y1) * previous.width() + sx - previous.x1) * 4 + c];
                                    ++count;
                                }
                            }

                        ASSERT_GT(count, 0);
                        reduced[(size_t(y - target.y1) * target.width() + x - target.x1) * 4 + c] = Pixel(sum / count);
                    }
                }

            reference.swap(reduced);
            previous = target;
            const RectI allocation(target.x1 - 1, target.y1 - 1, target.x2 + 1, target.y2 + 1);

            for (bool bitmap : {
                     false, true
                 }) {
                Image output(ImagePlaneDesc::getRGBAComponents(), rod, allocation, level, 1., depth,
                             eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, true);
                {
                    auto pixels = output.getWriteRights();

                    for (int y = allocation.y1; y < allocation.y2; ++y) for (int x = allocation.x1; x < allocation.x2; ++x) {
                            auto* pixel = reinterpret_cast<Pixel *>(pixels.pixelAt(x, y));

                            for (int c = 0; c < 4; ++c) {
                                pixel[c] = Pixel(91);
                            }
                        }
                }
                source.downscaleMipmap(rod, bounds, 0, level, bitmap, &output);
                {
                    auto pixels = output.getReadRights();

                    for (int y = allocation.y1; y < allocation.y2; ++y) for (int x = allocation.x1; x < allocation.x2; ++x) {
                            const auto* pixel = reinterpret_cast<const Pixel *>(pixels.pixelAt(x, y));

                            for (int c = 0; c < 4; ++c) {
                                Pixel expected = Pixel(91);

                                if (x >= target.x1 && x < target.x2 && y >= target.y1 && y < target.y2) {
                                    expected = reference[(size_t(y - target.y1) * target.width() + x - target.x1) * 4 + c];
                                }

                                EXPECT_EQ(pixel[c], expected) << "level=" << level << " x=" << x << " y=" << y;
                            }
                        }
                }
                std::list<RectI> missing;
                output.getRestToRender(target, missing);
                EXPECT_EQ(missing.empty(), bitmap);
            }
        }
    }
}
}

namespace
{
template <typename Pixel>
void checkUpscalePadding(ImageBitDepthEnum depth)
{
    for (const RectI& bounds : {
             RectI(-3, -5, 4, 2), RectI(1, 3, 8, 10)
         })

        for (unsigned level : {
                 1u, 3u

             }) for (bool partial : {
                             false, true

                         }) for (bool alpha : {
                                         false, true
                                     }) {
                    const auto components = alpha ? ImagePlaneDesc::getAlphaComponents() : ImagePlaneDesc::getRGBAComponents();
                    const RectD rod = bounds.toCanonical_noClipping(level, 1.);
                    Image source(components, rod, bounds, level, 1., depth, eImagePremultiplicationPremultiplied, eImageFieldingOrderNone);
                    {
                        auto access = source.getWriteRights();

                        for (int y = bounds.y1; y < bounds.y2; ++y) for (int x = bounds.x1; x < bounds.x2; ++x) {
                                auto* pixel = reinterpret_cast<Pixel *>(access.pixelAt(x, y));

                                for (int c = 0; c < components.getNumComponents(); ++c) {
                                    pixel[c] = Pixel(16 + (x - bounds.x1) * 7 + (y - bounds.y1) * 5 + c);
                                }
                            }
                    }
                    const RectI requested = partial ? RectI(bounds.x1 + 1, bounds.y1 + 1, bounds.x2 - 1, bounds.y2 - 1) : bounds;
                    const RectI target = requested.upscalePowerOfTwo(level);
                    const RectI padded(target.x1 - 1, target.y1 - 1, target.x2 + 1, target.y2 + 1);
                    Image output(components, rod, padded, 0, 1., depth, eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, true);
                    {
                        auto access = output.getWriteRights();

                        for (int y = padded.y1; y < padded.y2; ++y) for (int x = padded.x1; x < padded.x2; ++x) {
                                auto* pixel = reinterpret_cast<Pixel *>(access.pixelAt(x, y));
                                std::fill(pixel, pixel + components.getNumComponents(), Pixel(91));
                            }
                    }
                    output.markForRendered(RectI(padded.x1, padded.y1, padded.x1 + 1, padded.y2));
                    source.upscaleMipmap(requested, level, 0, &output);
                    auto access = output.getReadRights();

                    for (int y = padded.y1; y < padded.y2; ++y) for (int x = padded.x1; x < padded.x2; ++x) {
                            const auto* pixel = reinterpret_cast<const Pixel *>(access.pixelAt(x, y));
                            const bool inside = x >= target.x1 && x < target.x2 && y >= target.y1 && y < target.y2;
                            const int sx = int(std::floor(double(x) / (1 << level))), sy = int(std::floor(double(y) / (1 << level)));

                            for (int c = 0; c < components.getNumComponents(); ++c) {
                                EXPECT_EQ(pixel[c], inside ? Pixel(16 + (sx - bounds.x1) * 7 + (sy - bounds.y1) * 5 + c) : Pixel(91));
                            }

                            EXPECT_EQ(*access.bitmapAt(x, y), x == padded.x1 ? 1 : 0);
                        }
                }
}
}
TEST(ImageUpscaleTest, NegativeCoordinatesPartialRoiAndPadding)
{
    checkUpscalePadding<unsigned char>(eImageBitDepthByte);
    checkUpscalePadding<unsigned short>(eImageBitDepthShort);
    checkUpscalePadding<float>(eImageBitDepthFloat);
}
TEST(ImageKeyTest, SerializedProxyInputQuality)
{
    const ImageKey original(nullptr, 52, false, 1., ViewIdx(0), 1., false, true, 2);
    std::stringstream stream;
    {
        boost::archive::xml_oarchive archive(stream);
        archive << boost::serialization::make_nvp("Key", original);
    }
    ImageKey restored;
    {
        boost::archive::xml_iarchive archive(stream);
        archive >> boost::serialization::make_nvp("Key", restored);
    }
    EXPECT_TRUE(original == restored);
    EXPECT_EQ(original.getHash(), restored.getHash());
    const ImageKey otherQuality(nullptr, 52, false, 1., ViewIdx(0), 1., false, true, 3);
    EXPECT_FALSE(restored == otherQuality);
    EXPECT_NE(restored.getHash(), otherQuality.getHash());

    // Version 2 did not record either input-quality field. Such an old cache
    // cannot prove its quality and must not satisfy a new render request.
    std::string legacy = stream.str();
    const auto version = legacy.find("version=\"3\"");
    ASSERT_NE(version, std::string::npos);
    legacy.replace(version, 11, "version=\"2\"");

    for (const std::string tag : {
             "FullScaleWithDownscaleInputs", "InputMipmapLevel"
         }) {
        const auto begin = legacy.find("<" + tag + ">");
        const auto end = legacy.find("</" + tag + ">", begin);
        ASSERT_NE(begin, std::string::npos);
        ASSERT_NE(end, std::string::npos);
        legacy.erase(begin, end + tag.size() + 3 - begin);
    }
    std::stringstream legacyStream(legacy);
    ImageKey old;
    {
        boost::archive::xml_iarchive archive(legacyStream);
        archive >> boost::serialization::make_nvp("Key", old);
    }
    EXPECT_EQ(old._inputMipmapLevel, std::numeric_limits<unsigned int>::max());
    EXPECT_FALSE(old == original);
    const ImageKey fullQuality(nullptr, 52, false, 1., ViewIdx(0), 1., false, false);
    EXPECT_FALSE(old == fullQuality);
    EXPECT_NE(old.getHash(), fullQuality.getHash());
}

TEST(ImageMipmapTest, OddNegativeAndSinglePixelEdges)
{
    checkMipmapEdges<unsigned char>(eImageBitDepthByte);
    checkMipmapEdges<unsigned short>(eImageBitDepthShort);
    checkMipmapEdges<float>(eImageBitDepthFloat);
}

TEST(ImageMipmapTest, IncompleteEdgeBitmap)
{
    const RectI bounds(-3, -5, 4, 2);
    const RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);
    Image source(ImagePlaneDesc::getRGBAComponents(), rod, bounds, 0, 1., eImageBitDepthFloat,
                 eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, true);
    source.fill(bounds, .2f, .4f, .6f, .8f);
    source.markForRendered(bounds);
    source.clearBitmap(RectI(-3, -5, -2, -4));
    const RectI target = bounds.downscalePowerOfTwoSmallestEnclosing(1);
    Image output(ImagePlaneDesc::getRGBAComponents(), rod, target, 1, 1., eImageBitDepthFloat,
                 eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, true);
    source.downscaleMipmap(rod, bounds, 0, 1, true, &output);
    std::list<RectI> missing;
    output.getRestToRender(target, missing);
    ASSERT_EQ(missing.size(), 1);
    EXPECT_EQ(missing.front(), RectI(-2, -3, -1, -2));
}

TEST(ImageMaskMixTest, MissingOriginalUsesZero)
{
    const RectI bounds(-3, -2, 5, 4), roi(-1, 0, 3, 2);
    const RectD rod(-3, -2, 5, 4);
    Image output(ImagePlaneDesc::getRGBAComponents(), rod, bounds, 0, 1., eImageBitDepthFloat,
                 eImagePremultiplicationPremultiplied, eImageFieldingOrderNone);
    Image mask(ImagePlaneDesc::getAlphaComponents(), rod, bounds, 0, 1., eImageBitDepthFloat,
               eImagePremultiplicationPremultiplied, eImageFieldingOrderNone);

    for (bool masked : {
             false, true
         }) {
        output.fill(bounds, 2, 4, 6, 0.8f);
        mask.fill(bounds, 0, 0, 0, 0.5f);
        output.applyMaskMix(roi, masked ? &mask : nullptr, nullptr, masked, true, 0.4f);
        auto pixels = output.getReadRights();

        for (int y = bounds.y1; y < bounds.y2; ++y) for (int x = bounds.x1; x < bounds.x2; ++x) {
                const auto* value = reinterpret_cast<const float *>(pixels.pixelAt(x, y));
                const float scale = x >= roi.x1 && x < roi.x2 && y >= roi.y1 && y < roi.y2 ? (masked ? 0.2f : 0.4f) : 1.f;
                EXPECT_NEAR(2 * scale, value[0], 0.000001f);
                EXPECT_NEAR(4 * scale, value[1], 0.000001f);
                EXPECT_NEAR(6 * scale, value[2], 0.000001f);
                EXPECT_NEAR(0.8f * scale, value[3], 0.000001f);
            }
    }
}

TEST(BitmapTest,
     SimpleRect)
{
    RectI rod(0, 0, 100, 100);
    Bitmap bm(rod);

    ///assert that the union of all the non rendered rects is the rod
    std::list<RectI> nonRenderedRects;

    bm.minimalNonMarkedRects(rod, nonRenderedRects);
    RectI nonRenderedRectsUnion;

    for (std::list<RectI>::iterator it = nonRenderedRects.begin(); it != nonRenderedRects.end(); ++it) {
        nonRenderedRectsUnion.merge(*it);
    }

    ASSERT_TRUE(rod == nonRenderedRectsUnion);

    ///assert that the "underlying" bitmap is clean
    const char *map = bm.getBitmap();
    ASSERT_TRUE(!std::memchr(map, 1, rod.area()));
    ASSERT_TRUE(bm.isNonMarked(rod));

    RectI halfRoD(0, 0, 100, 50);
    bm.markForRendered(halfRoD);

    ///assert that non of the rendered rects interesect the non rendered half
    RectI nonRenderedHalf(0, 50, 100, 100);
    nonRenderedRects.clear();
    bm.minimalNonMarkedRects(rod, nonRenderedRects);

    for (std::list<RectI>::iterator it = nonRenderedRects.begin(); it != nonRenderedRects.end(); ++it) {
        ASSERT_TRUE((*it).intersects(nonRenderedHalf));
    }


    ///assert that the underlying bitmap is marked as expected
    const char *start = map;

    ///check that there are only ones in the rendered half
    ASSERT_TRUE(!memchr(start, 0, halfRoD.area()));

    ///check that there are only 0s in the non rendered half
    start = map + halfRoD.area();
    ASSERT_TRUE(!memchr(start, 1, halfRoD.area()));

    ///mark for renderer the other half of the rod
    bm.markForRendered(nonRenderedHalf);

    ///assert that the bm is rendered totally
    nonRenderedRects.clear();
    bm.minimalNonMarkedRects(rod, nonRenderedRects);
    ASSERT_TRUE(nonRenderedRects.empty());
    ASSERT_TRUE(!memchr(map, 0, rod.area()));

    ///More complex example where A,B,C,D are not rendered check that both trimap & bitmap yield the same result
    // BBBBBBBBBBBBBB
    // BBBBBBBBBBBBBB
    // CXXXXXXXXXXDDD
    // CXXXXXXXXXXDDD
    // CXXXXXXXXXXDDD
    // CXXXXXXXXXXDDD
    // AAAAAAAAAAAAAA
    bm.clear(rod);

    RectI xBox(20, 20, 80, 80);
    bm.markForRendered(xBox);
    nonRenderedRects.clear();
    bm.minimalNonMarkedRects(rod, nonRenderedRects);
    EXPECT_TRUE(nonRenderedRects.size() == 4);
    nonRenderedRects.clear();
    bool beingRenderedElseWhere = false;
    bm.minimalNonMarkedRects_trimap(rod, nonRenderedRects, &beingRenderedElseWhere);
    EXPECT_TRUE(nonRenderedRects.size() == 4);
    ASSERT_TRUE(beingRenderedElseWhere == false);

    nonRenderedRects.clear();
    //Mark the A rectangle as being rendered
    RectI aBox(0, 0, 20, 20);
    bm.markForRendering(aBox);
    bm.minimalNonMarkedRects_trimap(rod, nonRenderedRects, &beingRenderedElseWhere);
    ASSERT_TRUE(beingRenderedElseWhere == true);
    EXPECT_TRUE(nonRenderedRects.size() == 3);
} // TEST

TEST(ImageNaNTest, BitPatternsAndRoiBoundaries)
{
    const uint32_t patterns[] = {0x00000000, 0x80000000, 0x00000001, 0x80000001,
                                 0x3f800000, 0xff7fffff, 0x7f800000, 0xff800000,
                                 0x7fc12345, 0xffc54321, 0x7f800001, 0xff800001
                                };

    for (const auto& components : {
             ImagePlaneDesc::getAlphaComponents(), ImagePlaneDesc::getXYComponents(),
             ImagePlaneDesc::getRGBComponents(), ImagePlaneDesc::getRGBAComponents()
         }) {
        for (int width = 1; width <= 35; ++width) {
            const RectI bounds(-3, -2, width + 1, 3), roi(-2, -1, width - 2, 2);
            const RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);
            Image image(components, rod, bounds, 0, 1., eImageBitDepthFloat,
                        eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, true);
            image.markForRendered(bounds);
            const unsigned channels = components.getNumComponents();
            std::vector<uint32_t> expected(size_t(bounds.area()) * channels);
            bool containsNaN = false;

            {
                auto pixels = image.getWriteRights();

                for (int y = bounds.y1; y < bounds.y2; ++y) {
                    auto* row = pixels.pixelAt(bounds.x1, y);

                    for (int x = bounds.x1; x < bounds.x2; ++x) for (unsigned c = 0; c < channels; ++c) {
                            const size_t index = (size_t(y - bounds.y1) * bounds.width() + x - bounds.x1) * channels + c;
                            const uint32_t bits = patterns[index % (sizeof(patterns) / sizeof(patterns[0]))];
                            std::memcpy(row + (size_t(x - bounds.x1) * channels + c) * sizeof(float), &bits, sizeof(bits));
                            const bool nanInRoi = x >= roi.x1 && x < roi.x2 && y >= roi.y1 && y < roi.y2 &&
                                                  (bits & 0x7fffffff) > 0x7f800000;
                            expected[index] = nanInRoi ? 0x3f800000 : bits;
                            containsNaN = containsNaN || nanInRoi;
                        }
                }
            }

            EXPECT_EQ(image.checkForNaNsAndFix(roi), containsNaN);
            EXPECT_FALSE(image.checkForNaNsAndFix(roi));

            {
                auto pixels = image.getReadRights();

                for (int y = bounds.y1; y < bounds.y2; ++y) {
                    const size_t offset = size_t(y - bounds.y1) * bounds.width() * channels;
                    EXPECT_EQ(std::memcmp(pixels.pixelAt(bounds.x1, y), expected.data() + offset,
                                          size_t(bounds.width()) * channels * sizeof(float)), 0);
                }
            }

            std::list<RectI> missing;
            image.getRestToRender(bounds, missing);
            EXPECT_TRUE(missing.empty());
        }
    }
}

TEST(BitmapTest, RowBlockEdgesAndTrimap)
{
    for (int width : {
             1, 15, 16, 17, 31, 32, 33, 65
         }) {
        const RectI bounds(-5, -3, width - 5, 2);
        Bitmap bitmap(bounds);
        bitmap.setTo1();
        EXPECT_TRUE(bitmap.minimalNonMarkedBbox(bounds).isNull());

        for (int y = bounds.y1; y < bounds.y2; ++y) for (int x = bounds.x1; x < bounds.x2; ++x) {
                const RectI hole(x, y, x + 1, y + 1);
                bitmap.clear(hole);
                EXPECT_EQ(bitmap.minimalNonMarkedBbox(bounds), hole);
                bool pending = false;
                EXPECT_EQ(bitmap.minimalNonMarkedBbox_trimap(bounds, &pending), hole);
                EXPECT_FALSE(pending);
                bitmap.markForRendering(hole);
                EXPECT_EQ(bitmap.minimalNonMarkedBbox(bounds), hole);
                EXPECT_TRUE(bitmap.minimalNonMarkedBbox_trimap(bounds, &pending).isNull());
                EXPECT_TRUE(pending);
                bitmap.markForRendered(hole);
            }
    }
}

TEST(ImageKeyTest, Equality)
{
    srand(2000);
    // coverity[dont_call]
    int randomHashKey1 = rand();
    SequenceTime time1 = 0;
    ViewIdx view1(0);
    double pa1 = 1.;
    ImageKey key1(0, randomHashKey1, false, time1, view1, pa1, false, false);
    U64 keyHash1 = key1.getHash();


    ///make a second ImageKey equal to the first
    int randomHashKey2 = randomHashKey1;
    SequenceTime time2 = time1;
    ViewIdx view2(view1);
    double pa2 = pa1;
    ImageKey key2(0, randomHashKey2, false, time2, view2, pa2, false, false);
    U64 keyHash2 = key2.getHash();
    ASSERT_TRUE(keyHash1 == keyHash2);
}

TEST(ImageKeyTest, Difference)
{
    srand(2000);
    // coverity[dont_call]
    int randomHashKey1 = rand() % 100;
    SequenceTime time1 = 0;
    ViewIdx view1(0);
    double pa1 = 1.;
    ImageKey key1(0, randomHashKey1, false, time1, view1, pa1, false, false);
    U64 keyHash1 = key1.getHash();


    ///make a second ImageKey different to the first
    // coverity[dont_call]
    int randomHashKey2 = rand() % 1000  + 150;
    SequenceTime time2 = time1;
    ViewIdx view2(view1);
    double pa2 = pa1;
    ImageKey key2(0, randomHashKey2, false, time2, view2, pa2, false, false);
    U64 keyHash2 = key2.getHash();
    ASSERT_TRUE(keyHash1 != keyHash2);
}

