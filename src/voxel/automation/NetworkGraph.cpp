#include <voxel/automation/NetworkGraph.hpp>

namespace voxel::automation {

void NetworkGraph::addNode(NetworkNode node)
{
    nodes_.push_back(std::move(node));
    markDirty();
}

void NetworkGraph::addEdge(NetworkEdge edge)
{
    edges_.push_back(edge);
    markDirty();
}

void NetworkGraph::markDirty()
{
    dirty_ = true;
}

bool NetworkGraph::dirty() const noexcept
{
    return dirty_;
}

const std::vector<NetworkNode>& NetworkGraph::nodes() const noexcept
{
    return nodes_;
}

} // namespace voxel::automation

