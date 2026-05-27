#pragma once

#include <string>
#include <vector>

namespace voxel::render {

struct RenderPass {
    std::string name;
};

class RenderGraph {
public:
    void addPass(RenderPass pass);
    void clear();
    [[nodiscard]] const std::vector<RenderPass>& passes() const noexcept;

    // TODO(render): Track images, buffers, barriers, queue ownership, and async compute scheduling.

private:
    std::vector<RenderPass> passes_;
};

} // namespace voxel::render

