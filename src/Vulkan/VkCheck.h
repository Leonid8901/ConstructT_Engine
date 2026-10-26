#pragma once
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <string>

#define VK_CHECK(expr)                                                                  \
    do {                                                                                \
        VkResult vkCheckResult_ = (expr);                                               \
        if (vkCheckResult_ != VK_SUCCESS)                                               \
            throw std::runtime_error(std::string("Vulkan error ") +                     \
                                     std::to_string(static_cast<int>(vkCheckResult_)) + \
                                     " in " #expr " at " __FILE__ ":" +                 \
                                     std::to_string(__LINE__));                         \
    } while (0)
