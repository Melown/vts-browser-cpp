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

#include <boost/utility/in_place_factory.hpp>

#include "include/vts-browser/map.hpp"
#include "include/vts-browser/exceptions.hpp"
#include "map.hpp"

namespace vts
{

namespace
{

bool testAndThrow(Resource::State state, const std::string &message)
{
    switch (state)
    {
    case Resource::State::errorRetry:
    case Resource::State::downloaded:
    case Resource::State::downloading:
    case Resource::State::finalizing:
    case Resource::State::initializing:
        return false;
    case Resource::State::ready:
        return true;
    default:
        LOGTHROW(err4, MapConfigException) << message;
        throw;
    }
}

vec3 lowerUpperCombine(uint32 i)
{
    vec3 res;
    res(0) = (i >> 0) % 2;
    res(1) = (i >> 1) % 2;
    res(2) = (i >> 2) % 2;
    return res;
}

vec4 column(const mat4 &m, uint32 index)
{
    return vec4(m(index, 0), m(index, 1), m(index, 2), m(index, 3));
}

bool aabbTest(const vec3 aabb[2], const vec4 planes[6])
{
    for (uint32 i = 0; i < 6; i++)
    {
        const vec4 &p = planes[i]; // current plane
        vec3 pv = vec3( // current p-vertex
                aabb[!!(p[0] > 0)](0),
                aabb[!!(p[1] > 0)](1),
                aabb[!!(p[2] > 0)](2));
        double d = dot(vec4to3(p), pv);
        if (d < -p[3])
            return false;
    }
    return true;
}

void frustumPlanes(const mat4 &vp, vec4 planes[6])
{
    vec4 c0 = column(vp, 0);
    vec4 c1 = column(vp, 1);
    vec4 c2 = column(vp, 2);
    vec4 c3 = column(vp, 3);
    planes[0] = c3 + c0;
    planes[1] = c3 - c0;
    planes[2] = c3 + c1;
    planes[3] = c3 - c1;
    planes[4] = c3 + c2;
    planes[5] = c3 - c2;
}

} // namespace

MapImpl::Renderer::Renderer() :
    windowWidth(0), windowHeight(0), tickIndex(0)
{}

void MapImpl::renderInitialize()
{
    LOG(info3) << "Render initialize";
}

void MapImpl::renderFinalize()
{
    LOG(info3) << "Render finalize";
}

void MapImpl::setMapConfigPath(const std::string &mapConfigPath,
                               const std::string &authPath,
                               const std::string &sriPath)
{
    LOG(info3) << "Changing map config path to <" << mapConfigPath << ">, "
               << (!authPath.empty() ? "using" : "without")
               << " authentication and "
               << (!sriPath.empty() ?
                       std::string() + "using SRI <" + sriPath + ">"
                     : "without SRI");
    this->mapConfigPath = mapConfigPath;
    resources.authPath = authPath;
    resources.sriPath = sriPath;
    purgeMapConfig();
}

void MapImpl::purgeMapConfig()
{
    LOG(info2) << "Purge map config";

    if (resources.auth)
        resources.auth->state = Resource::State::finalizing;
    if (mapConfig)
        mapConfig->state = Resource::State::finalizing;

    resources.auth.reset();
    mapConfig.reset();
    renderer.credits.purge();
    resources.searchTasks.clear();
    resetNavigationMode();
    navigation.autoRotation = 0;
    navigation.lastPositionAltitudeShift.reset();
    navigation.positionAltitudeResetHeight.reset();
    body = MapCelestialBody();
    purgeViewCache();
}

void MapImpl::purgeViewCache()
{
    LOG(info2) << "Purge view cache";

    if (mapConfig)
    {
        mapConfig->consolidateView();
        mapConfig->surfaceStack.clear();
    }

    renderer.traverseRoot.reset();
    renderer.tilesetMapping.reset();
    statistics.resetFrame();
    draws = MapDraws();
    credits = MapCredits();
    mapConfigView = "";
    initialized = false;
}

const TileId MapImpl::roundId(TileId nodeId)
{
    uint32 metaTileBinaryOrder = mapConfig->referenceFrame.metaBinaryOrder;
    return TileId (nodeId.lod,
       (nodeId.x >> metaTileBinaryOrder) << metaTileBinaryOrder,
       (nodeId.y >> metaTileBinaryOrder) << metaTileBinaryOrder);
}

Validity MapImpl::reorderBoundLayers(const NodeInfo &nodeInfo,
        uint32 subMeshIndex, BoundParamInfo::List &boundList, double priority)
{
    // prepare all layers
    {
        bool determined = true;
        auto it = boundList.begin();
        while (it != boundList.end())
        {
            switch (it->prepare(nodeInfo, this, subMeshIndex, priority))
            {
            case Validity::Invalid:
                it = boundList.erase(it);
                break;
            case Validity::Indeterminate:
                determined = false;
                // no break here
            case Validity::Valid:
                it++;
            }
        }
        if (!determined)
            return Validity::Indeterminate;
    }
    
    // skip overlapping layers
    std::reverse(boundList.begin(), boundList.end());
    auto it = boundList.begin(), et = boundList.end();
    while (it != et && (!it->watertight || it->transparent))
        it++;
    if (it != et)
        boundList.erase(++it, et);
    std::reverse(boundList.begin(), boundList.end());
    
    return Validity::Valid;
}

void MapImpl::drawRenders(Renders &renders)
{
    for (std::shared_ptr<RenderTask> &r : renders.opaque)
    {
        if (r->ready())
            draws.opaque.emplace_back(r.get(), this);
    }
    for (std::shared_ptr<RenderTask> &r : renders.transparent)
    {
        if (r->ready())
            draws.transparent.emplace_back(r.get(), this);
    }
    for (std::shared_ptr<RenderTask> &r : renders.infographic)
    {
        if (r->ready())
            draws.Infographic.emplace_back(r.get(), this);
    }
}

void MapImpl::touchDraws(const std::shared_ptr<RenderTask> &task)
{
    if (task->meshAgg)
        touchResource(task->meshAgg);
    if (task->textureColor)
        touchResource(task->textureColor);
    if (task->textureMask)
        touchResource(task->textureMask);
}

void MapImpl::touchDraws(Renders &renders)
{
    for (auto &&it : renders.opaque)
        touchDraws(it);
    for (auto &&it : renders.transparent)
        touchDraws(it);
    for (auto &&it : renders.infographic)
        touchDraws(it);
}

bool MapImpl::visibilityTest(const std::shared_ptr<TraverseNode> &trav)
{
    assert(trav->meta);
    // aabb test
    if (!aabbTest(trav->meta->aabbPhys, renderer.frustumPlanes))
        return false;
    // additional obb test
    if (trav->meta->obb)
    {
        TraverseNode::Obb &obb = *trav->meta->obb;
        vec4 planes[6];
        frustumPlanes(renderer.viewProj * obb.rotInv, planes);
        if (!aabbTest(obb.points, planes))
            return false;
    }
    // all tests passed
    return true;
}

bool MapImpl::coarsenessTest(const std::shared_ptr<TraverseNode> &trav)
{
    assert(trav->meta);
    bool applyTexelSize = trav->meta->flags()
            & vtslibs::vts::MetaNode::Flag::applyTexelSize;
    bool applyDisplaySize = trav->meta->flags()
            & vtslibs::vts::MetaNode::Flag::applyDisplaySize;

    if (!applyTexelSize && !applyDisplaySize)
        return false;

    bool result = true;

    if (applyTexelSize)
    {
        vec3 up = renderer.perpendicularUnitVector * trav->meta->texelSize;
        for (const vec3 &c : trav->meta->cornersPhys)
        {
            vec3 c1 = c - up * 0.5;
            vec3 c2 = c1 + up;
            c1 = vec4to3(renderer.viewProj * vec3to4(c1, 1), true);
            c2 = vec4to3(renderer.viewProj * vec3to4(c2, 1), true);
            double len = std::abs(c2[1] - c1[1]) * renderer.windowHeight * 0.5;
            result = result && len < options.maxTexelToPixelScale;
        }
    }

    if (applyDisplaySize)
    {
        result = false; // todo
    }

    return result;
}

void MapImpl::renderNode(const std::shared_ptr<TraverseNode> &trav)
{
    assert(trav->meta);
    assert(visibilityTest(trav));

    // meshes
    if (!options.debugRenderNoMeshes)
        drawRenders(trav->renders);

    // surrogate
    if (options.debugRenderSurrogates)
    {
        RenderTask task;
        task.mesh = getMeshRenderable("internal://data/meshes/sphere.obj");
        task.mesh->priority = std::numeric_limits<float>::infinity();
        task.model = translationMatrix(trav->meta->surrogatePhys)
                * scaleMatrix(trav->nodeInfo.extents().size() * 0.03);
        if (trav->meta->surface)
            task.color = vec3to4f(trav->meta->surface->color, task.color(3));
        if (task.ready())
            draws.Infographic.emplace_back(&task, this);
    }

    // mesh box
    if (options.debugRenderMeshBoxes)
    {
        for (std::shared_ptr<RenderTask> &r : trav->renders.opaque)
        {
            RenderTask task;
            task.model = r->model;
            task.mesh = getMeshRenderable("internal://data/meshes/aabb.obj");
            task.mesh->priority = std::numeric_limits<float>::infinity();
            task.color = vec4f(0, 0, 1, 1);
            if (task.ready())
                draws.Infographic.emplace_back(&task, this);
        }
    }

    // tile box
    if (options.debugRenderTileBoxes)
    {
        RenderTask task;
        task.mesh = getMeshRenderable("internal://data/meshes/line.obj");
        task.mesh->priority = std::numeric_limits<float>::infinity();
        task.color = vec4f(1, 0, 0, 1);
        if (task.ready())
        {
            static const uint32 cora[] = {
                0, 0, 1, 2, 4, 4, 5, 6, 0, 1, 2, 3
            };
            static const uint32 corb[] = {
                1, 2, 3, 3, 5, 6, 7, 7, 4, 5, 6, 7
            };
            for (uint32 i = 0; i < 12; i++)
            {
                vec3 a = trav->meta->cornersPhys[cora[i]];
                vec3 b = trav->meta->cornersPhys[corb[i]];
                task.model = lookAt(a, b);
                draws.Infographic.emplace_back(&task, this);
            }
        }
    }

    // credits
    for (auto &it : trav->meta->credits)
        renderer.credits.hit(Credits::Scope::Imagery, it,
                             trav->nodeInfo.distanceFromRoot());

    // statistics
    statistics.meshesRenderedTotal++;
    statistics.meshesRenderedPerLod[std::min<uint32>(
        trav->nodeInfo.nodeId().lod, MapStatistics::MaxLods-1)]++;
}

bool MapImpl::travDetermineMeta(const std::shared_ptr<TraverseNode> &trav)
{
    assert(!trav->meta);
    assert(trav->childs.empty());
    assert(trav->renders.empty());

    if (statistics.currentNodeMetaUpdates++
            >= options.maxNodeMetaUpdatesPerTick)
        return false;

    const TileId nodeId = trav->nodeInfo.nodeId();

    // find all metatiles
    std::vector<std::shared_ptr<MetaTile>> metaTiles;
    metaTiles.resize(mapConfig->surfaceStack.size());
    const UrlTemplate::Vars tileIdVars(roundId(nodeId));
    bool determined = true;
    for (uint32 i = 0, e = metaTiles.size(); i != e; i++)
    {
        if (trav->parent)
        {
            const std::shared_ptr<MetaTile> &p
                    = trav->parent->meta->metaTiles[i];
            if (!p)
                continue;
            TileId pid = vtslibs::vts::parent(nodeId);
            uint32 idx = (nodeId.x % 2) + (nodeId.y % 2) * 2;
            const vtslibs::vts::MetaNode &node = p->get(pid);
            if ((node.flags()
                 & (vtslibs::vts::MetaNode::Flag::ulChild << idx)) == 0)
                continue;
        }
        auto m = getMetaTile(mapConfig->surfaceStack[i].surface
                             ->urlMeta(tileIdVars));
        m->updatePriority(trav->priority);
        switch (getResourceValidity(m))
        {
        case Validity::Indeterminate:
            determined = false;
            // no break here
        case Validity::Invalid:
            continue;
        case Validity::Valid:
            break;
        }
        metaTiles[i] = m;
    }
    if (!determined)
        return false;

    // find topmost nonempty surface
    MapConfig::SurfaceStackItem *topmost = nullptr;
    const vtslibs::vts::MetaNode *node = nullptr;
    bool childsAvailable[4] = {false, false, false, false};
    for (uint32 i = 0, e = metaTiles.size(); i != e; i++)
    {
        if (!metaTiles[i])
            continue;
        const vtslibs::vts::MetaNode &n = metaTiles[i]->get(nodeId);
        for (uint32 i = 0; i < 4; i++)
            childsAvailable[i] = childsAvailable[i]
                    || (n.childFlags()
                        & (vtslibs::vts::MetaNode::Flag::ulChild << i));
        if (topmost || n.alien() != mapConfig->surfaceStack[i].alien)
            continue;
        if (n.geometry())
        {
            node = &n;
            if (renderer.tilesetMapping)
            {
                assert(n.sourceReference > 0 && n.sourceReference
                       <= renderer.tilesetMapping->surfaceStack.size());
                topmost = &renderer.tilesetMapping
                        ->surfaceStack[n.sourceReference];
            }
            else
                topmost = &mapConfig->surfaceStack[i];
        }
        if (!node)
            node = &n;
    }

    assert(node);
    trav->meta = boost::in_place(*node);
    trav->meta->metaTiles.swap(metaTiles);

    // corners
    if (!vtslibs::vts::empty(node->geomExtents)
            && !trav->nodeInfo.srs().empty()
            && !options.debugDisableMeta5)
    {
        vec2 fl = vecFromUblas<vec2>(trav->nodeInfo.extents().ll);
        vec2 fu = vecFromUblas<vec2>(trav->nodeInfo.extents().ur);
        vec3 el = vec2to3(fl, node->geomExtents.z.min);
        vec3 eu = vec2to3(fu, node->geomExtents.z.max);
        vec3 *corners = trav->meta->cornersPhys;
        for (uint32 i = 0; i < 8; i++)
        {
            vec3 f = lowerUpperCombine(i).cwiseProduct(eu - el) + el;
            f = convertor->convert(f, trav->nodeInfo.srs(),
                        mapConfig->referenceFrame.model.physicalSrs);
            corners[i] = f;
        }

        // obb
        if (trav->nodeInfo.distanceFromRoot() > 4)
        {
            vec3 center = vec3(0,0,0);
            for (uint32 i = 0; i < 8; i++)
                center += corners[i];
            center /= 8;

            vec3 f = corners[4] - corners[0];
            vec3 u = corners[2] - corners[0];
            mat4 t = lookAt(center, center + f, u);

            TraverseNode::Obb obb;
            obb.rotInv = t.inverse();
            double di = std::numeric_limits<double>::infinity();
            vec3 vi(di, di, di);
            obb.points[0] = vi;
            obb.points[1] = -vi;

            for (uint32 i = 0; i < 8; i++)
            {
                vec3 p = vec4to3(t * vec3to4(corners[i], 1), false);
                obb.points[0] = min(obb.points[0], p);
                obb.points[1] = max(obb.points[1], p);
            }

            trav->meta->obb = obb;
        }
    }
    else if (node->extents.ll != node->extents.ur)
    {
        vec3 fl = vecFromUblas<vec3>(node->extents.ll);
        vec3 fu = vecFromUblas<vec3>(node->extents.ur);
        vec3 el = vecFromUblas<vec3>
                (mapConfig->referenceFrame.division.extents.ll);
        vec3 eu = vecFromUblas<vec3>
                (mapConfig->referenceFrame.division.extents.ur);
        for (uint32 i = 0; i < 8; i++)
        {
            vec3 f = lowerUpperCombine(i).cwiseProduct(fu - fl) + fl;
            trav->meta->cornersPhys[i] = f.cwiseProduct(eu - el) + el;
        }
    }

    // aabb
    if (trav->nodeInfo.distanceFromRoot() > 2)
    {
        trav->meta->aabbPhys[0]
                = trav->meta->aabbPhys[1]
                = trav->meta->cornersPhys[0];
        for (const vec3 &it : trav->meta->cornersPhys)
        {
            trav->meta->aabbPhys[0] = min(trav->meta->aabbPhys[0], it);
            trav->meta->aabbPhys[1] = max(trav->meta->aabbPhys[1], it);
        }
    }

    // surrogate
    if (vtslibs::vts::GeomExtents::validSurrogate(
                node->geomExtents.surrogate))
    {
        vec2 exU = vecFromUblas<vec2>(trav->nodeInfo.extents().ur);
        vec2 exL = vecFromUblas<vec2>(trav->nodeInfo.extents().ll);
        vec3 sds = vec2to3((exU + exL) * 0.5,
                           node->geomExtents.surrogate);
        trav->meta->surrogatePhys = convertor->convert(sds,
                            trav->nodeInfo.srs(),
                            mapConfig->referenceFrame.model.physicalSrs);
    }

    // surface
    if (topmost)
    {
        trav->meta->surface = topmost;
        // credits
        for (auto it : node->credits())
            trav->meta->credits.push_back(it);
    }

    // prepare children
    vtslibs::vts::Children childs = vtslibs::vts::children(nodeId);
    for (uint32 i = 0; i < 4; i++)
    {
        if (childsAvailable[i])
            trav->childs.push_back(std::make_shared<TraverseNode>(
                        trav.get(), trav->nodeInfo.child(childs[i])));
    }

    return true;
}

bool MapImpl::travDetermineDraws(const std::shared_ptr<TraverseNode> &trav)
{
    assert(trav->meta);
    assert(trav->meta->surface);
    assert(trav->renders.empty());

    if (statistics.currentNodeDrawsUpdates++
            >= options.maxNodeDrawsUpdatesPerTick)
        return false;

    const TileId nodeId = trav->nodeInfo.nodeId();

    // aggregate mesh
    std::string meshAggName = trav->meta->surface->surface->urlMesh(
            UrlTemplate::Vars(nodeId, vtslibs::vts::local(trav->nodeInfo)));
    std::shared_ptr<MeshAggregate> meshAgg = getMeshAggregate(meshAggName);
    meshAgg->updatePriority(trav->priority);
    switch (getResourceValidity(meshAggName))
    {
    case Validity::Invalid:
        trav->meta->surface = nullptr;
        // no break here
    case Validity::Indeterminate:
        return false;
    case Validity::Valid:
        break;
    }

    bool determined = true;
    Renders newRenders;
    std::vector<vtslibs::registry::CreditId> newCredits;

    // iterate over all submeshes
    for (uint32 subMeshIndex = 0, e = meshAgg->submeshes.size();
         subMeshIndex != e; subMeshIndex++)
    {
        const MeshPart &part = meshAgg->submeshes[subMeshIndex];
        std::shared_ptr<GpuMesh> mesh = part.renderable;

        // external bound textures
        if (part.externalUv)
        {
            std::string surfaceName;
            if (trav->meta->surface->surface->name.size() > 1)
                surfaceName = trav->meta->surface->surface
                        ->name[part.surfaceReference - 1];
            else
                surfaceName = trav->meta->surface->surface->name.back();
            const vtslibs::registry::View::BoundLayerParams::list &boundList
                    = mapConfig->view.surfaces[surfaceName];
            BoundParamInfo::List bls(boundList.begin(), boundList.end());
            if (part.textureLayer)
            {
                bls.push_back(BoundParamInfo(
                        vtslibs::registry::View::BoundLayerParams(
                        mapConfig->boundLayers.get(part.textureLayer).id)));
            }
            switch (reorderBoundLayers(trav->nodeInfo, subMeshIndex,
                                       bls, trav->priority))
            {
            case Validity::Indeterminate:
                determined = false;
                // no break here
            case Validity::Invalid:
                continue;
            case Validity::Valid:
                break;
            }
            bool allTransparent = true;
            for (BoundParamInfo &b : bls)
            {
                // credits
                {
                    MapConfig::BoundInfo *l = b.bound;
                    assert(l);
                    for (auto &it : l->credits)
                    {
                        auto c = renderer.credits.find(it.first);
                        if (c)
                            newCredits.push_back(*c);
                    }
                }

                // draw task
                std::shared_ptr<RenderTask> task
                        = std::make_shared<RenderTask>();
                task->meshAgg = meshAgg;
                task->mesh = mesh;
                task->model = part.normToPhys;
                task->uvm = b.uvMatrix();
                task->textureColor = getTexture(b.bound->urlExtTex(b.vars));
                task->textureColor->updatePriority(trav->priority);
                task->textureColor->availTest = b.bound->availability;
                task->externalUv = true;
                task->color(3) = b.alpha ? *b.alpha : 1;
                if (!b.watertight)
                {
                    task->textureMask = getTexture(b.bound->urlMask(b.vars));
                    task->textureMask->updatePriority(trav->priority);
                }
                if (b.transparent)
                    newRenders.transparent.push_back(task);
                else
                    newRenders.opaque.push_back(task);
                allTransparent = allTransparent && b.transparent;
            }
            if (!allTransparent)
                continue;
        }

        // internal texture
        if (part.internalUv)
        {
            UrlTemplate::Vars vars(nodeId,
                    vtslibs::vts::local(trav->nodeInfo), subMeshIndex);
            std::shared_ptr<RenderTask> task = std::make_shared<RenderTask>();
            task->meshAgg = meshAgg;
            task->mesh = mesh;
            task->model = part.normToPhys;
            task->uvm = identityMatrix3().cast<float>();
            task->textureColor = getTexture(
                        trav->meta->surface->surface->urlIntTex(vars));
            task->textureColor->updatePriority(trav->priority);
            task->externalUv = false;
            newRenders.opaque.insert(newRenders.opaque.begin(), task);
        }
    }

    if (determined)
    {
        assert(trav->renders.empty());
        std::swap(trav->renders, newRenders);
        trav->meta->credits.insert(trav->meta->credits.end(),
                             newCredits.begin(), newCredits.end());
    }

    return determined;
}

void MapImpl::traverse(const std::shared_ptr<TraverseNode> &trav, bool loadOnly)
{
    // statistics
    statistics.metaNodesTraversedTotal++;
    statistics.metaNodesTraversedPerLod[
            std::min<uint32>(trav->nodeInfo.nodeId().lod,
                             MapStatistics::MaxLods-1)]++;

    // prepare meta data
    trav->lastAccessTime = renderer.tickIndex;
    if (!trav->meta && !travDetermineMeta(trav))
        return;
    assert(trav->meta);

    if (options.traverseMode == TraverseMode::Hierarchical)
    {
        // hierarchical traversal

        touchDraws(trav->renders);

        if (trav->meta->surface && trav->renders.empty())
            travDetermineDraws(trav);

        if (!visibilityTest(trav))
            return;

        if (coarsenessTest(trav))
        {
            if (!loadOnly)
                renderNode(trav);
            return;
        }

        if (!loadOnly)
        {
            bool ok = true;
            for (std::shared_ptr<TraverseNode> &t : trav->childs)
            {
                if (!t->meta)
                {
                    ok = false;
                    continue;
                }
                if (t->meta->surface && (t->renders.empty() || !t->ready()))
                    ok = false;
            }
            if (!ok)
            {
                renderNode(trav);
                loadOnly = true;
            }
        }

        if (!trav->childs.empty())
        {
            for (std::shared_ptr<TraverseNode> &t : trav->childs)
            {
                t->priority = t->meta ? computeResourcePriority(t)
                                      : trav->priority;
                renderer.traverseQueue.emplace(t, loadOnly);
            }
            return;
        }

        if (!loadOnly)
            renderNode(trav);
    }
    else
    {
        // flat traversal

        if (!visibilityTest(trav))
            return;

        touchDraws(trav->renders);

        if (coarsenessTest(trav) || trav->childs.empty())
        {
            if (!loadOnly)
            {
                if (trav->meta->surface && trav->renders.empty())
                    travDetermineDraws(trav);
                renderNode(trav);
            }
            return;
        }

        for (std::shared_ptr<TraverseNode> &t : trav->childs)
        {
            t->priority = t->meta ? computeResourcePriority(t)
                                  : trav->priority;
            renderer.traverseQueue.emplace(t, loadOnly);
        }
    }
}

void MapImpl::traverseClearing(const std::shared_ptr<TraverseNode> &trav)
{
    TileId id = trav->nodeInfo.nodeId();
    if (id.lod == 3)
    {
        if ((id.y * 8 + id.x) % 64 != renderer.tickIndex % 64)
            return;
    }
    
    if (trav->lastAccessTime + 5 < renderer.tickIndex)
    {
        trav->clear();
        return;
    }
    
    for (auto &&it : trav->childs)
        traverseClearing(it);
}

void MapImpl::updateCamera()
{
    vec3 center, dir, up;
    positionToCamera(center, dir, up);

    vtslibs::registry::Position &pos = mapConfig->position;

    // camera view matrix
    double dist = pos.type == vtslibs::registry::Position::Type::objective
            ? positionObjectiveDistance() : 1e-5;
    vec3 cameraPosPhys = center - dir * dist;
    if (callbacks.cameraOverrideEye)
        callbacks.cameraOverrideEye((double*)&cameraPosPhys);
    if (callbacks.cameraOverrideTarget)
        callbacks.cameraOverrideTarget((double*)&center);
    if (callbacks.cameraOverrideUp)
        callbacks.cameraOverrideUp((double*)&up);
    mat4 view = lookAt(cameraPosPhys, center, up);
    if (callbacks.cameraOverrideView)
    {
        callbacks.cameraOverrideView((double*)&view);
        // update dir and up
        mat4 vi = view.inverse();
        cameraPosPhys = vec4to3(vi * vec4(0, 0, -1, 1), true);
        dir = vec4to3(vi * vec4(0, 0, -1, 0), false);
        up = vec4to3(vi * vec4(0, 1, 0, 0), false);
        center = cameraPosPhys + dir * dist;
    }

    // camera projection matrix
    double near = std::max(2.0, dist * 0.1);
    double terrainAboveOrigin = 0;
    double cameraAboveOrigin = 0;
    switch (mapConfig->navigationSrsType())
    {
    case vtslibs::registry::Srs::Type::projected:
    {
        const vtslibs::registry::Srs &srs = mapConfig->srs.get(
                    mapConfig->referenceFrame.model.navigationSrs);
        if (srs.periodicity)
            terrainAboveOrigin = srs.periodicity->period / (2 * 3.14159);
        cameraAboveOrigin = terrainAboveOrigin + dist * 2;
    } break;
    case vtslibs::registry::Srs::Type::geographic:
    {
        terrainAboveOrigin = length(convertor->navToPhys(vec2to3(vec3to2(
                                    vecFromUblas<vec3>(pos.position)), 0)));
        cameraAboveOrigin = length(cameraPosPhys);
    } break;
    case vtslibs::registry::Srs::Type::cartesian:
        LOGTHROW(fatal, std::invalid_argument) << "Invalid navigation srs type";
    }
    double cameraToHorizon = cameraAboveOrigin > terrainAboveOrigin
            ? std::sqrt(cameraAboveOrigin * cameraAboveOrigin
                - terrainAboveOrigin * terrainAboveOrigin)
            : 0;
    double mountains = 5000 + terrainAboveOrigin;
    double mountainsBehindHorizon = std::sqrt(mountains * mountains
                                    - terrainAboveOrigin * terrainAboveOrigin);
    double far = cameraToHorizon + mountainsBehindHorizon;
    double fov = pos.verticalFov;
    double aspect = (double)renderer.windowWidth/(double)renderer.windowHeight;
    if (callbacks.cameraOverrideFovAspectNearFar)
        callbacks.cameraOverrideFovAspectNearFar(fov, aspect, near, far);
    assert(fov > 1e-3 && fov < 180 - 1e-3);
    assert(aspect > 0);
    assert(near > 0);
    assert(far > near);
    mat4 proj = perspectiveMatrix(fov, aspect, near, far);
    if (callbacks.cameraOverrideProj)
        callbacks.cameraOverrideProj((double*)&proj);

    // few other variables
    renderer.viewProjRender = proj * view;
    if (!options.debugDetachedCamera)
    {
        renderer.viewProj = renderer.viewProjRender;
        renderer.perpendicularUnitVector
                = normalize(cross(cross(up, dir), dir));
        renderer.forwardUnitVector = dir;
        frustumPlanes(renderer.viewProj, renderer.frustumPlanes);
        renderer.cameraPosPhys = cameraPosPhys;
        renderer.focusPosPhys = center;
    }
    else
    {
        // render original camera
        RenderTask task;
        task.mesh = getMeshRenderable("internal://data/meshes/line.obj");
        task.mesh->priority = std::numeric_limits<float>::infinity();
        task.color = vec4f(0, 1, 0, 1);
        if (task.ready())
        {
            std::vector<vec3> corners;
            corners.reserve(8);
            mat4 m = renderer.viewProj.inverse();
            for (int x = 0; x < 2; x++)
                for (int y = 0; y < 2; y++)
                    for (int z = 0; z < 2; z++)
                        corners.push_back(vec4to3(m
                            * vec4(x * 2 - 1, y * 2 - 1, z * 2 - 1, 1), true));
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
                draws.Infographic.emplace_back(&task, this);
            }
        }
    }

