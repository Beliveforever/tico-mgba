#pragma once

#include "TicoVulkan.h"
#include "woplay/rendering/effect_backend.hpp"
#include <memory>

namespace woplay_menu {
class VulkanMenuEffects final : public gbastation::rendering::EffectBackend {
public:
    VulkanMenuEffects();
    ~VulkanMenuEffects() override;
    bool initialize();
    bool isReady() const noexcept override;
    void prepare(VkCommandBuffer cmd, ImDrawData& frame);

private:
    void drawGlass(const gbastation::rendering::GlassRequest& request) override;
    void beginEffectFrame() override;
    struct Resources;
    std::unique_ptr<Resources> gpu_;
};
}
