/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef GEODATA_HPP_d5f4g65de
#define GEODATA_HPP_d5f4g65de

#include <array>
#include <vector>
#include <string>
#include <memory>

#include "foundation.hpp"

namespace vts
{

// information about geodata passed to loadGeodata callback
class VTS_API GpuGeodataSpec
{
public:
    enum class Type
    {
        Invalid,
        LineScreen,
        LineFlat,
        LineLabel,
        PointScreen,
        PointFlat,
        PointLabel,
        Icon,
        PackedLabelIcon,
        Triangles,
    };

    enum class Units
    {
        Invalid,
        Pixels,
        Meters,
        Ratio,
    };

    enum class TextAlign
    {
        Invalid,
        Left,
        Right,
        Center,
    };

    enum class Origin
    {
        Invalid,
        TopLeft,
        TopRight,
        TopCenter,
        CenterLeft,
        CenterRight,
        CenterCenter,
        BottomLeft,
        BottomRight,
        BottomCenter,
    };

    struct VTS_API Stick
    {
        float color[4];
        float heights[2];
        float width;
        float offset;
    };

    struct VTS_API Icon
    {
        float offset[2];
        float scale;
        Origin origin;
    };

    struct VTS_API Line
    {
        float color[4];
        float width;
        Units units;
    };

    struct VTS_API LineLabel
    {
        float color[4];
        float color2[4];
        float size;
        float offset;
    };

    struct VTS_API Point
    {
        float color[4];
        float radius;
    };

    struct VTS_API PointLabel
    {
        float outline[4];
        float color[4];
        float color2[4];
        float margin[4];
        float offset[2];
        float size;
        float width;
        Origin origin;
        TextAlign textAlign;
    };

    struct VTS_API PackedLabelIcon
    {
        PointLabel pointlabel;
        Icon icon;
    };

    union VTS_API UnionData
    {
        Icon icon;
        Line line;
        LineLabel lineLabel;
        Point point;
        PointLabel pointLabel;
        PackedLabelIcon packedLabelIcon;
        UnionData();
    };

    struct VTS_API CommonData
    {
        Stick stick;
        float visibilities[4]; // distance, view-min, view-max, culling
        float zBufferOffset[3];
        sint32 zIndex;
        CommonData();
    };

    GpuGeodataSpec();

    // positions
    std::vector<std::vector<std::array<float, 3>>> positions;
    std::vector<std::vector<std::array<float, 2>>> uvs;

    // properties per position-segment
    std::vector<std::shared_ptr<void>> bitmaps;
    std::vector<std::string> texts;
    std::vector<float> importances;

    // global properties
    std::vector<std::shared_ptr<void>> fontCascade;
    double model[16];
    UnionData unionData;
    CommonData commonData;
    Type type;
};

} // namespace vts

#endif