    // render object position
    if (options.debugRenderObjectPosition)
    {
        vec3 phys = convertor->navToPhys(vecFromUblas<vec3>(pos.position));
        RenderTask r;
        r.mesh = getMeshRenderable("internal://data/meshes/cube.obj");
        r.mesh->priority = std::numeric_limits<float>::infinity();
        r.textureColor = getTexture("internal://data/textures/helper.jpg");
        r.textureColor->priority = std::numeric_limits<float>::infinity();
        r.model = translationMatrix(phys)
                * scaleMatrix(pos.verticalExtent * 0.015);
        if (r.ready())
            draws.Infographic.emplace_back(&r, this);
    }
    
    // render target position
    if (options.debugRenderTargetPosition)
    {
        vec3 phys = convertor->navToPhys(navigation.targetPoint);
        RenderTask r;
        r.mesh = getMeshRenderable("internal://data/meshes/cube.obj");
        r.mesh->priority = std::numeric_limits<float>::infinity();
        r.textureColor = getTexture("internal://data/textures/helper.jpg");
        r.textureColor->priority = std::numeric_limits<float>::infinity();
        r.model = translationMatrix(phys)
                * scaleMatrix(navigation.targetViewExtent * 0.015);
        if (r.ready())
            draws.Infographic.emplace_back(&r, this);
    }
}

