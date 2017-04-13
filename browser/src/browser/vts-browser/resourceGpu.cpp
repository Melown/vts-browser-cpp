#include <vts-libs/registry/referenceframe.hpp>
#include <vts/resources.hpp>
#include <vts/buffer.hpp>
#include <vts/math.hpp>

#include "resource.hpp"
#include "image.hpp"
#include "obj.hpp"

namespace vts
{

GpuTextureSpec::GpuTextureSpec() : width(0), height(0), components(0),
    verticalFlip(true)
{}

GpuTexture::GpuTexture(const std::string &name) : Resource(name)
{}

void GpuTexture::load(class MapImpl *)
{
    GpuTextureSpec spec;
    decodeImage(name, impl->contentData, spec.buffer,
                spec.width, spec.height, spec.components);
    loadTexture(spec);
}

GpuMeshSpec::GpuMeshSpec() : verticesCount(0), indicesCount(0),
    faceMode(FaceMode::Triangles)
{}

GpuMeshSpec::VertexAttribute::VertexAttribute() : offset(0), stride(0),
    components(0), type(Type::Float), enable(false), normalized(false)
{}

GpuMesh::GpuMesh(const std::string &name) : Resource(name)
{}

void GpuMesh::load(class MapImpl *)
{
    uint32 vc = 0, ic = 0;
    GpuMeshSpec spec;
    decodeObj(name, impl->contentData,
              spec.vertices, spec.indices, vc, ic);
    spec.verticesCount = vc;
    spec.attributes[0].enable = true;
    spec.attributes[0].stride = sizeof(vec3f) + sizeof(vec2f);
    spec.attributes[0].components = 3;
    spec.attributes[1].enable = true;
    spec.attributes[1].stride = spec.attributes[0].stride;
    spec.attributes[1].components = 2;
    spec.attributes[1].offset = sizeof(vec3f);
    spec.attributes[2] = spec.attributes[1];
    loadMesh(spec);
}

} // namespace vts