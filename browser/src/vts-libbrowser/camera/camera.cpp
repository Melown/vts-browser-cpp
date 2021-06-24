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

#include "../camera.hpp"
#include "../traverseNode.hpp"
#include "../renderTasks.hpp"
#include "../map.hpp"
#include "../gpuResource.hpp"
#include "../renderInfos.hpp"
#include "../mapLayer.hpp"
#include "../mapConfig.hpp"
#include "../credits.hpp"
#include "../coordsManip.hpp"
#include "../hashTileId.hpp"
#include "../geodata.hpp"

#include <unordered_set>
#include <optick.h>

namespace vts
{

CameraImpl::CameraImpl(MapImpl *map, Camera *cam) :
    map(map), camera(cam),
    viewProjActual(identityMatrix4()),
    viewProjRender(identityMatrix4()),
    viewProjCulling(identityMatrix4()),
    viewActual(identityMatrix4()),
    apiProj(identityMatrix4()),
    cullingPlanes { nan4(), nan4(), nan4(), nan4(), nan4(), nan4() },
    perpendicularUnitVector(nan3()),
    forwardUnitVector(nan3()),
    cameraPosPhys(nan3()),
    focusPosPhys(nan3()),
    eye(nan3()),
    target(nan3()),
    up(nan3())
{}

void CameraImpl::clear()
{
    OPTICK_EVENT();
    draws.clear();
    credits.clear();

    // reset statistics
    {
        for (uint32 i = 0; i < CameraStatistics::MaxLods; i++)
        {
            statistics.metaNodesTraversedPerLod[i] = 0;
            statistics.nodesRenderedPerLod[i] = 0;
        }
        statistics.metaNodesTraversedTotal = 0;
        statistics.nodesRenderedTotal = 0;
        statistics.currentNodeMetaUpdates = 0;
        statistics.currentNodeDrawsUpdates = 0;
    }
}

namespace
{

void touchDraws(MapImpl *map, const RenderSurfaceTask &task)
{
    if (task.mesh)
        map->touchResource(task.mesh);
    if (task.textureColor)
        map->touchResource(task.textureColor);
    if (task.textureMask)
        map->touchResource(task.textureMask);
}

template<class T>
void touchDraws(MapImpl *map, const T &renders)
{
    for (auto &it : renders)
        touchDraws(map, it);
}

} // namespace

void CameraImpl::touchDraws(TraverseNode *trav)
{
    vts::touchDraws(map, trav->opaque);
    vts::touchDraws(map, trav->transparent);
    for (const auto &it : trav->resources)
        map->touchResource(it);
}

bool CameraImpl::visibilityTest(TraverseNode *trav)
{
    assert(trav->meta);
    // aabb test
    if (!aabbTest(trav->meta->aabbPhys, cullingPlanes))
        return false;
    // additional obb test
    if (trav->meta->obb)
    {
        const MetaNode::Obb &obb = *trav->meta->obb;
        vec4 planes[6];
        vts::frustumPlanes(viewProjCulling * obb.rotInv, planes);
        if (!aabbTest(obb.points, planes))
            return false;
    }
    // all tests passed
    return true;
}

bool CameraImpl::coarsenessTest(TraverseNode *trav)
{
    assert(trav->meta);
    return coarsenessValue(trav)
        < (trav->layer->isGeodata()
        ? options.targetPixelRatioGeodata
        : options.targetPixelRatioSurfaces);
}

namespace
{

double distanceToDisk(const vec3 &diskNormal,
    const vec2 &diskHeights, double diskHalfAngle,
    const vec3 &point)
{
    double l = point.norm();
    vec3 n = point.normalized();
    double angle = std::acos(dot(diskNormal, n));
    double vertical = l > diskHeights[1] ? l - diskHeights[1] :
        l < diskHeights[0] ? diskHeights[0] - l : 0;
    double horizontal = std::max(angle - diskHalfAngle, 0.0) * l;
    double d = std::sqrt(vertical * vertical + horizontal * horizontal);
    assert(!std::isnan(d) && d >= 0);
    return d;
}

} // namespace

double CameraImpl::coarsenessValue(TraverseNode *trav)
{
    assert(trav->meta);
    assert(!std::isnan(trav->meta->texelSize));

    const auto &meta = trav->meta;

    if (meta->texelSize == inf1())
        return meta->texelSize;

    if (map->options.debugCoarsenessDisks
        && !std::isnan(meta->diskHalfAngle))
    {
        // test the value at point at the distance from the disk
        double dist = distanceToDisk(meta->diskNormalPhys,
            meta->diskHeightsPhys, meta->diskHalfAngle,
            cameraPosPhys);
        double v = meta->texelSize * diskNominalDistance / dist;
        assert(!std::isnan(v) && v > 0);
        return v;
    }
    else
    {
        // test the value on all corners of node bounding box
        double result = 0;
        for (uint32 i = 0; i < 8; i++)
        {
            vec3 c = meta->cornersPhys(i);
            vec3 up = perpendicularUnitVector * meta->texelSize;
            vec3 c1 = c - up * 0.5;
            vec3 c2 = c1 + up;
            c1 = vec4to3(vec4(viewProjRender * vec3to4(c1, 1)), true);
            c2 = vec4to3(vec4(viewProjRender * vec3to4(c2, 1)), true);
            double len = std::abs(c2[1] - c1[1]);
            result = std::max(result, len);
        }
        result *= windowHeight * 0.5;
        return result;
    }
}

float CameraImpl::getTextSize(float size, const std::string &text)
{
    float x = 0;
    float sizeX = size - 1;
    float sizeX2 = round(size * 0.5);

    for (uint32 i = 0, li = text.size(); i < li; i++)
    {
        uint8 c = text[i] - 32;

        switch (c)
        {
        case 1:
        case 7:
        case 12:
        case 14:
        case 27: //:
        case 28: //;
        case 64: //'
        case 73: //i
        case 76: //l
        case 84: //t
            x += sizeX2;
            break;

        default:
            x += sizeX;
            break;
        }
    }

    return x;
}

void CameraImpl::renderText(TraverseNode *trav, float x, float y,
    const vec4f &color, float size,
    const std::string &text, bool centerText)
{
    assert(trav);
    assert(trav->meta);

    RenderInfographicsTask task;
    task.mesh = map->getMesh("internal://data/meshes/rect.obj");
    task.mesh->priority = inf1();

    task.textureColor =
        map->getTexture("internal://data/textures/debugFont2.png");
    task.textureColor->priority = inf1();

    task.model = translationMatrix(*trav->meta->surrogatePhys);
    task.color = color;

    if (centerText)
        x -= round(getTextSize(size, text) * 0.5);

    float sizeX = size - 1;
    float sizeX2 = round(size * 0.5);

    if (task.ready())
    {
        // black box
        {
            auto ctask = convert(task);
            float l = getTextSize(size, text);
            ctask.data[0] = size + 2;
            ctask.data[1] = (l + 2) / (size + 2);
            ctask.data[2] = 2.0 / windowWidth;
            ctask.data[3] = 2.0 / windowHeight;
            ctask.data2[0] = -1000;
            ctask.data2[1] = 0;
            ctask.data2[2] = x - 1;
            ctask.data2[3] = y - 1;
            ctask.type = 1;
            draws.infographics.emplace_back(ctask);
        }

        for (uint32 i = 0, li = text.size(); i < li; i++)
        {
            auto ctask = convert(task);
            uint8 c = text[i] - 32;
            uint32 charPosX = (c & 15) << 4;
            uint32 charPosY = (c >> 4) * 19;

            ctask.data[0] = size;
            ctask.data[2] = 2.0 / windowWidth;
            ctask.data[3] = 2.0 / windowHeight;

            ctask.data2[0] = charPosX;
            ctask.data2[1] = charPosY;
            ctask.data2[2] = x;
            ctask.data2[3] = y;

            switch (c)
            {
            case 1:
            case 7:
            case 12:
            case 14:
            case 27: //:
            case 28: //;
            case 64: //'
            case 73: //i
            case 76: //l
            case 84: //t
                ctask.data[1] = 0.5;// sizeX2;
                x += sizeX2;
                break;

            default:
                ctask.data[1] = 1; // sizeX;
                x += sizeX;
                break;
            }

            ctask.type = 1;
            draws.infographics.emplace_back(ctask);
        }
    }
}

void CameraImpl::renderNodeBox(TraverseNode *trav, const vec4f &color)
{
    assert(trav);
    assert(trav->meta);

    RenderInfographicsTask task;
    task.mesh = map->getMesh("internal://data/meshes/aabb.obj");
    task.mesh->priority = inf1();
    if (!task.ready())
        return;

    const auto &aabbMatrix = [](const vec3 box[2]) -> mat4
    {
        return translationMatrix((box[0] + box[1]) * 0.5)
            * scaleMatrix((box[1] - box[0]) * 0.5);
    };
    if (trav->meta->obb)
    {
        task.model = trav->meta->obb->rotInv
            * aabbMatrix(trav->meta->obb->points);
    }
    else
    {
        task.model = aabbMatrix(trav->meta->aabbPhys);
    }

    task.color = color;
    draws.infographics.emplace_back(convert(task));
}

void CameraImpl::renderNode(TraverseNode *trav)
{
    assert(trav);
    assert(trav->meta);
    assert(trav->surface);
    assert(trav->determined);
    assert(trav->rendersReady());

    trav->lastRenderTime = map->renderTickIndex;
    if (trav->rendersEmpty())
        return;

    // statistics
    statistics.nodesRenderedTotal++;
    statistics.nodesRenderedPerLod[std::min<uint32>(
        trav->id.lod, CameraStatistics::MaxLods - 1)]++;

    // credits
    for (auto &it : trav->credits)
        map->credits->hit(trav->layer->creditScope, it,
            trav->meta->localId.lod);

    // surfaces
    for (const RenderSurfaceTask &r : trav->opaque)
        draws.opaque.emplace_back(convert(r));
    for (const RenderSurfaceTask &r : trav->transparent)
        draws.transparent.emplace_back(convert(r));
    for (const auto &it : trav->geodata)
        draws.geodata.emplace_back(it);
    for (const RenderColliderTask &r : trav->colliders)
        draws.colliders.emplace_back(convert(r));

    // surrogate
    if (options.debugRenderSurrogates && trav->meta->surrogatePhys)
    {
        RenderInfographicsTask task;
        task.mesh = map->getMesh("internal://data/meshes/sphere.obj");
        task.mesh->priority = inf1();
        if (task.ready())
        {
            task.model = translationMatrix(*trav->meta->surrogatePhys)
                * scaleMatrix(trav->meta->extents.size() * 0.03);
            task.color = vec3to4(trav->surface->color, task.color(3));
            draws.infographics.emplace_back(convert(task));
        }
    }

    // mesh box
    if (options.debugRenderMeshBoxes)
    {
        RenderInfographicsTask task;
        task.mesh = map->getMesh("internal://data/meshes/aabb.obj");
        task.mesh->priority = inf1();
        if (task.ready())
        {
            for (RenderSurfaceTask &r : trav->opaque)
            {
                task.model = r.model;
                task.color = vec3to4(trav->surface->color, task.color(3));
                draws.infographics.emplace_back(convert(task));
            }
        }
    }

    // tile box
    if (!options.debugRenderTileDiagnostics && options.debugRenderTileBoxes)
    {
        vec4f color = vec4f(1, 1, 1, 1);
        if (trav->layer->freeLayer)
        {
            switch (trav->layer->freeLayer->type)
            {
            case vtslibs::registry::FreeLayer::Type::meshTiles:
                color = vec4f(1, 0, 0, 1);
                break;
            case vtslibs::registry::FreeLayer::Type::geodataTiles:
                color = vec4f(0, 1, 0, 1);
                break;
            case vtslibs::registry::FreeLayer::Type::geodata:
                color = vec4f(0, 0, 1, 1);
                break;
            default:
                color = vec4f(1, 1, 1, 1);
                break;
            }
        }
        renderNodeBox(trav, color);
    }

    // tile options
    if (!(options.debugRenderTileGeodataOnly && !trav->layer->isGeodata()) && options.debugRenderTileDiagnostics)
    {
        renderNodeBox(trav, vec4f(0, 0, 1, 1));

        char stmp[1024];
        auto id = trav->id;
        float size = options.debugRenderTileBigText ? 12 : 8;

        if (options.debugRenderTileLod)
        {
            sprintf(stmp, "%d", id.lod);
            renderText(trav, 0, 0, vec4f(1, 0, 0, 1), size, stmp);
        }

        if (options.debugRenderTileIndices)
        {
            sprintf(stmp, "%d %d", id.x, id.y);
            renderText(trav, 0, -(size + 2), vec4f(0, 1, 1, 1), size, stmp);
        }

        if (options.debugRenderTileTexelSize)
        {
            sprintf(stmp, "%.2f %.2f",
                trav->meta->texelSize, coarsenessValue(trav));
            renderText(trav, 0, (size + 2), vec4f(1, 0, 1, 1), size, stmp);
        }

        if (options.debugRenderTileFaces)
        {
            uint32 i = 0;
            for (RenderSurfaceTask &r : trav->opaque)
            {
                if (r.mesh.get())
                {
                    sprintf(stmp, "[%d] %d", i++, r.mesh->faces);
                    renderText(trav, 0, (size + 2) * i,
                        vec4f(1, 0, 1, 1), size, stmp);
                }
            }
            for (RenderSurfaceTask &r : trav->transparent)
            {
                if (r.mesh.get())
                {
                    sprintf(stmp, "[%d] %d", i++, r.mesh->faces);
                    renderText(trav, 0, (size + 2) * i,
                        vec4f(1, 0, 1, 1), size, stmp);
                }
            }
        }

        if (options.debugRenderTileTextureSize)
        {
            uint32 i = 0;
            for (RenderSurfaceTask &r : trav->opaque)
            {
                if (r.mesh.get() && r.textureColor.get())
                {
                    sprintf(stmp, "[%d] %dx%d", i++,
                        r.textureColor->width, r.textureColor->height);
                    renderText(trav, 0, (size + 2) * i,
                        vec4f(1, 1, 1, 1), size, stmp);
                }
            }
            for (RenderSurfaceTask &r : trav->transparent)
            {
                if (r.mesh.get() && r.textureColor.get())
                {
                    sprintf(stmp, "[%d] %dx%d", i++,
                        r.textureColor->width, r.textureColor->height);
                    renderText(trav, 0, (size + 2) * i,
                        vec4f(1, 1, 1, 1), size, stmp);
                }
            }
        }

        if (options.debugRenderTileSurface && trav->surface)
        {
            std::string stmp2;
            if (trav->surface->alien)
            {
                renderText(trav, 0, (size + 2),
                    vec4f(1, 1, 1, 1), size, "<Alien>");
            }
            const auto &names = trav->surface->name;
            for (uint32 i = 0, li = names.size(); i < li; i++)
            {
                sprintf(stmp, "[%d] %s", i, names[i].c_str());
                renderText(trav, 0,
                    (size + 2) * (i + (trav->surface->alien ? 1 : 0)),
                    vec4f(1, 1, 1, 1), size, stmp);
            }
        }

        if (options.debugRenderTileBoundLayer)
        {
            uint32 i = 0;
            for (RenderSurfaceTask &r : trav->opaque)
            {
                if (!r.boundLayerId.empty())
                {
                    sprintf(stmp, "[%d] %s", i, r.boundLayerId.c_str());
                    renderText(trav, 0, (size + 2) * (i++),
                        vec4f(1, 1, 1, 1), size, stmp);
                }
            }

            for (RenderSurfaceTask &r : trav->transparent)
            {
                if (!r.boundLayerId.empty())
                {
                    sprintf(stmp, "[%d] %s", i, r.boundLayerId.c_str());
                    renderText(trav, 0, (size + 2) * (i++),
                        vec4f(1, 1, 1, 1), size, stmp);
                }
            }
        }

        if (options.debugRenderTileCredits)
        {
            uint32 i = 0;
            for (auto &it : trav->credits)
            {
                sprintf(stmp, "[%d] %s", i,
                    map->credits->findId(it).c_str());
                renderText(trav, 0, (size + 2) * (i++),
                    vec4f(1, 1, 1, 1), size, stmp);
            }
        }
    }
}

void CameraImpl::renderUpdate()
{
    OPTICK_EVENT();
    clear();

    if (!map->mapconfigReady)
        return;

    updateNavigation(navigation, map->lastElapsedFrameTime);

    if (windowWidth == 0 || windowHeight == 0)
        return;

    // render variables
    viewActual = lookAt(eye, target, up);
    viewProjActual = apiProj * viewActual;
    if (!options.debugDetachedCamera)
    {
        vec3 forward = normalize(vec3(target - eye));
        vec3 off = forward * options.cullingOffsetDistance;
        viewProjCulling = apiProj * lookAt(eye - off, target, up);
        viewProjRender = viewProjActual;
        perpendicularUnitVector
            = normalize(cross(cross(up, forward), forward));
        forwardUnitVector = forward;
        vts::frustumPlanes(viewProjCulling, cullingPlanes);
        cameraPosPhys = eye;
        focusPosPhys = target;
        diskNominalDistance =  windowHeight * apiProj(1, 1) * 0.5;
    }
    else
    {
        // render original camera
        RenderInfographicsTask task;
        task.mesh = map->getMesh("internal://data/meshes/line.obj");
        task.mesh->priority = inf1();
        task.color = vec4f(0, 1, 0, 1);
        if (task.ready())
        {
            std::vector<vec3> corners;
            corners.reserve(8);
            mat4 m = viewProjRender.inverse();
            for (int x = 0; x < 2; x++)
                for (int y = 0; y < 2; y++)
                    for (int z = 0; z < 2; z++)
                        corners.push_back(vec4to3(vec4(m
                           * vec4(x * 2 - 1, y * 2 - 1, z * 2 - 1, 1)), true));
            static const uint32 cora[] = {
                0, 0, 1, 2, 4, 4, 5, 6, 0, 1, 2, 3
            };
            static const uint32 corb[] = {
                1, 2, 3, 3, 5, 6, 7, 7, 4, 5, 6, 7
            };
            for (uint32 i = 0; i < 12; i++)
            {
                vec3 a = corners[cora[i]];
                vec3 b = corners[corb[i]];
                task.model = lookAt(a, b);
                draws.infographics.emplace_back(convert(task));
            }
        }
    }

    // update draws camera
    {
        CameraDraws::Camera &c = draws.camera;
        matToRaw(viewActual, c.view);
        matToRaw(apiProj, c.proj);
        vecToRaw(eye, c.eye);
        c.targetDistance = length(vec3(target - eye));
        c.viewExtent = c.targetDistance / (c.proj[5] * 0.5);

        // altitudes
        {
            vec3 navPos = map->convertor->physToNav(eye);
            c.altitudeOverEllipsoid = navPos[2];
            double tmp;
            if (getSurfaceOverEllipsoid(tmp, navPos))
                c.altitudeOverSurface = c.altitudeOverEllipsoid - tmp;
            else
                c.altitudeOverSurface = nan1();
        }
    }

    // traverse and generate draws
    for (auto &it : map->layers)
    {
        if (it->surfaceStack.surfaces.empty())
            continue;
        OPTICK_EVENT("layer");
        if (!it->freeLayerName.empty())
        {
            OPTICK_TAG("freeLayerName", it->freeLayerName.c_str());
        }
        {
            OPTICK_EVENT("traversal");
            traverseRender(it->traverseRoot.get());
        }
    }
    sortOpaqueFrontToBack();

    // update camera credits
    map->credits->tick(credits);
}

namespace
{

void computeNearFar(double &near_, double &far_, double altitude,
    const MapCelestialBody &body, bool projected,
    vec3 cameraPos, vec3 cameraForward)
{
    (void)cameraForward;
    double major = body.majorRadius;
    double flat = major / body.minorRadius;
    cameraPos[2] *= flat;
    double ground = major + (std::isnan(altitude) ? 0.0 : altitude);
    double l = projected ? cameraPos[2] + major : length(cameraPos);
    double a = std::max(1.0, l - ground);

    if (a > 2 * major)
    {
        near_ = a - major;
        far_ = l;
    }
    else
    {
        double f = std::pow(a / (2 * major), 1.1);
        near_ = interpolate(10.0, major, f);
        far_ = std::sqrt(std::max(0.0, l * l - major * major)) + 0.1 * major;
    }
}

} // namespace

void CameraImpl::suggestedNearFar(double &near_, double &far_)
{
    vec3 navPos = map->convertor->physToNav(eye);
    double altitude;
    if (!getSurfaceOverEllipsoid(altitude, navPos))
        altitude = nan1();
    bool projected = map->mapconfig->navigationSrsType()
        == vtslibs::registry::Srs::Type::projected;
    computeNearFar(near_, far_, altitude, map->body,
        projected, eye, target - eye);
    assert(options.minSuggestedNearClipPlaneDistance > 0);
    assert(options.minSuggestedNearClipPlaneDistance <= options.maxSuggestedNearClipPlaneDistance);
    near_ = std::max(options.minSuggestedNearClipPlaneDistance, std::min(options.maxSuggestedNearClipPlaneDistance, near_));
}

void CameraImpl::sortOpaqueFrontToBack()
{
    OPTICK_EVENT();
    vec3 e = rawToVec3(draws.camera.eye);
    std::sort(draws.opaque.begin(), draws.opaque.end(), [e](
        const DrawSurfaceTask &a, const DrawSurfaceTask &b) {
        vec3 va = rawToVec3(a.center).cast<double>() - e;
        vec3 vb = rawToVec3(b.center).cast<double>() - e;
        return dot(va, va) < dot(vb, vb);
    });
}

} // namespace vts
