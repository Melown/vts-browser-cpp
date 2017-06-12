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

#ifndef RESOURCES_H_ergiuhdusgju
#define RESOURCES_H_ergiuhdusgju

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vts-libs/vts/mapconfig.hpp>
#include <vts-libs/vts/urltemplate.hpp>
#include <vts-libs/vts/metatile.hpp>

#include "include/vts-browser/resources.hpp"
#include "include/vts-browser/math.hpp"
#include "fetchTask.hpp"

namespace vts
{

class Resource
{
public:
    Resource();
    virtual ~Resource();
    virtual void load() = 0;
    operator bool () const;
    
    ResourceInfo info;
    std::shared_ptr<FetchTaskImpl> fetch;
    
    float priority;
    float priorityCopy;
    uint32 lastAccessTick;
};

class GpuMesh : public Resource
{
public:
    void load() override;
};

class GpuTexture : public Resource
{
public:
    void load() override;
};

class AuthConfig : public Resource
{
public:
    AuthConfig();
    void load() override;
    void checkTime();
    void authorize(FetchTaskImpl *task);

private:
    std::string token;
    std::unordered_set<std::string> hostnames;
    uint64 timeValid;
    uint64 timeParsed;
};

class MapConfig : public Resource, public vtslibs::vts::MapConfig
{
public:
    class BoundInfo : public vtslibs::registry::BoundLayer
    {
    public:
        BoundInfo(const BoundLayer &layer);
    
        vtslibs::vts::UrlTemplate urlExtTex;
        vtslibs::vts::UrlTemplate urlMeta;
        vtslibs::vts::UrlTemplate urlMask;
    };
    
    class SurfaceInfo : public vtslibs::vts::SurfaceCommonConfig
    {
    public:
        SurfaceInfo(const SurfaceCommonConfig &surface,
                    const std::string &parentPath);
    
        vtslibs::vts::UrlTemplate urlMeta;
        vtslibs::vts::UrlTemplate urlMesh;
        vtslibs::vts::UrlTemplate urlIntTex;
        vtslibs::vts::UrlTemplate urlNav;
        vtslibs::vts::TilesetIdList name;
    };
    
    class SurfaceStackItem
    {
    public:
        SurfaceStackItem();
        
        std::shared_ptr<SurfaceInfo> surface;
        vec3f color;
        bool alien;
    };
    
    class BrowserOptions
    {
    public:
        BrowserOptions();
        
        double autorotate;
    };
    
    MapConfig();
    void load() override;
    void clear();
    static const std::string convertPath(const std::string &path,
                                         const std::string &parent);
    vtslibs::registry::Srs::Type navigationType() const;
    
    vtslibs::vts::SurfaceCommonConfig *findGlue(
            const vtslibs::vts::Glue::Id &id);
    vtslibs::vts::SurfaceCommonConfig *findSurface(const std::string &id);
    BoundInfo *getBoundInfo(const std::string &id);
    void printSurfaceStack();
    void generateSurfaceStack();
    
    std::unordered_map<std::string, std::shared_ptr<SurfaceInfo>> surfaceInfos;
    std::unordered_map<std::string, std::shared_ptr<BoundInfo>> boundInfos;
    std::vector<SurfaceStackItem> surfaceStack;
    BrowserOptions browserOptions;
};

class ExternalBoundLayer : public Resource,
        public vtslibs::registry::BoundLayer
{
public:
    ExternalBoundLayer();
    void load() override;
};

class BoundMetaTile : public Resource
{
public:
    void load() override;

    uint8 flags[vtslibs::registry::BoundLayer::rasterMetatileWidth
                * vtslibs::registry::BoundLayer::rasterMetatileHeight];
};

class BoundMaskTile : public Resource
{
public:
    void load() override;

    std::shared_ptr<GpuTexture> texture;
};

class MetaTile : public Resource, public vtslibs::vts::MetaTile
{
public:
    MetaTile();
    void load() override;
};

class MeshPart
{
public:
    MeshPart();
    std::shared_ptr<GpuMesh> renderable;
    mat4 normToPhys;
    uint32 textureLayer;
    uint32 surfaceReference;
    bool internalUv;
    bool externalUv;
};

class MeshAggregate : public Resource
{
public:
    void load() override;

    std::vector<MeshPart> submeshes;
};

class NavTile : public Resource
{
public:
    void load() override;
    
    std::vector<unsigned char> data;
    
    static vec2 sds2px(const vec2 &point, const math::Extents2 &extents);
};

class SearchTaskImpl : public Resource
{
public:
    void load() override;
    
    Buffer data;
};

} // namespace vts

#endif