bool MapImpl::prerequisitesCheck()
{
    if (resources.auth)
    {
        resources.auth->checkTime();
        touchResource(resources.auth);
    }

    if (mapConfig)
        touchResource(mapConfig);

    if (renderer.tilesetMapping)
        touchResource(renderer.tilesetMapping);

    if (initialized)
        return true;

    if (mapConfigPath.empty())
        return false;

    if (!resources.authPath.empty())
    {
        resources.auth = getAuthConfig(resources.authPath);
        if (!testAndThrow(resources.auth->state, "Authentication failure."))
            return false;
    }

    mapConfig = getMapConfig(mapConfigPath);
    if (!testAndThrow(mapConfig->state, "Map config failure."))
        return false;

    // check for virtual surface
    if (!options.debugDisableVirtualSurfaces)
    {
        std::vector<std::string> viewSurfaces;
        viewSurfaces.reserve(mapConfig->view.surfaces.size());
        for (auto &&it : mapConfig->view.surfaces)
            viewSurfaces.push_back(it.first);
        std::sort(viewSurfaces.begin(), viewSurfaces.end());
        for (vtslibs::vts::VirtualSurfaceConfig &it :mapConfig->virtualSurfaces)
        {
            std::vector<std::string> virtSurfaces(it.id.begin(), it.id.end());
            if (virtSurfaces.size() != viewSurfaces.size())
                continue;
            std::vector<std::string> virtSurfaces2(virtSurfaces);
            std::sort(virtSurfaces2.begin(), virtSurfaces2.end());
            if (!boost::algorithm::equals(viewSurfaces, virtSurfaces2))
                continue;
            renderer.tilesetMapping = getTilesetMapping(MapConfig::convertPath(
                                                it.mapping, mapConfig->name));
            if (!testAndThrow(renderer.tilesetMapping->state,
                              "Tileset mapping failure."))
                return false;
            mapConfig->generateSurfaceStack(&it);
            renderer.tilesetMapping->update(virtSurfaces);
            break;
        }
    }

    if (mapConfig->surfaceStack.empty())
        mapConfig->generateSurfaceStack();

    renderer.traverseRoot = std::make_shared<TraverseNode>(nullptr, NodeInfo(
                    mapConfig->referenceFrame, TileId(), false, *mapConfig));
    renderer.traverseRoot->priority = std::numeric_limits<double>::infinity();
    renderer.credits.merge(mapConfig.get());
    initializeNavigation();
    mapConfig->initializeCelestialBody();

    LOG(info3) << "Map config ready";
    initialized = true;
    if (callbacks.mapconfigReady)
        callbacks.mapconfigReady();    
    return initialized;
}

void MapImpl::renderTickPrepare()
{
    if (!prerequisitesCheck())
        return;

    assert(!resources.auth || *resources.auth);
    assert(mapConfig && *mapConfig);
    assert(convertor);
    assert(renderer.traverseRoot);

    updateNavigation();
    updateSearch();
    updateSris();
    traverseClearing(renderer.traverseRoot);
}

void MapImpl::renderTickRender()
{
    draws.clear();

    if (!initialized || mapConfig->surfaceStack.empty()
            || renderer.windowWidth == 0 || renderer.windowHeight == 0)
        return;

    updateCamera();
    emptyTraverseQueue();
    renderer.traverseQueue.emplace(renderer.traverseRoot, false);
    while (!renderer.traverseQueue.empty())
    {
        TraverseQueueItem t = renderer.traverseQueue.top();
        renderer.traverseQueue.pop();
        traverse(t.trav, t.loadOnly);
    }
    renderer.credits.tick(credits);
    drawRenders(navigation.renders);
}

} // namespace vts
