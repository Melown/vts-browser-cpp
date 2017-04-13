#include <utility/path.hpp>
#include <vts/map.hpp>

#include "map.hpp"

namespace vts
{

namespace
{

template<class T>
std::shared_ptr<T> getMapResource(const std::string &name, MapImpl *map)
{
    auto it = map->resources.resources.find(name);
    if (it == map->resources.resources.end())
    {
        map->resources.resources[name] = std::make_shared<T>(name);
        it = map->resources.resources.find(name);
    }
    map->touchResource(it->second);
    return std::dynamic_pointer_cast<T>(it->second);
}

} // namespace

MapImpl::Resources::Resources(const std::string &cachePathVal,
                              bool keepInvalidUrls)
    : takeItemIndex(0), downloads(0), cachePath(cachePathVal),
    destroyTheFetcher(false), fetcher(nullptr)
{
    if (cachePath.empty())
    {
        cachePath = utility::homeDir().string();
        if (cachePath.empty())
            throw std::runtime_error("invalid home dir, "
                                     "the cache path must be defined");
        cachePath += "/.cache/vts-browser/";
    }
    if (cachePath.back() != '/')
        cachePath += "/";
    try
    {
        if (keepInvalidUrls && boost::filesystem::exists(
                    cachePath + invalidUrlFileName))
        {
            std::ifstream f;
            f.open(cachePath + invalidUrlFileName);
            while (f.good())
            {
                std::string line;
                std::getline(f, line);
                invalidUrl.insert(line);
            }
            f.close();
        }
    }
    catch (...)
    {}
}

MapImpl::Resources::~Resources()
{
    if (destroyTheFetcher)
    {
        delete fetcher;
        fetcher = nullptr;
        destroyTheFetcher = false;
    }
    try
    {
        std::ofstream f;
        f.open(cachePath + invalidUrlFileName);
        for (auto &&line : invalidUrl)
            f << line << '\n';
        f.close();
    }
    catch (...)
    {}
}

void MapImpl::dataInitialize(Fetcher *fetcher)
{
    if (!fetcher)
    {
        resources.destroyTheFetcher = true;
        fetcher = Fetcher::create();
    }
    resources.fetcher = fetcher;
    Fetcher::Func func = std::bind(
                &MapImpl::fetchedFile,
                this, std::placeholders::_1);
    fetcher->initialize(func);
}

void MapImpl::dataFinalize()
{
    resources.fetcher->finalize();
    resources.prepareQue.clear();
}

void MapImpl::loadResource(ResourceImpl *r)
{
    assert(r->contentData.size() > 0);
    statistics.resourcesProcessLoaded++;
    try
    {
        r->resource->load(this);
        r->state = ResourceImpl::State::ready;
    }
    catch (std::runtime_error &)
    {
        LOG(err4) << "Error loading resource: " + r->resource->name;
        statistics.resourcesFailed++;
        r->state = ResourceImpl::State::errorLoad;
    }
    r->contentData.free();
}

bool MapImpl::dataTick()
{
    statistics.currentDownloads = resources.downloads;
    
    { // sync invalid urls
        boost::lock_guard<boost::mutex> l(resources.mutInvalidUrls);
        resources.invalidUrl.insert(resources.invalidUrlNew.begin(),
                                    resources.invalidUrlNew.end());
        resources.invalidUrlNew.clear();
    }
    
    ResourceImpl *r = nullptr;
    {
        boost::lock_guard<boost::mutex> l(resources.mutPrepareQue);
        if (!resources.prepareQue.empty())
        {
            auto it = std::next(resources.prepareQue.begin(),
                                resources.takeItemIndex++
                                % resources.prepareQue.size());
            r = *it;
            resources.prepareQue.erase(it);
        }
    }
    if (!r)
        return true; // sleep
    
    if (r->state == ResourceImpl::State::downloaded)
    {
        loadResource(r);
        return false;
    }
    
    if (r->state == ResourceImpl::State::initializing)
    {
        if (resources.invalidUrl.find(r->resource->name)
                != resources.invalidUrl.end())
        {
            statistics.resourcesIgnored++;
            r->state = ResourceImpl::State::errorLoad;
            return false;
        }
        
        r->state = ResourceImpl::State::downloading;
        
        if (r->resource->name.find("://") == std::string::npos)
        {
            r->loadFromInternalMemory();
            loadResource(r);
        }
        else if (r->resource->name.find(".json") == std::string::npos
                 && availableInCache(r->resource->name))
        {
            r->loadFromCache(this);
            loadResource(r);
            statistics.resourcesDiskLoaded++;
        }
        else if (resources.downloads < options.maxConcurrentDownloads)
        {
            resources.fetcher->fetch(r);
            statistics.resourcesDownloaded++;
            resources.downloads++;
        }
        else
        {
            r->state = ResourceImpl::State::initializing;
            return true; // sleep
        }
        
        return false;
    }
    
    return true; // sleep
}

void MapImpl::fetchedFile(FetchTask *task)
{
    ResourceImpl *resource = dynamic_cast<ResourceImpl*>(task);
    assert(resource->state == ResourceImpl::State::downloading);
    
    // handle error codes
    if (task->code >= 400 || task->code == 0)
        resource->state = ResourceImpl::State::errorDownload;
    
    // availability tests
    if (resource->state == ResourceImpl::State::downloading
            && resource->availTest)
    {
        switch (resource->availTest->type)
        {
        case vtslibs::registry::BoundLayer
        ::Availability::Type::negativeCode:
            if (resource->availTest->codes.find(task->code)
                    == resource->availTest->codes.end())
                resource->state = ResourceImpl::State::errorDownload;
            break;
        case vtslibs::registry::BoundLayer
        ::Availability::Type::negativeType:
            if (resource->availTest->mime == task->contentType)
                resource->state = ResourceImpl::State::errorDownload;
            break;
        case vtslibs::registry::BoundLayer
        ::Availability::Type::negativeSize:
            if (task->contentData.size() <= resource->availTest->size)
                resource->state = ResourceImpl::State::errorDownload;
        default:
            throw std::invalid_argument("invalid availability test type");
        }
    }
    
    // handle redirections
    if (resource->state == ResourceImpl::State::downloading)
    {
        switch (task->code)
        {
        case 301:
        case 302:
        case 303:
        case 307:
        case 308:
            if (task->redirectionsCount++ > 5)
                resource->state = ResourceImpl::State::errorDownload;
            else
            {
                task->url = task->redirectUrl;
                resources.fetcher->fetch(task);
                return;
            }
        }
    }
    
    resources.downloads--;
    
    if (resource->state == ResourceImpl::State::errorDownload)
    {
        resource->contentData.free();
        boost::lock_guard<boost::mutex> l(resources.mutInvalidUrls);
        resources.invalidUrlNew.insert(resource->resource->name);
        return;
    }
    
    resource->saveToCache(this);
    resource->state = ResourceImpl::State::downloaded;
}

void MapImpl::dataRenderInitialize()
{}

void MapImpl::dataRenderFinalize()
{
    resources.prepareQueNew.clear();
    //LOG(info3) << "Releasing " << resources.resources.size() << " resources";
    resources.resources.clear();
}

bool MapImpl::dataRenderTick()
{
    { // sync download queue
        boost::lock_guard<boost::mutex> l(resources.mutPrepareQue);
        std::swap(resources.prepareQueNew, resources.prepareQue);
    }
    resources.prepareQueNew.clear();
    
    { // clear old resources
        std::vector<Resource*> res;
        res.reserve(resources.resources.size());
        uint32 memRamUse = 0;
        uint32 memGpuUse = 0;
        for (auto &&it : resources.resources)
        {
            memRamUse += it.second->ramMemoryCost;
            memGpuUse += it.second->gpuMemoryCost;
            // consider long time not used resources only
            if (it.second->impl->lastAccessTick + 100 < statistics.frameIndex
                    && it.second.use_count() == 1 && it.second->impl->state
                    != ResourceImpl::State::downloading)
                res.push_back(it.second.get());
        }
        uint32 memUse = memRamUse + memGpuUse;
        if (memUse > options.maxResourcesMemory)
        {
            std::sort(res.begin(), res.end(), [](Resource *a, Resource *b){
                if (a->impl->lastAccessTick == b->impl->lastAccessTick)
                    return a->gpuMemoryCost + a->ramMemoryCost
                            > b->gpuMemoryCost + b->ramMemoryCost;
                return a->impl->lastAccessTick < b->impl->lastAccessTick;
            });
            for (Resource *it : res)
            {
                if (memUse <= options.maxResourcesMemory)
                    break;
                memUse -= it->gpuMemoryCost + it->ramMemoryCost;
                if (it->impl->state != ResourceImpl::State::finalizing)
                    it->impl->state = ResourceImpl::State::finalizing;
                else
                {
                    statistics.resourcesReleased++;
                    resources.resources.erase(it->name);
                }
            }
        }
        statistics.currentGpuMemUse = memGpuUse;
        statistics.currentRamMemUse = memRamUse;
        statistics.currentResources = resources.resources.size();
    }
}

void MapImpl::touchResource(std::shared_ptr<Resource> resource)
{
    resource->impl->lastAccessTick = statistics.frameIndex;
    switch (resource->impl->state)
    {
    case ResourceImpl::State::finalizing:
        resource->impl->state = ResourceImpl::State::initializing;
        // no break here
    case ResourceImpl::State::initializing:
    case ResourceImpl::State::downloaded:
        resources.prepareQueNew.insert(resource->impl.get());
        break;
    }
}

std::shared_ptr<GpuTexture> MapImpl::getTexture(const std::string &name)
{
    auto it = resources.resources.find(name);
    if (it == resources.resources.end())
    {
        resources.resources[name] = mapFoundation->createTexture(name);
        it = resources.resources.find(name);
    }
    touchResource(it->second);
    return std::dynamic_pointer_cast<GpuTexture>(it->second);
}

std::shared_ptr<GpuMesh> MapImpl::getMeshRenderable(const std::string &name)
{
    auto it = resources.resources.find(name);
    if (it == resources.resources.end())
    {
        resources.resources[name] = mapFoundation->createMesh(name);
        it = resources.resources.find(name);
    }
    touchResource(it->second);
    return std::dynamic_pointer_cast<GpuMesh>(it->second);
}

std::shared_ptr<MapConfig> MapImpl::getMapConfig(const std::string &name)
{
    return getMapResource<MapConfig>(name, this);
}

std::shared_ptr<MetaTile> MapImpl::getMetaTile(const std::string &name)
{
    return getMapResource<MetaTile>(name, this);
}

std::shared_ptr<NavTile> MapImpl::getNavTile(
        const std::string &name)
{
    return getMapResource<NavTile>(name, this);
}

std::shared_ptr<MeshAggregate> MapImpl::getMeshAggregate(
        const std::string &name)
{
    return getMapResource<MeshAggregate>(name, this);
}

std::shared_ptr<ExternalBoundLayer> MapImpl::getExternalBoundLayer(
        const std::string &name)
{
    return getMapResource<ExternalBoundLayer>(name, this);
}

std::shared_ptr<BoundMetaTile> MapImpl::getBoundMetaTile(
        const std::string &name)
{
    return getMapResource<BoundMetaTile>(name, this);
}

std::shared_ptr<BoundMaskTile> MapImpl::getBoundMaskTile(
        const std::string &name)
{
    return getMapResource<BoundMaskTile>(name, this);
}

Validity MapImpl::getResourceValidity(const std::string &name)
{
    auto it = resources.resources.find(name);
    if (it == resources.resources.end())
        return Validity::Invalid;
    switch (it->second->impl->state)
    {
    case ResourceImpl::State::errorDownload:
    case ResourceImpl::State::errorLoad:
        return Validity::Invalid;
    case ResourceImpl::State::finalizing:
    case ResourceImpl::State::initializing:
    case ResourceImpl::State::downloading:
    case ResourceImpl::State::downloaded:
        return Validity::Indeterminate;
    case ResourceImpl::State::ready:
        return Validity::Valid;
    default:
        assert(false);
    }
}

const std::string MapImpl::convertNameToCache(const std::string &path)
{
    uint32 p = path.find("://");
    std::string a = p == std::string::npos ? path : path.substr(p + 3);
    std::string b = boost::filesystem::path(a).parent_path().string();
    std::string c = a.substr(b.length() + 1);
    return resources.cachePath
            + convertNameToPath(b, false) + "/"
            + convertNameToPath(c, false);
}

bool MapImpl::availableInCache(const std::string &name)
{
    std::string path = convertNameToCache(name);
    return boost::filesystem::exists(path);
}

const std::string MapImpl::convertNameToPath(std::string path,
                                           bool preserveSlashes)
{
    path = boost::filesystem::path(path).normalize().string();
    std::string res;
    res.reserve(path.size());
    for (char it : path)
    {
        if ((it >= 'a' && it <= 'z')
         || (it >= 'A' && it <= 'Z')
         || (it >= '0' && it <= '9')
         || (it == '-' || it == '.'))
            res += it;
        else if (preserveSlashes && (it == '/' || it == '\\'))
            res += '/';
        else
            res += '_';
    }
    return res;
}

const std::string MapImpl::Resources::invalidUrlFileName = "invalidUrl.txt";

} // namespace vts