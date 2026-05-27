#pragma once

#include <functional>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace voxel::core {

class EventBus {
public:
    template <typename Event>
    using Handler = std::function<void(const Event&)>;

    template <typename Event>
    void subscribe(Handler<Event> handler)
    {
        auto& bucket = handlers_[std::type_index(typeid(Event))];
        bucket.emplace_back([handler = std::move(handler)](const void* event) {
            handler(*static_cast<const Event*>(event));
        });
    }

    template <typename Event>
    void publish(const Event& event) const
    {
        const auto found = handlers_.find(std::type_index(typeid(Event)));
        if (found == handlers_.end()) {
            return;
        }

        for (const auto& handler : found->second) {
            handler(&event);
        }
    }

private:
    using ErasedHandler = std::function<void(const void*)>;
    std::unordered_map<std::type_index, std::vector<ErasedHandler>> handlers_;
};

} // namespace voxel::core

