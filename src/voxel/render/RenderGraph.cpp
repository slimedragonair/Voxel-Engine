#include <voxel/render/RenderGraph.hpp>

#include <utility>

namespace voxel::render {

void RenderGraph::addPass(RenderPass pass)
{
    passes_.push_back(std::move(pass));
}

void RenderGraph::clear()
{
    passes_.clear();
}

const std::vector<RenderPass>& RenderGraph::passes() const noexcept
{
    return passes_;
}

} // namespace voxel::render

