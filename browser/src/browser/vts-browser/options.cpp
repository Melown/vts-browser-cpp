#include <dbglog/dbglog.hpp>
#include <vts/options.hpp>

namespace vts
{

void setLogMask(const std::string &mask)
{
    dbglog::set_mask(mask);
}

MapOptions::MapOptions() :
    maxTexelToPixelScale(1.2),
    positionViewExtentMin(75),
    positionViewExtentMax(1e7),
    cameraSensitivityPan(1),
    cameraSensitivityAltitude(1),
    cameraSensitivityZoom(1),
    cameraSensitivityRotate(1),
    cameraInertiaPan(0.8),
    cameraInertiaAltitude(0.95),
    cameraInertiaZoom(0.8),
    cameraInertiaRotate(0.8),
    maxResourcesMemory(512 * 1024 * 1024),
    maxConcurrentDownloads(10),
    maxNodeUpdatesPerTick(10),
    maxResourceProcessesPerTick(5),
    navigationSamplesPerViewExtent(4),
    renderSurrogates(false),
    renderMeshBoxes(false),
    renderTileBoxes(false),
    renderObjectPosition(false),
    debugDetachedCamera(false),
    debugDisableMeta5(false)
{}

MapOptions::~MapOptions()
{}

} // namespace vts
