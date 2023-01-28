/* Copyright (c) 2015-2023 The Khronos Group Inc.
 * Copyright (c) 2015-2023 Valve Corporation
 * Copyright (c) 2015-2023 LunarG, Inc.
 * Copyright (C) 2015-2023 Google Inc.
 * Modifications Copyright (C) 2020-2022 Advanced Micro Devices, Inc. All rights reserved.
 * Modifications Copyright (C) 2022 RasterGrid Kft.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Mark Lobodzinski <mark@lunarg.com>
 * Author: Dave Houlton <daveh@lunarg.com>
 * Shannon McPherson <shannon@lunarg.com>
 * Author: Tobias Hector <tobias.hector@amd.com>
 * Author: Daniel Rakos <daniel.rakos@rastergrid.com>
 */

#include <cmath>
#include <set>
#include <sstream>
#include <string>
#include <iostream>

#include "vk_enum_string_helper.h"
#include "vk_format_utils.h"
#include "vk_layer_data.h"
#include "vk_layer_utils.h"
#include "vk_layer_logging.h"
#include "vk_typemap_helper.h"

#include "chassis.h"
#include "core_checks/core_validation.h"
#include "core_error_location.h"
#include "core_checks/shader_validation.h"
#include "state_tracker/descriptor_sets.h"
#include "sync/sync_utils.h"
#include "sync/sync_vuid_maps.h"

using LayoutRange = image_layout_map::ImageSubresourceLayoutMap::RangeType;
using LayoutEntry = image_layout_map::ImageSubresourceLayoutMap::LayoutEntry;

static VkImageLayout NormalizeDepthImageLayout(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;

        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

        default:
            return layout;
    }
}

static VkImageLayout NormalizeStencilImageLayout(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
            return VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;

        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;

        default:
            return layout;
    }
}

static VkImageLayout NormalizeSynchronization2Layout(const VkImageAspectFlags aspect_mask, VkImageLayout layout) {
    if (layout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR) {
        if (aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT) {
            layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
            layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        } else if (aspect_mask == VK_IMAGE_ASPECT_DEPTH_BIT) {
            layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        } else if (aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT) {
            layout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
        }
    } else if (layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR) {
        if (aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT) {
            layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
            layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        } else if (aspect_mask == VK_IMAGE_ASPECT_DEPTH_BIT) {
            layout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        } else if (aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT) {
            layout = VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
        }
    }
    return layout;
}

static bool ImageLayoutMatches(const VkImageAspectFlags aspect_mask, VkImageLayout a, VkImageLayout b) {
    bool matches = (a == b);
    if (!matches) {
        a = NormalizeSynchronization2Layout(aspect_mask, a);
        b = NormalizeSynchronization2Layout(aspect_mask, b);
        matches = (a == b);
        if (!matches) {
            // Relaxed rules when referencing *only* the depth or stencil aspects.
            // When accessing both, normalize layouts for aspects separately.
            if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
                matches = NormalizeDepthImageLayout(a) == NormalizeDepthImageLayout(b) &&
                          NormalizeStencilImageLayout(a) == NormalizeStencilImageLayout(b);
            } else if (aspect_mask == VK_IMAGE_ASPECT_DEPTH_BIT) {
                matches = NormalizeDepthImageLayout(a) == NormalizeDepthImageLayout(b);
            } else if (aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT) {
                matches = NormalizeStencilImageLayout(a) == NormalizeStencilImageLayout(b);
            }
        }
    }
    return matches;
}

// Utility type for checking Image layouts
struct LayoutUseCheckAndMessage {
    const static VkImageAspectFlags kDepthOrStencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    const VkImageLayout expected_layout;
    const VkImageAspectFlags aspect_mask;
    const char *message;
    VkImageLayout layout;

    LayoutUseCheckAndMessage() = delete;
    LayoutUseCheckAndMessage(VkImageLayout expected, const VkImageAspectFlags aspect_mask_ = 0)
        : expected_layout{expected}, aspect_mask{aspect_mask_}, message(nullptr), layout(kInvalidLayout) {}
    bool Check(const LayoutEntry &layout_entry) {
        message = nullptr;
        layout = kInvalidLayout;  // Success status
        if (layout_entry.current_layout != kInvalidLayout) {
            if (!ImageLayoutMatches(aspect_mask, expected_layout, layout_entry.current_layout)) {
                message = "previous known";
                layout = layout_entry.current_layout;
            }
        } else if (layout_entry.initial_layout != kInvalidLayout) {
            if (!ImageLayoutMatches(aspect_mask, expected_layout, layout_entry.initial_layout)) {
                assert(layout_entry.state);  // If we have an initial layout, we better have a state for it
                if (!((layout_entry.state->aspect_mask & kDepthOrStencil) &&
                      ImageLayoutMatches(layout_entry.state->aspect_mask, expected_layout, layout_entry.initial_layout))) {
                    message = "previously used";
                    layout = layout_entry.initial_layout;
                }
            }
        }
        return layout == kInvalidLayout;
    }
};

bool CoreChecks::FindLayouts(const IMAGE_STATE &image_state, std::vector<VkImageLayout> &layouts) const {
    const auto *layout_range_map = image_state.layout_range_map.get();
    if (!layout_range_map) return false;

    auto guard = layout_range_map->ReadLock();
    // TODO: FindLayouts function should mutate into a ValidatePresentableLayout with the loop wrapping the LogError
    //       from the caller. You can then use decode to add the subresource of the range::begin to the error message.

    // TODO: what is this test and what is it supposed to do?! -- the logic doesn't match the comment below?!

    // TODO: Make this robust for >1 aspect mask. Now it will just say ignore potential errors in this case.
    if (layout_range_map->size() >= (image_state.createInfo.arrayLayers * image_state.createInfo.mipLevels + 1)) {
        return false;
    }

    for (const auto &entry : *layout_range_map) {
        layouts.push_back(entry.second);
    }
    return true;
}

bool CoreChecks::ValidateMultipassRenderedToSingleSampledSampleCount(RenderPassCreateVersion rp_version, VkFramebuffer framebuffer,
                                                                     VkRenderPass renderpass, uint32_t subpass,
                                                                     IMAGE_STATE *image_state, VkSampleCountFlagBits msrtss_samples,
                                                                     uint32_t attachment_index, bool depth) const {
    bool skip = false;
    const char *function_name = (rp_version == RENDER_PASS_VERSION_2) ? "vkCmdBeginRenderPass2()" : "vkCmdBeginRenderPass()";
    const auto image_create_info = image_state->createInfo;
    if (!image_state->image_format_properties.sampleCounts) {
        skip |= GetPhysicalDeviceImageFormatProperties(*image_state, "VUID-VkRenderPassAttachmentBeginInfo-pAttachments-07010");
    }
    if (!(image_state->image_format_properties.sampleCounts & msrtss_samples)) {
        std::stringstream msg;
        if (depth) {
            msg << "depth stencil attachment";
        } else {
            msg << "attachment " << attachment_index;
        }
        skip |= LogError(device, "VUID-VkRenderPassAttachmentBeginInfo-pAttachments-07010",
                         "%s(): Renderpass subpass %" PRIu32
                         " enables "
                         "multisampled-render-to-single-sampled and %s"
                         ", is specified with "
                         "VK_SAMPLE_COUNT_1_BIT samples, but image (%s) created with format %s imageType: %s, "
                         "tiling: %s, usage: %s, "
                         "flags: %s does not support a rasterizationSamples count of %s",
                         function_name, subpass, msg.str().c_str(), report_data->FormatHandle(image_state->Handle()).c_str(),
                         string_VkFormat(image_create_info.format), string_VkImageType(image_create_info.imageType),
                         string_VkImageTiling(image_create_info.tiling), string_VkImageUsageFlags(image_create_info.usage).c_str(),
                         string_VkImageCreateFlags(image_create_info.flags).c_str(), string_VkSampleCountFlagBits(msrtss_samples));
    }
    return skip;
}

bool CoreChecks::ValidateRenderPassLayoutAgainstFramebufferImageUsage(RenderPassCreateVersion rp_version, VkImageLayout layout,
                                                                      const IMAGE_VIEW_STATE &image_view_state,
                                                                      VkFramebuffer framebuffer, VkRenderPass renderpass,
                                                                      uint32_t attachment_index, const char *variable_name) const {
    bool skip = false;
    const auto &image_view = image_view_state.Handle();
    const auto *image_state = image_view_state.image_state.get();
    const auto &image = image_state->Handle();
    const char *vuid;
    const bool use_rp2 = (rp_version == RENDER_PASS_VERSION_2);
    const char *function_name = use_rp2 ? "vkCmdBeginRenderPass2()" : "vkCmdBeginRenderPass()";

    if (!image_state) {
        const LogObjectList objlist(image, renderpass, framebuffer, image_view);
        skip |=
            LogError(objlist, "VUID-VkRenderPassBeginInfo-framebuffer-parameter",
                     "%s: RenderPass %s uses %s where pAttachments[%" PRIu32 "] = %s, which refers to an invalid image",
                     function_name, report_data->FormatHandle(renderpass).c_str(), report_data->FormatHandle(framebuffer).c_str(),
                     attachment_index, report_data->FormatHandle(image_view).c_str());
        return skip;
    }

    auto image_usage = image_state->createInfo.usage;
    const auto stencil_usage_info = LvlFindInChain<VkImageStencilUsageCreateInfo>(image_state->createInfo.pNext);
    if (stencil_usage_info) {
        image_usage |= stencil_usage_info->stencilUsage;
    }

    // Check for layouts that mismatch image usages in the framebuffer
    if (layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && !(image_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        vuid = use_rp2 ? "VUID-vkCmdBeginRenderPass2-initialLayout-03094" : "VUID-vkCmdBeginRenderPass-initialLayout-00895";
        const LogObjectList objlist(image, renderpass, framebuffer, image_view);
        skip |= LogError(objlist, vuid,
                         "%s: Layout/usage mismatch for attachment %" PRIu32
                         " in %s"
                         " - the %s is %s but the image attached to %s via %s"
                         " was not created with VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT",
                         function_name, attachment_index, report_data->FormatHandle(renderpass).c_str(), variable_name,
                         string_VkImageLayout(layout), report_data->FormatHandle(framebuffer).c_str(),
                         report_data->FormatHandle(image_view).c_str());
    }

    if (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        !(image_usage & (VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))) {
        vuid = use_rp2 ? "VUID-vkCmdBeginRenderPass2-initialLayout-03097" : "VUID-vkCmdBeginRenderPass-initialLayout-00897";
        const LogObjectList objlist(image, renderpass, framebuffer, image_view);
        skip |= LogError(objlist, vuid,
                         "%s: Layout/usage mismatch for attachment %" PRIu32
                         " in %s"
                         " - the %s is %s but the image attached to %s via %s"
                         " was not created with VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT or VK_IMAGE_USAGE_SAMPLED_BIT",
                         function_name, attachment_index, report_data->FormatHandle(renderpass).c_str(), variable_name,
                         string_VkImageLayout(layout), report_data->FormatHandle(framebuffer).c_str(),
                         report_data->FormatHandle(image_view).c_str());
    }

    if (layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && !(image_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        vuid = use_rp2 ? "VUID-vkCmdBeginRenderPass2-initialLayout-03098" : "VUID-vkCmdBeginRenderPass-initialLayout-00898";
        const LogObjectList objlist(image, renderpass, framebuffer, image_view);
        skip |= LogError(objlist, vuid,
                         "%s: Layout/usage mismatch for attachment %" PRIu32
                         " in %s"
                         " - the %s is %s but the image attached to %s via %s"
                         " was not created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT",
                         function_name, attachment_index, report_data->FormatHandle(renderpass).c_str(), variable_name,
                         string_VkImageLayout(layout), report_data->FormatHandle(framebuffer).c_str(),
                         report_data->FormatHandle(image_view).c_str());
    }

    if (layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && !(image_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        vuid = use_rp2 ? "VUID-vkCmdBeginRenderPass2-initialLayout-03099" : "VUID-vkCmdBeginRenderPass-initialLayout-00899";
        const LogObjectList objlist(image, renderpass, framebuffer, image_view);
        skip |= LogError(objlist, vuid,
                         "%s: Layout/usage mismatch for attachment %" PRIu32
                         " in %s"
                         " - the %s is %s but the image attached to %s via %s"
                         " was not created with VK_IMAGE_USAGE_TRANSFER_DST_BIT",
                         function_name, attachment_index, report_data->FormatHandle(renderpass).c_str(), variable_name,
                         string_VkImageLayout(layout), report_data->FormatHandle(framebuffer).c_str(),
                         report_data->FormatHandle(image_view).c_str());
    }

    if (layout == VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT) {
        if (((image_usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) == 0) ||
            ((image_usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) == 0)) {
            vuid = use_rp2 ? "VUID-vkCmdBeginRenderPass2-initialLayout-07002" : "VUID-vkCmdBeginRenderPass-initialLayout-07000";
            const LogObjectList objlist(image, renderpass, framebuffer, image_view);
            skip |=
                LogError(objlist, vuid,
                         "%s: Layout/usage mismatch for attachment %" PRIu32
                         " in %s"
                         " - the %s is %s but the image attached to %s via %s"
                         " was not created with either the VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT or "
                         "VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT usage bits, and the VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT or "
                         "VK_IMAGE_USAGE_SAMPLED_BIT usage bits",
                         function_name, attachment_index, report_data->FormatHandle(renderpass).c_str(), variable_name,
                         string_VkImageLayout(layout), report_data->FormatHandle(framebuffer).c_str(),
                         report_data->FormatHandle(image_view).c_str());
        }
        if (!(image_usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)) {
            vuid = use_rp2 ? "VUID-vkCmdBeginRenderPass2-initialLayout-07003" : "VUID-vkCmdBeginRenderPass-initialLayout-07001";
            const LogObjectList objlist(image, renderpass, framebuffer, image_view);
            skip |= LogError(objlist, vuid,
                             "%s: Layout/usage mismatch for attachment %" PRIu32
                             " in %s"
                             " - the %s is %s but the image attached to %s via %s"
                             " was not created with the VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT usage bit",
                             function_name, attachment_index, report_data->FormatHandle(renderpass).c_str(), variable_name,
                             string_VkImageLayout(layout), report_data->FormatHandle(framebuffer).c_str(),
                             report_data->FormatHandle(image_view).c_str());
        }
    }

    if (IsExtEnabled(device_extensions.vk_khr_maintenance2)) {
        if ((layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL ||
             layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL ||
             layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
             layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) &&
            !(image_usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            vuid = use_rp2 ? "VUID-vkCmdBeginRenderPass2-initialLayout-03096" : "VUID-vkCmdBeginRenderPass-initialLayout-01758";
            const LogObjectList objlist(image, renderpass, framebuffer, image_view);
            skip |= LogError(objlist, vuid,
                             "%s: Layout/usage mismatch for attachment %" PRIu32
                             " in %s"
                             " - the %s is %s but the image attached to %s via %s"
                             " was not created with VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT",
                             function_name, attachment_index, report_data->FormatHandle(renderpass).c_str(), variable_name,
                             string_VkImageLayout(layout), report_data->FormatHandle(framebuffer).c_str(),
                             report_data->FormatHandle(image_view).c_str());
        }
    } else {
        // The create render pass 2 extension requires maintenance 2 (the previous branch), so no vuid switch needed here.
        if ((layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
             layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) &&
            !(image_usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            const LogObjectList objlist(image, renderpass, framebuffer, image_view);
            skip |= LogError(objlist, "VUID-vkCmdBeginRenderPass-initialLayout-00896",
                             "%s: Layout/usage mismatch for attachment %" PRIu32
                             " in %s"
                             " - the %s is %s but the image attached to %s via %s"
                             " was not created with VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT",
                             function_name, attachment_index, report_data->FormatHandle(renderpass).c_str(), variable_name,
                             string_VkImageLayout(layout), report_data->FormatHandle(framebuffer).c_str(),
                             report_data->FormatHandle(image_view).c_str());
        }
    }
    return skip;
}

bool CoreChecks::VerifyFramebufferAndRenderPassLayouts(RenderPassCreateVersion rp_version, const CMD_BUFFER_STATE &cb_state,
                                                       const VkRenderPassBeginInfo *pRenderPassBegin,
                                                       const FRAMEBUFFER_STATE *framebuffer_state) const {
    bool skip = false;
    auto render_pass_state = Get<RENDER_PASS_STATE>(pRenderPassBegin->renderPass);
    const auto *render_pass_info = render_pass_state->createInfo.ptr();
    auto render_pass = render_pass_state->renderPass();
    auto const &framebuffer_info = framebuffer_state->createInfo;
    const VkImageView *attachments = framebuffer_info.pAttachments;

    auto framebuffer = framebuffer_state->framebuffer();

    if (render_pass_info->attachmentCount != framebuffer_info.attachmentCount) {
        skip |= LogError(cb_state.commandBuffer(), kVUID_Core_DrawState_InvalidRenderpass,
                         "You cannot start a render pass using a framebuffer with a different number of attachments.");
    }

    const auto *attachment_info = LvlFindInChain<VkRenderPassAttachmentBeginInfo>(pRenderPassBegin->pNext);
    if (((framebuffer_info.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0) && attachment_info != nullptr) {
        attachments = attachment_info->pAttachments;
    }

    if (attachments == nullptr) {
        return skip;
    }
    for (uint32_t i = 0; i < render_pass_info->attachmentCount && i < framebuffer_info.attachmentCount; ++i) {
        auto image_view = attachments[i];
        auto view_state = Get<IMAGE_VIEW_STATE>(image_view);

        if (!view_state) {
            const LogObjectList objlist(pRenderPassBegin->renderPass, framebuffer_state->framebuffer(), image_view);
            skip |= LogError(objlist, "VUID-VkRenderPassBeginInfo-framebuffer-parameter",
                             "vkCmdBeginRenderPass(): %s pAttachments[%" PRIu32 "] = %s is not a valid VkImageView handle",
                             report_data->FormatHandle(framebuffer_state->framebuffer()).c_str(), i,
                             report_data->FormatHandle(image_view).c_str());
            continue;
        }

        const VkImage image = view_state->create_info.image;
        const auto *image_state = view_state->image_state.get();

        if (!image_state) {
            const LogObjectList objlist(pRenderPassBegin->renderPass, framebuffer_state->framebuffer(), image_view, image);
            skip |= LogError(objlist, "VUID-VkRenderPassBeginInfo-framebuffer-parameter",
                             "vkCmdBeginRenderPass(): %s pAttachments[%" PRIu32 "] =  %s references non-extant %s.",
                             report_data->FormatHandle(framebuffer_state->framebuffer()).c_str(), i,
                             report_data->FormatHandle(image_view).c_str(), report_data->FormatHandle(image).c_str());
            continue;
        }
        auto attachment_initial_layout = render_pass_info->pAttachments[i].initialLayout;
        auto final_layout = render_pass_info->pAttachments[i].finalLayout;

        // Default to expecting stencil in the same layout.
        auto attachment_stencil_initial_layout = attachment_initial_layout;

        // If a separate layout is specified, look for that.
        const auto *attachment_description_stencil_layout =
            LvlFindInChain<VkAttachmentDescriptionStencilLayout>(render_pass_info->pAttachments[i].pNext);
        if (attachment_description_stencil_layout) {
            attachment_stencil_initial_layout = attachment_description_stencil_layout->stencilInitialLayout;
        }

        const ImageSubresourceLayoutMap *subresource_map = nullptr;
        bool has_queried_map = false;

        for (uint32_t aspect_index = 0; aspect_index < 32; aspect_index++) {
            VkImageAspectFlags test_aspect = 1u << aspect_index;
            if ((view_state->normalized_subresource_range.aspectMask & test_aspect) == 0) {
                continue;
            }

            // Allow for differing depth and stencil layouts
            VkImageLayout check_layout = attachment_initial_layout;
            if (test_aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
                check_layout = attachment_stencil_initial_layout;
            }

            // If no layout information for image yet, will be checked at QueueSubmit time
            if (check_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                continue;
            }
            if (!has_queried_map) {
                subresource_map = cb_state.GetImageSubresourceLayoutMap(*image_state);
                has_queried_map = true;
            }
            if (!subresource_map) {
                // If no layout information for image yet, will be checked at QueueSubmit time
                continue;
            }
            auto normalized_range = view_state->normalized_subresource_range;
            normalized_range.aspectMask = test_aspect;
            LayoutUseCheckAndMessage layout_check(check_layout, test_aspect);

            skip |= subresource_map->AnyInRange(
                normalized_range, [this, &layout_check, i](const LayoutRange &range, const LayoutEntry &state) {
                    bool subres_skip = false;
                    if (!layout_check.Check(state)) {
                        subres_skip = LogError(device, kVUID_Core_DrawState_InvalidRenderpass,
                                               "You cannot start a render pass using attachment %" PRIu32
                                               " where the render pass initial "
                                               "layout is %s "
                                               "and the %s layout of the attachment is %s. The layouts must match, or the render "
                                               "pass initial layout for the attachment must be VK_IMAGE_LAYOUT_UNDEFINED",
                                               i, string_VkImageLayout(layout_check.expected_layout), layout_check.message,
                                               string_VkImageLayout(layout_check.layout));
                    }
                    return subres_skip;
                });
        }
        skip |= ValidateRenderPassLayoutAgainstFramebufferImageUsage(rp_version, attachment_initial_layout, *view_state,
                                                                     framebuffer, render_pass, i, "initial layout");

        skip |= ValidateRenderPassLayoutAgainstFramebufferImageUsage(rp_version, final_layout, *view_state, framebuffer,
                                                                     render_pass, i, "final layout");
    }

    for (uint32_t j = 0; j < render_pass_info->subpassCount; ++j) {
        auto &subpass = render_pass_info->pSubpasses[j];
        const auto *ms_rendered_to_single_sampled =
            LvlFindInChain<VkMultisampledRenderToSingleSampledInfoEXT>(render_pass_info->pSubpasses[j].pNext);
        for (uint32_t k = 0; k < render_pass_info->pSubpasses[j].inputAttachmentCount; ++k) {
            auto &attachment_ref = subpass.pInputAttachments[k];
            if (attachment_ref.attachment != VK_ATTACHMENT_UNUSED) {
                auto image_view = attachments[attachment_ref.attachment];
                auto view_state = Get<IMAGE_VIEW_STATE>(image_view);

                if (view_state) {
                    skip |= ValidateRenderPassLayoutAgainstFramebufferImageUsage(
                        rp_version, attachment_ref.layout, *view_state, framebuffer, render_pass, attachment_ref.attachment,
                        "input attachment layout");
                }
                if (ms_rendered_to_single_sampled && ms_rendered_to_single_sampled->multisampledRenderToSingleSampledEnable) {
                    if (render_pass_info->pAttachments[attachment_ref.attachment].samples == VK_SAMPLE_COUNT_1_BIT) {
                        skip |= ValidateMultipassRenderedToSingleSampledSampleCount(
                            rp_version, framebuffer, render_pass, k, view_state->image_state.get(),
                            ms_rendered_to_single_sampled->rasterizationSamples, attachment_ref.attachment);
                    }
                }
            }
        }

        for (uint32_t k = 0; k < render_pass_info->pSubpasses[j].colorAttachmentCount; ++k) {
            auto &attachment_ref = subpass.pColorAttachments[k];
            if (attachment_ref.attachment != VK_ATTACHMENT_UNUSED) {
                auto image_view = attachments[attachment_ref.attachment];
                auto view_state = Get<IMAGE_VIEW_STATE>(image_view);

                if (view_state) {
                    skip |= ValidateRenderPassLayoutAgainstFramebufferImageUsage(
                        rp_version, attachment_ref.layout, *view_state, framebuffer, render_pass, attachment_ref.attachment,
                        "color attachment layout");
                    if (subpass.pResolveAttachments) {
                        skip |= ValidateRenderPassLayoutAgainstFramebufferImageUsage(
                            rp_version, attachment_ref.layout, *view_state, framebuffer, render_pass, attachment_ref.attachment,
                            "resolve attachment layout");
                    }
                }
                if (ms_rendered_to_single_sampled && ms_rendered_to_single_sampled->multisampledRenderToSingleSampledEnable) {
                    if (render_pass_info->pAttachments[attachment_ref.attachment].samples == VK_SAMPLE_COUNT_1_BIT) {
                        skip |= ValidateMultipassRenderedToSingleSampledSampleCount(
                            rp_version, framebuffer, render_pass, k, view_state->image_state.get(),
                            ms_rendered_to_single_sampled->rasterizationSamples, attachment_ref.attachment);
                    }
                }
            }
        }

        if (render_pass_info->pSubpasses[j].pDepthStencilAttachment) {
            auto &attachment_ref = *subpass.pDepthStencilAttachment;
            if (attachment_ref.attachment != VK_ATTACHMENT_UNUSED) {
                auto image_view = attachments[attachment_ref.attachment];
                auto view_state = Get<IMAGE_VIEW_STATE>(image_view);

                if (view_state) {
                    skip |= ValidateRenderPassLayoutAgainstFramebufferImageUsage(
                        rp_version, attachment_ref.layout, *view_state, framebuffer, render_pass, attachment_ref.attachment,
                        "input attachment layout");
                }
                if (ms_rendered_to_single_sampled && ms_rendered_to_single_sampled->multisampledRenderToSingleSampledEnable) {
                    if (render_pass_info->pAttachments[attachment_ref.attachment].samples == VK_SAMPLE_COUNT_1_BIT) {
                        skip |= ValidateMultipassRenderedToSingleSampledSampleCount(
                            rp_version, framebuffer, render_pass, 0, view_state->image_state.get(),
                            ms_rendered_to_single_sampled->rasterizationSamples, attachment_ref.attachment, true);
                    }
                }
            }
        }
    }
    return skip;
}

void CoreChecks::TransitionAttachmentRefLayout(CMD_BUFFER_STATE *cb_state, FRAMEBUFFER_STATE *pFramebuffer,
                                               const safe_VkAttachmentReference2 &ref) {
    if (ref.attachment != VK_ATTACHMENT_UNUSED) {
        IMAGE_VIEW_STATE *image_view = cb_state->GetActiveAttachmentImageViewState(ref.attachment);
        if (image_view) {
            VkImageLayout stencil_layout = kInvalidLayout;
            const auto *attachment_reference_stencil_layout = LvlFindInChain<VkAttachmentReferenceStencilLayout>(ref.pNext);
            if (attachment_reference_stencil_layout) {
                stencil_layout = attachment_reference_stencil_layout->stencilLayout;
            }

            cb_state->SetImageViewLayout(*image_view, ref.layout, stencil_layout);
        }
    }
}

void CoreChecks::TransitionSubpassLayouts(CMD_BUFFER_STATE *cb_state, const RENDER_PASS_STATE *render_pass_state,
                                          const int subpass_index, FRAMEBUFFER_STATE *framebuffer_state) {
    assert(render_pass_state);

    if (framebuffer_state) {
        auto const &subpass = render_pass_state->createInfo.pSubpasses[subpass_index];
        for (uint32_t j = 0; j < subpass.inputAttachmentCount; ++j) {
            TransitionAttachmentRefLayout(cb_state, framebuffer_state, subpass.pInputAttachments[j]);
        }
        for (uint32_t j = 0; j < subpass.colorAttachmentCount; ++j) {
            TransitionAttachmentRefLayout(cb_state, framebuffer_state, subpass.pColorAttachments[j]);
        }
        if (subpass.pDepthStencilAttachment) {
            TransitionAttachmentRefLayout(cb_state, framebuffer_state, *subpass.pDepthStencilAttachment);
        }
    }
}

// Transition the layout state for renderpass attachments based on the BeginRenderPass() call. This includes:
// 1. Transition into initialLayout state
// 2. Transition from initialLayout to layout used in subpass 0
void CoreChecks::TransitionBeginRenderPassLayouts(CMD_BUFFER_STATE *cb_state, const RENDER_PASS_STATE *render_pass_state,
                                                  FRAMEBUFFER_STATE *framebuffer_state) {
    // First record expected initialLayout as a potential initial layout usage.
    auto const rpci = render_pass_state->createInfo.ptr();
    for (uint32_t i = 0; i < rpci->attachmentCount; ++i) {
        auto *view_state = cb_state->GetActiveAttachmentImageViewState(i);
        if (view_state) {
            IMAGE_STATE *image_state = view_state->image_state.get();
            const auto initial_layout = rpci->pAttachments[i].initialLayout;
            const auto *attachment_description_stencil_layout =
                LvlFindInChain<VkAttachmentDescriptionStencilLayout>(rpci->pAttachments[i].pNext);
            if (attachment_description_stencil_layout) {
                const auto stencil_initial_layout = attachment_description_stencil_layout->stencilInitialLayout;
                VkImageSubresourceRange sub_range = view_state->normalized_subresource_range;
                sub_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                cb_state->SetImageInitialLayout(*image_state, sub_range, initial_layout);
                sub_range.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                cb_state->SetImageInitialLayout(*image_state, sub_range, stencil_initial_layout);
            } else {
                cb_state->SetImageInitialLayout(*image_state, view_state->normalized_subresource_range, initial_layout);
            }
        }
    }
    // Now transition for first subpass (index 0)
    TransitionSubpassLayouts(cb_state, render_pass_state, 0, framebuffer_state);
}

// There is a table in the Vulkan spec to list all formats that implicitly require YCbCr conversion,
// but some features/extensions can explicitly turn that restriction off
// The implicit check is done in format utils, while feature checks are done here in CoreChecks
bool CoreChecks::FormatRequiresYcbcrConversionExplicitly(const VkFormat format) const {
    if (format == VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16 &&
        enabled_features.rgba10x6_formats_features.formatRgba10x6WithoutYCbCrSampler) {
        return false;
    }
    return FormatRequiresYcbcrConversion(format);
}

// Verify an ImageMemoryBarrier's old/new ImageLayouts are compatible with the Image's ImageUsageFlags.
bool CoreChecks::ValidateBarrierLayoutToImageUsage(const Location &loc, VkImage image, VkImageLayout layout,
                                                   VkImageUsageFlags usage_flags) const {
    bool skip = false;
    bool is_error = false;
    switch (layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            is_error = ((usage_flags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0);
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            is_error = ((usage_flags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0);
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            is_error = ((usage_flags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0);
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            is_error = ((usage_flags & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) == 0);
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            is_error = ((usage_flags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0);
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            is_error = ((usage_flags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0);
            break;
        // alias VK_IMAGE_LAYOUT_SHADING_RATE_OPTIMAL_NV
        case VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR:
            // alias VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
            is_error = ((usage_flags & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) == 0);
            break;
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
            is_error = ((usage_flags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0);
            break;
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
            is_error = ((usage_flags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0);
            break;
        case VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT:
            is_error = ((usage_flags & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) == 0);
            is_error |= ((usage_flags & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) == 0);
            is_error |= ((usage_flags & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) == 0);
            break;
        case VK_IMAGE_LAYOUT_VIDEO_DECODE_SRC_KHR:
            is_error = ((usage_flags & VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR) == 0);
            break;
        case VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR:
            is_error = ((usage_flags & VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR) == 0);
            break;
        case VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR:
            is_error = ((usage_flags & VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR) == 0);
            break;
        case VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR:
            is_error = ((usage_flags & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR) == 0);
            break;
        case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DST_KHR:
            is_error = ((usage_flags & VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR) == 0);
            break;
        case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR:
            is_error = ((usage_flags & VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR) == 0);
            break;
        default:
            // Other VkImageLayout values do not have VUs defined in this context.
            break;
    }

    if (is_error) {
        const auto &vuid = sync_vuid_maps::GetBadImageLayoutVUID(loc, layout);

        skip |=
            LogError(image, vuid, "%s Image barrier Layout=%s is not compatible with %s usage flags 0x%" PRIx32 ".",
                     loc.Message().c_str(), string_VkImageLayout(layout), report_data->FormatHandle(image).c_str(), usage_flags);
    }
    return skip;
}

// Verify image barriers are compatible with the images they reference.
template <typename ImageBarrier>
bool CoreChecks::ValidateBarriersToImages(const Location &outer_loc, const CMD_BUFFER_STATE *cb_state,
                                          uint32_t imageMemoryBarrierCount, const ImageBarrier *pImageMemoryBarriers) const {
    bool skip = false;
    using sync_vuid_maps::GetImageBarrierVUID;
    using sync_vuid_maps::ImageError;

    // Scoreboard for duplicate layout transition barriers within the list
    // Pointers retained in the scoreboard only have the lifetime of *this* call (i.e. within the scope of the API call)
    const CommandBufferImageLayoutMap &current_map = cb_state->GetImageSubresourceLayoutMap();
    CommandBufferImageLayoutMap layout_updates;

    for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
        auto loc = outer_loc.dot(Field::pImageMemoryBarriers, i);
        const auto &img_barrier = pImageMemoryBarriers[i];

        auto image_state = Get<IMAGE_STATE>(img_barrier.image);
        if (!image_state) {
            continue;
        }

        if ((img_barrier.srcQueueFamilyIndex != img_barrier.dstQueueFamilyIndex) ||
            (img_barrier.oldLayout != img_barrier.newLayout)) {
            VkImageUsageFlags usage_flags = image_state->createInfo.usage;
            skip |=
                ValidateBarrierLayoutToImageUsage(loc.dot(Field::oldLayout), img_barrier.image, img_barrier.oldLayout, usage_flags);
            skip |=
                ValidateBarrierLayoutToImageUsage(loc.dot(Field::newLayout), img_barrier.image, img_barrier.newLayout, usage_flags);
        }

        // Make sure layout is able to be transitioned, currently only presented shared presentable images are locked
        if (image_state->layout_locked) {
            // TODO: Add unique id for error when available
            skip |= LogError(
                img_barrier.image, "VUID-Undefined",
                "%s Attempting to transition shared presentable %s"
                " from layout %s to layout %s, but image has already been presented and cannot have its layout transitioned.",
                loc.Message().c_str(), report_data->FormatHandle(img_barrier.image).c_str(),
                string_VkImageLayout(img_barrier.oldLayout), string_VkImageLayout(img_barrier.newLayout));
        }

        const VkImageCreateInfo &image_create_info = image_state->createInfo;
        const VkFormat image_format = image_create_info.format;
        const VkImageAspectFlags aspect_mask = img_barrier.subresourceRange.aspectMask;
        // For a Depth/Stencil image both aspects MUST be set
        auto image_loc = loc.dot(Field::image);
        if (FormatIsDepthAndStencil(image_format)) {
            if (enabled_features.core12.separateDepthStencilLayouts) {
                if (!(aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
                    auto vuid = GetImageBarrierVUID(loc, ImageError::kNotDepthOrStencilAspect);
                    skip |= LogError(img_barrier.image, vuid,
                                     "%s references %s of format %s that must have either the depth or stencil "
                                     "aspects set, but its aspectMask is 0x%" PRIx32 ".",
                                     image_loc.Message().c_str(), report_data->FormatHandle(img_barrier.image).c_str(),
                                     string_VkFormat(image_format), aspect_mask);
                }
            } else {
                auto const ds_mask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                if ((aspect_mask & ds_mask) != (ds_mask)) {
                    auto error = IsExtEnabled(device_extensions.vk_khr_separate_depth_stencil_layouts)
                                     ? ImageError::kNotSeparateDepthAndStencilAspect
                                     : ImageError::kNotDepthAndStencilAspect;
                    auto vuid = GetImageBarrierVUID(image_loc, error);
                    skip |= LogError(img_barrier.image, vuid,
                                     "%s references %s of format %s that must have the depth and stencil "
                                     "aspects set, but its aspectMask is 0x%" PRIx32 ".",
                                     image_loc.Message().c_str(), report_data->FormatHandle(img_barrier.image).c_str(),
                                     string_VkFormat(image_format), aspect_mask);
                }
            }
        }

        if (img_barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            // TODO: Set memory invalid which is in mem_tracker currently
        } else if (!QueueFamilyIsExternal(img_barrier.srcQueueFamilyIndex)) {
            auto &write_subresource_map = layout_updates[image_state.get()];
            bool new_write = false;
            if (!write_subresource_map) {
                write_subresource_map = std::make_shared<ImageSubresourceLayoutMap>(*image_state);
                new_write = true;
            }
            const auto &current_subresource_map = current_map.find(image_state.get());
            const auto &read_subresource_map = (new_write && current_subresource_map != current_map.end())
                                                   ? (*current_subresource_map).second
                                                   : write_subresource_map;

            // Validate aspects in isolation.
            // This is required when handling separate depth-stencil layouts.
            for (uint32_t aspect_index = 0; aspect_index < 32; aspect_index++) {
                VkImageAspectFlags test_aspect = 1u << aspect_index;
                if ((img_barrier.subresourceRange.aspectMask & test_aspect) == 0) {
                    continue;
                }
                auto old_layout = NormalizeSynchronization2Layout(img_barrier.subresourceRange.aspectMask, img_barrier.oldLayout);

                LayoutUseCheckAndMessage layout_check(old_layout, test_aspect);
                auto normalized_isr = image_state->NormalizeSubresourceRange(img_barrier.subresourceRange);
                normalized_isr.aspectMask = test_aspect;
                skip |= read_subresource_map->AnyInRange(
                    normalized_isr, [this, read_subresource_map, cb_state, &layout_check, &loc, &img_barrier](
                                        const LayoutRange &range, const LayoutEntry &state) {
                        bool subres_skip = false;
                        if (!layout_check.Check(state)) {
                            const auto &vuid = GetImageBarrierVUID(loc, ImageError::kConflictingLayout);
                            auto subres = read_subresource_map->Decode(range.begin);
                            subres_skip = LogError(
                                cb_state->commandBuffer(), vuid,
                                "%s %s cannot transition the layout of aspect=%d level=%d layer=%d from %s when the "
                                "%s layout is %s.",
                                loc.Message().c_str(), report_data->FormatHandle(img_barrier.image).c_str(), subres.aspectMask,
                                subres.mipLevel, subres.arrayLayer, string_VkImageLayout(img_barrier.oldLayout),
                                layout_check.message, string_VkImageLayout(layout_check.layout));
                        }
                        return subres_skip;
                    });
                write_subresource_map->SetSubresourceRangeLayout(*cb_state, normalized_isr, img_barrier.newLayout);
            }
        }

        // checks color format and (single-plane or non-disjoint)
        // if ycbcr extension is not supported then single-plane and non-disjoint are always both true
        if ((FormatIsColor(image_format) == true) &&
            ((FormatIsMultiplane(image_format) == false) || (image_state->disjoint == false))) {
            if (aspect_mask != VK_IMAGE_ASPECT_COLOR_BIT) {
                auto error = IsExtEnabled(device_extensions.vk_khr_sampler_ycbcr_conversion) ? ImageError::kNotColorAspect
                                                                                             : ImageError::kNotColorAspectYcbcr;
                const auto &vuid = GetImageBarrierVUID(loc, error);
                skip |= LogError(img_barrier.image, vuid,
                                 "%s references %s of format %s that must be only VK_IMAGE_ASPECT_COLOR_BIT, "
                                 "but its aspectMask is 0x%" PRIx32 ".",
                                 image_loc.Message().c_str(), report_data->FormatHandle(img_barrier.image).c_str(),
                                 string_VkFormat(image_format), aspect_mask);
            }
        }

        VkImageAspectFlags valid_disjoint_mask =
            VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT | VK_IMAGE_ASPECT_COLOR_BIT;
        if ((FormatIsMultiplane(image_format) == true) && (image_state->disjoint == true) &&
            ((aspect_mask & valid_disjoint_mask) == 0)) {
            const auto &vuid = GetImageBarrierVUID(image_loc, ImageError::kBadMultiplanarAspect);
            skip |= LogError(img_barrier.image, vuid,
                             "%s references %s of format %s has aspectMask (0x%" PRIx32
                             ") but needs to include either an VK_IMAGE_ASPECT_PLANE_*_BIT or VK_IMAGE_ASPECT_COLOR_BIT.",
                             image_loc.Message().c_str(), report_data->FormatHandle(img_barrier.image).c_str(),
                             string_VkFormat(image_format), aspect_mask);
        }

        if ((FormatPlaneCount(image_format) == 2) && ((aspect_mask & VK_IMAGE_ASPECT_PLANE_2_BIT) != 0)) {
            const auto &vuid = GetImageBarrierVUID(image_loc, ImageError::kBadPlaneCount);
            skip |= LogError(img_barrier.image, vuid,
                             "%s references %s of format %s has only two planes but included "
                             "VK_IMAGE_ASPECT_PLANE_2_BIT in its aspectMask (0x%" PRIx32 ").",
                             image_loc.Message().c_str(), report_data->FormatHandle(img_barrier.image).c_str(),
                             string_VkFormat(image_format), aspect_mask);
        }
    }
    return skip;
}

template <typename Barrier, typename TransferBarrier>
bool CoreChecks::ValidateQFOTransferBarrierUniqueness(const Location &loc, const CMD_BUFFER_STATE *cb_state, const Barrier &barrier,
                                                      const QFOTransferBarrierSets<TransferBarrier> &barrier_sets) const {
    bool skip = false;
    const char *handle_name = TransferBarrier::HandleName();
    const char *transfer_type = nullptr;
    if (!IsTransferOp(barrier)) {
        return skip;
    }
    const TransferBarrier *barrier_record = nullptr;
    if (cb_state->IsReleaseOp(barrier) && !QueueFamilyIsExternal(barrier.dstQueueFamilyIndex)) {
        const auto found = barrier_sets.release.find(barrier);
        if (found != barrier_sets.release.cend()) {
            barrier_record = &(*found);
            transfer_type = "releasing";
        }
    } else if (cb_state->IsAcquireOp(barrier) && !QueueFamilyIsExternal(barrier.srcQueueFamilyIndex)) {
        const auto found = barrier_sets.acquire.find(barrier);
        if (found != barrier_sets.acquire.cend()) {
            barrier_record = &(*found);
            transfer_type = "acquiring";
        }
    }
    if (barrier_record != nullptr) {
        skip |=
            LogWarning(cb_state->commandBuffer(), TransferBarrier::ErrMsgDuplicateQFOInCB(),
                       "%s %s queue ownership of %s (%s), from srcQueueFamilyIndex %" PRIu32 " to dstQueueFamilyIndex %" PRIu32
                       " duplicates existing barrier recorded in this command buffer.",
                       loc.Message().c_str(), transfer_type, handle_name, report_data->FormatHandle(barrier_record->handle).c_str(),
                       barrier_record->srcQueueFamilyIndex, barrier_record->dstQueueFamilyIndex);
    }
    return skip;
}

VulkanTypedHandle BarrierTypedHandle(const VkImageMemoryBarrier &barrier) {
    return VulkanTypedHandle(barrier.image, kVulkanObjectTypeImage);
}

VulkanTypedHandle BarrierTypedHandle(const VkImageMemoryBarrier2KHR &barrier) {
    return VulkanTypedHandle(barrier.image, kVulkanObjectTypeImage);
}

std::shared_ptr<const IMAGE_STATE> BarrierHandleState(const ValidationStateTracker &device_state,
                                                      const VkImageMemoryBarrier &barrier) {
    return device_state.Get<IMAGE_STATE>(barrier.image);
}

std::shared_ptr<const IMAGE_STATE> BarrierHandleState(const ValidationStateTracker &device_state,
                                                      const VkImageMemoryBarrier2KHR &barrier) {
    return device_state.Get<IMAGE_STATE>(barrier.image);
}

VulkanTypedHandle BarrierTypedHandle(const VkBufferMemoryBarrier &barrier) {
    return VulkanTypedHandle(barrier.buffer, kVulkanObjectTypeBuffer);
}

VulkanTypedHandle BarrierTypedHandle(const VkBufferMemoryBarrier2KHR &barrier) {
    return VulkanTypedHandle(barrier.buffer, kVulkanObjectTypeBuffer);
}

const std::shared_ptr<const BUFFER_STATE> BarrierHandleState(const ValidationStateTracker &device_state,
                                                             const VkBufferMemoryBarrier &barrier) {
    return device_state.Get<BUFFER_STATE>(barrier.buffer);
}

std::shared_ptr<const BUFFER_STATE> BarrierHandleState(const ValidationStateTracker &device_state,
                                                       const VkBufferMemoryBarrier2KHR &barrier) {
    return device_state.Get<BUFFER_STATE>(barrier.buffer);
}

template <typename Barrier, typename TransferBarrier>
void CoreChecks::RecordBarrierValidationInfo(const Location &loc, CMD_BUFFER_STATE *cb_state, const Barrier &barrier,
                                             QFOTransferBarrierSets<TransferBarrier> &barrier_sets) {
    if (IsTransferOp(barrier)) {
        if (cb_state->IsReleaseOp(barrier) && !QueueFamilyIsExternal(barrier.dstQueueFamilyIndex)) {
            barrier_sets.release.emplace(barrier);
        } else if (cb_state->IsAcquireOp(barrier) && !QueueFamilyIsExternal(barrier.srcQueueFamilyIndex)) {
            barrier_sets.acquire.emplace(barrier);
        }
    }

    // 7.7.4: If the values of srcQueueFamilyIndex and dstQueueFamilyIndex are equal, no ownership transfer is performed, and the
    // barrier operates as if they were both set to VK_QUEUE_FAMILY_IGNORED.
    const uint32_t src_queue_family = barrier.srcQueueFamilyIndex;
    const uint32_t dst_queue_family = barrier.dstQueueFamilyIndex;
    const bool is_ownership_transfer = src_queue_family != dst_queue_family;

    if (is_ownership_transfer) {
        // Only enqueue submit time check if it is needed. If more submit time checks are added, change the criteria
        // TODO create a better named list, or rename the submit time lists to something that matches the broader usage...
        auto handle_state = BarrierHandleState(*this, barrier);
        const bool mode_concurrent = handle_state ? handle_state->createInfo.sharingMode == VK_SHARING_MODE_CONCURRENT : false;
        if (!mode_concurrent) {
            const auto typed_handle = BarrierTypedHandle(barrier);
            core_error::LocationCapture loc_capture(loc);
            cb_state->queue_submit_functions.emplace_back(
                [loc_capture, typed_handle, src_queue_family, dst_queue_family](
                    const ValidationStateTracker &device_data, const QUEUE_STATE &queue_state, const CMD_BUFFER_STATE &cb_state) {
                    return ValidateConcurrentBarrierAtSubmit(loc_capture.Get(), device_data, queue_state, cb_state, typed_handle,
                                                             src_queue_family, dst_queue_family);
                });
        }
    }
}

// Verify image barrier image state and that the image is consistent with FB image
template <typename ImgBarrier>
bool CoreChecks::ValidateImageBarrierAttachment(const Location &loc, CMD_BUFFER_STATE const *cb_state,
                                                const FRAMEBUFFER_STATE *framebuffer, uint32_t active_subpass,
                                                const safe_VkSubpassDescription2 &sub_desc, const VkRenderPass rp_handle,
                                                const ImgBarrier &img_barrier, const CMD_BUFFER_STATE *primary_cb_state) const {
    using sync_vuid_maps::GetImageBarrierVUID;
    using sync_vuid_maps::ImageError;

    bool skip = false;
    const auto *fb_state = framebuffer;
    assert(fb_state);
    const auto img_bar_image = img_barrier.image;
    bool image_match = false;
    bool sub_image_found = false;  // Do we find a corresponding subpass description
    VkImageLayout sub_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t attach_index = 0;
    // Verify that a framebuffer image matches barrier image
    const auto attachment_count = fb_state->createInfo.attachmentCount;
    for (uint32_t attachment = 0; attachment < attachment_count; ++attachment) {
        auto view_state = primary_cb_state ? primary_cb_state->GetActiveAttachmentImageViewState(attachment)
                                           : cb_state->GetActiveAttachmentImageViewState(attachment);
        if (view_state && (img_bar_image == view_state->create_info.image)) {
            image_match = true;
            attach_index = attachment;
            break;
        }
    }
    if (image_match) {  // Make sure subpass is referring to matching attachment
        if (sub_desc.pDepthStencilAttachment && sub_desc.pDepthStencilAttachment->attachment == attach_index) {
            sub_image_layout = sub_desc.pDepthStencilAttachment->layout;
            sub_image_found = true;
        }
        if (!sub_image_found && IsExtEnabled(device_extensions.vk_khr_depth_stencil_resolve)) {
            const auto *resolve = LvlFindInChain<VkSubpassDescriptionDepthStencilResolve>(sub_desc.pNext);
            if (resolve && resolve->pDepthStencilResolveAttachment &&
                resolve->pDepthStencilResolveAttachment->attachment == attach_index) {
                sub_image_layout = resolve->pDepthStencilResolveAttachment->layout;
                sub_image_found = true;
            }
        }
        if (!sub_image_found) {
            for (uint32_t j = 0; j < sub_desc.colorAttachmentCount; ++j) {
                if (sub_desc.pColorAttachments && sub_desc.pColorAttachments[j].attachment == attach_index) {
                    sub_image_layout = sub_desc.pColorAttachments[j].layout;
                    sub_image_found = true;
                    break;
                }
                if (!sub_image_found && sub_desc.pResolveAttachments &&
                    sub_desc.pResolveAttachments[j].attachment == attach_index) {
                    sub_image_layout = sub_desc.pResolveAttachments[j].layout;
                    sub_image_found = true;
                    break;
                }
            }
        }
        if (!sub_image_found) {
            auto img_loc = loc.dot(Field::image);
            const auto &vuid = GetImageBarrierVUID(img_loc, ImageError::kRenderPassMismatch);
            skip |=
                LogError(rp_handle, vuid,
                         "%s Barrier for %s is not referenced by the VkSubpassDescription for active subpass (%d) of current %s.",
                         img_loc.Message().c_str(), report_data->FormatHandle(img_bar_image).c_str(), active_subpass,
                         report_data->FormatHandle(rp_handle).c_str());
        }
    } else {  // !image_match
        auto img_loc = loc.dot(Field::image);
        const auto &vuid = GetImageBarrierVUID(img_loc, ImageError::kRenderPassMismatch);
        skip |= LogError(fb_state->framebuffer(), vuid, "%s Barrier for %s does not match an image from the current %s.",
                         img_loc.Message().c_str(), report_data->FormatHandle(img_bar_image).c_str(),
                         report_data->FormatHandle(fb_state->framebuffer()).c_str());
    }
    if (img_barrier.oldLayout != img_barrier.newLayout) {
        auto layout_loc = loc.dot(Field::oldLayout);
        const auto &vuid = GetImageBarrierVUID(layout_loc, ImageError::kRenderPassLayoutChange);
        skip |= LogError(cb_state->commandBuffer(), vuid,
                         "%s As the Image Barrier for %s is being executed within a render pass instance, oldLayout must "
                         "equal newLayout yet they are %s and %s.",
                         layout_loc.Message().c_str(), report_data->FormatHandle(img_barrier.image).c_str(),
                         string_VkImageLayout(img_barrier.oldLayout), string_VkImageLayout(img_barrier.newLayout));
    } else {
        if (sub_image_found && sub_image_layout != img_barrier.oldLayout) {
            const LogObjectList objlist(rp_handle, img_bar_image);
            auto layout_loc = loc.dot(Field::oldLayout);
            const auto &vuid = GetImageBarrierVUID(layout_loc, ImageError::kRenderPassLayoutChange);
            skip |= LogError(objlist, vuid,
                             "%s Barrier for %s is referenced by the VkSubpassDescription for active "
                             "subpass (%d) of current %s as having layout %s, but image barrier has layout %s.",
                             layout_loc.Message().c_str(), report_data->FormatHandle(img_bar_image).c_str(), active_subpass,
                             report_data->FormatHandle(rp_handle).c_str(), string_VkImageLayout(sub_image_layout),
                             string_VkImageLayout(img_barrier.oldLayout));
        }
    }
    return skip;
}
// explictly instantiate so these can be used in core_validation.cpp
template bool CoreChecks::ValidateImageBarrierAttachment(const Location &loc, CMD_BUFFER_STATE const *cb_state,
                                                         const FRAMEBUFFER_STATE *framebuffer, uint32_t active_subpass,
                                                         const safe_VkSubpassDescription2 &sub_desc, const VkRenderPass rp_handle,
                                                         const VkImageMemoryBarrier &img_barrier,
                                                         const CMD_BUFFER_STATE *primary_cb_state) const;
template bool CoreChecks::ValidateImageBarrierAttachment(const Location &loc, CMD_BUFFER_STATE const *cb_state,
                                                         const FRAMEBUFFER_STATE *framebuffer, uint32_t active_subpass,
                                                         const safe_VkSubpassDescription2 &sub_desc, const VkRenderPass rp_handle,
                                                         const VkImageMemoryBarrier2KHR &img_barrier,
                                                         const CMD_BUFFER_STATE *primary_cb_state) const;

template <typename ImgBarrier>
void CoreChecks::EnqueueSubmitTimeValidateImageBarrierAttachment(const Location &loc, CMD_BUFFER_STATE *cb_state,
                                                                 const ImgBarrier &barrier) {
    // Secondary CBs can have null framebuffer so queue up validation in that case 'til FB is known
    if ((cb_state->activeRenderPass) && (VK_NULL_HANDLE == cb_state->activeFramebuffer) &&
        (VK_COMMAND_BUFFER_LEVEL_SECONDARY == cb_state->createInfo.level)) {
        const auto active_subpass = cb_state->activeSubpass;
        const auto rp_state = cb_state->activeRenderPass;
        const auto &sub_desc = rp_state->createInfo.pSubpasses[active_subpass];
        // Secondary CB case w/o FB specified delay validation
        auto *this_ptr = this;  // Required for older compilers with c++20 compatibility
        core_error::LocationCapture loc_capture(loc);
        const auto render_pass = rp_state->renderPass();
        cb_state->cmd_execute_commands_functions.emplace_back(
            [this_ptr, loc_capture, active_subpass, sub_desc, render_pass, barrier](
                const CMD_BUFFER_STATE &secondary_cb, const CMD_BUFFER_STATE *primary_cb, const FRAMEBUFFER_STATE *fb) {
                return this_ptr->ValidateImageBarrierAttachment(loc_capture.Get(), &secondary_cb, fb, active_subpass, sub_desc,
                                                                render_pass, barrier, primary_cb);
            });
    }
}

void CoreChecks::RecordBarriers(Func func_name, CMD_BUFFER_STATE *cb_state, uint32_t bufferBarrierCount,
                                const VkBufferMemoryBarrier *pBufferMemBarriers, uint32_t imageMemBarrierCount,
                                const VkImageMemoryBarrier *pImageMemBarriers) {
    for (uint32_t i = 0; i < bufferBarrierCount; i++) {
        Location loc(func_name, Struct::VkBufferMemoryBarrier, Field::pBufferMemoryBarriers, i);
        RecordBarrierValidationInfo(loc, cb_state, pBufferMemBarriers[i], cb_state->qfo_transfer_buffer_barriers);
    }
    for (uint32_t i = 0; i < imageMemBarrierCount; i++) {
        Location loc(func_name, Struct::VkImageMemoryBarrier, Field::pImageMemoryBarriers, i);
        const auto &img_barrier = pImageMemBarriers[i];
        RecordBarrierValidationInfo(loc, cb_state, img_barrier, cb_state->qfo_transfer_image_barriers);
        EnqueueSubmitTimeValidateImageBarrierAttachment(loc, cb_state, img_barrier);
    }
}

void CoreChecks::RecordBarriers(Func func_name, CMD_BUFFER_STATE *cb_state, const VkDependencyInfoKHR &dep_info) {
    for (uint32_t i = 0; i < dep_info.bufferMemoryBarrierCount; i++) {
        Location loc(func_name, Struct::VkBufferMemoryBarrier2, Field::pBufferMemoryBarriers, i);
        RecordBarrierValidationInfo(loc, cb_state, dep_info.pBufferMemoryBarriers[i], cb_state->qfo_transfer_buffer_barriers);
    }
    for (uint32_t i = 0; i < dep_info.imageMemoryBarrierCount; i++) {
        Location loc(func_name, Struct::VkImageMemoryBarrier2, Field::pImageMemoryBarriers, i);
        const auto &img_barrier = dep_info.pImageMemoryBarriers[i];
        RecordBarrierValidationInfo(loc, cb_state, img_barrier, cb_state->qfo_transfer_image_barriers);
        EnqueueSubmitTimeValidateImageBarrierAttachment(loc, cb_state, img_barrier);
    }
}

template <typename TransferBarrier, typename Scoreboard>
bool CoreChecks::ValidateAndUpdateQFOScoreboard(const debug_report_data *report_data, const CMD_BUFFER_STATE &cb_state,
                                                const char *operation, const TransferBarrier &barrier,
                                                Scoreboard *scoreboard) const {
    // Record to the scoreboard or report that we have a duplication
    bool skip = false;
    auto inserted = scoreboard->emplace(barrier, &cb_state);
    if (!inserted.second && inserted.first->second != &cb_state) {
        // This is a duplication (but don't report duplicates from the same CB, as we do that at record time
        const LogObjectList objlist(cb_state.commandBuffer(), barrier.handle, inserted.first->second->commandBuffer());
        skip = LogWarning(objlist, TransferBarrier::ErrMsgDuplicateQFOInSubmit(),
                          "%s: %s %s queue ownership of %s (%s), from srcQueueFamilyIndex %" PRIu32
                          " to dstQueueFamilyIndex %" PRIu32 " duplicates existing barrier submitted in this batch from %s.",
                          "vkQueueSubmit()", TransferBarrier::BarrierName(), operation, TransferBarrier::HandleName(),
                          report_data->FormatHandle(barrier.handle).c_str(), barrier.srcQueueFamilyIndex,
                          barrier.dstQueueFamilyIndex, report_data->FormatHandle(inserted.first->second->commandBuffer()).c_str());
    }
    return skip;
}

template <typename TransferBarrier>
bool CoreChecks::ValidateQueuedQFOTransferBarriers(
    const CMD_BUFFER_STATE &cb_state, QFOTransferCBScoreboards<TransferBarrier> *scoreboards,
    const GlobalQFOTransferBarrierMap<TransferBarrier> &global_release_barriers) const {
    bool skip = false;
    const auto &cb_barriers = cb_state.GetQFOBarrierSets(TransferBarrier());
    const char *barrier_name = TransferBarrier::BarrierName();
    const char *handle_name = TransferBarrier::HandleName();
    // No release should have an extant duplicate (WARNING)
    for (const auto &release : cb_barriers.release) {
        // Check the global pending release barriers
        const auto set_it = global_release_barriers.find(release.handle);
        if (set_it != global_release_barriers.cend()) {
            const QFOTransferBarrierSet<TransferBarrier> &set_for_handle = set_it->second;
            const auto found = set_for_handle.find(release);
            if (found != set_for_handle.cend()) {
                skip |= LogWarning(cb_state.commandBuffer(), TransferBarrier::ErrMsgDuplicateQFOSubmitted(),
                                   "%s: %s releasing queue ownership of %s (%s), from srcQueueFamilyIndex %" PRIu32
                                   " to dstQueueFamilyIndex %" PRIu32
                                   " duplicates existing barrier queued for execution, without intervening acquire operation.",
                                   "vkQueueSubmit()", barrier_name, handle_name, report_data->FormatHandle(found->handle).c_str(),
                                   found->srcQueueFamilyIndex, found->dstQueueFamilyIndex);
            }
        }
        skip |= ValidateAndUpdateQFOScoreboard(report_data, cb_state, "releasing", release, &scoreboards->release);
    }
    // Each acquire must have a matching release (ERROR)
    for (const auto &acquire : cb_barriers.acquire) {
        const auto set_it = global_release_barriers.find(acquire.handle);
        bool matching_release_found = false;
        if (set_it != global_release_barriers.cend()) {
            const QFOTransferBarrierSet<TransferBarrier> &set_for_handle = set_it->second;
            matching_release_found = set_for_handle.find(acquire) != set_for_handle.cend();
        }
        if (!matching_release_found) {
            skip |= LogError(cb_state.commandBuffer(), TransferBarrier::ErrMsgMissingQFOReleaseInSubmit(),
                             "%s: in submitted command buffer %s acquiring ownership of %s (%s), from srcQueueFamilyIndex %" PRIu32
                             " to dstQueueFamilyIndex %" PRIu32 " has no matching release barrier queued for execution.",
                             "vkQueueSubmit()", barrier_name, handle_name, report_data->FormatHandle(acquire.handle).c_str(),
                             acquire.srcQueueFamilyIndex, acquire.dstQueueFamilyIndex);
        }
        skip |= ValidateAndUpdateQFOScoreboard(report_data, cb_state, "acquiring", acquire, &scoreboards->acquire);
    }
    return skip;
}

bool CoreChecks::ValidateQueuedQFOTransfers(const CMD_BUFFER_STATE &cb_state,
                                            QFOTransferCBScoreboards<QFOImageTransferBarrier> *qfo_image_scoreboards,
                                            QFOTransferCBScoreboards<QFOBufferTransferBarrier> *qfo_buffer_scoreboards) const {
    bool skip = false;
    skip |=
        ValidateQueuedQFOTransferBarriers<QFOImageTransferBarrier>(cb_state, qfo_image_scoreboards, qfo_release_image_barrier_map);
    skip |= ValidateQueuedQFOTransferBarriers<QFOBufferTransferBarrier>(cb_state, qfo_buffer_scoreboards,
                                                                        qfo_release_buffer_barrier_map);
    return skip;
}

template <typename TransferBarrier>
void RecordQueuedQFOTransferBarriers(QFOTransferBarrierSets<TransferBarrier> &cb_barriers,
                                     GlobalQFOTransferBarrierMap<TransferBarrier> &global_release_barriers) {
    // Add release barriers from this submit to the global map
    for (const auto &release : cb_barriers.release) {
        // the global barrier list is mapped by resource handle to allow cleanup on resource destruction
        // NOTE: vl_concurrent_ordered_map::find() makes a thread safe copy of the result, so we must
        // copy back after updating.
        auto iter = global_release_barriers.find(release.handle);
        iter->second.insert(release);
        global_release_barriers.insert_or_assign(release.handle, iter->second);
    }

    // Erase acquired barriers from this submit from the global map -- essentially marking releases as consumed
    for (const auto &acquire : cb_barriers.acquire) {
        // NOTE: We're not using [] because we don't want to create entries for missing releases
        auto set_it = global_release_barriers.find(acquire.handle);
        if (set_it != global_release_barriers.end()) {
            QFOTransferBarrierSet<TransferBarrier> &set_for_handle = set_it->second;
            set_for_handle.erase(acquire);
            if (set_for_handle.size() == 0) {  // Clean up empty sets
                global_release_barriers.erase(acquire.handle);
            } else {
                // NOTE: vl_concurrent_ordered_map::find() makes a thread safe copy of the result, so we must
                // copy back after updating.
                global_release_barriers.insert_or_assign(acquire.handle, set_for_handle);
            }
        }
    }
}

void CoreChecks::RecordQueuedQFOTransfers(CMD_BUFFER_STATE *cb_state) {
    RecordQueuedQFOTransferBarriers<QFOImageTransferBarrier>(cb_state->qfo_transfer_image_barriers, qfo_release_image_barrier_map);
    RecordQueuedQFOTransferBarriers<QFOBufferTransferBarrier>(cb_state->qfo_transfer_buffer_barriers,
                                                              qfo_release_buffer_barrier_map);
}

template <typename ImgBarrier>
void CoreChecks::TransitionImageLayouts(CMD_BUFFER_STATE *cb_state, uint32_t barrier_count, const ImgBarrier *barriers) {
    // For ownership transfers, the barrier is specified twice; as a release
    // operation on the yielding queue family, and as an acquire operation
    // on the acquiring queue family. This barrier may also include a layout
    // transition, which occurs 'between' the two operations. For validation
    // purposes it doesn't seem important which side performs the layout
    // transition, but it must not be performed twice. We'll arbitrarily
    // choose to perform it as part of the acquire operation.
    //
    // However, we still need to record initial layout for the "initial layout" validation
    for (uint32_t i = 0; i < barrier_count; i++) {
        const auto &mem_barrier = barriers[i];
        const bool is_release_op = cb_state->IsReleaseOp(mem_barrier);
        auto image_state = Get<IMAGE_STATE>(mem_barrier.image);
        if (image_state) {
            RecordTransitionImageLayout(cb_state, image_state.get(), mem_barrier, is_release_op);
        }
    }
}
// explictly instantiate this template so it can be used in core_validation.cpp
template void CoreChecks::TransitionImageLayouts(CMD_BUFFER_STATE *cb_state, uint32_t barrier_count,
                                                 const VkImageMemoryBarrier *barrier);
template void CoreChecks::TransitionImageLayouts(CMD_BUFFER_STATE *cb_state, uint32_t barrier_count,
                                                 const VkImageMemoryBarrier2KHR *barrier);

VkImageLayout NormalizeSynchronization2Layout(const VkImageAspectFlags aspect_mask, VkImageLayout layout);

template <typename ImgBarrier>
void CoreChecks::RecordTransitionImageLayout(CMD_BUFFER_STATE *cb_state, const IMAGE_STATE *image_state,
                                             const ImgBarrier &mem_barrier, bool is_release_op) {
    if (enabled_features.core13.synchronization2) {
        if (mem_barrier.oldLayout == mem_barrier.newLayout) {
            return;
        }
    }
    auto normalized_isr = image_state->NormalizeSubresourceRange(mem_barrier.subresourceRange);

    VkImageLayout initial_layout = NormalizeSynchronization2Layout(mem_barrier.subresourceRange.aspectMask, mem_barrier.oldLayout);
    VkImageLayout new_layout = NormalizeSynchronization2Layout(mem_barrier.subresourceRange.aspectMask, mem_barrier.newLayout);

    // Layout transitions in external instance are not tracked, so don't validate initial layout.
    if (QueueFamilyIsExternal(mem_barrier.srcQueueFamilyIndex)) {
        initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    if (is_release_op) {
        cb_state->SetImageInitialLayout(*image_state, normalized_isr, initial_layout);
    } else {
        cb_state->SetImageLayout(*image_state, normalized_isr, new_layout, initial_layout);
    }
}

template <typename RangeFactory>
bool CoreChecks::VerifyImageLayoutRange(const CMD_BUFFER_STATE &cb_state, const IMAGE_STATE &image_state,
                                        VkImageAspectFlags aspect_mask, VkImageLayout explicit_layout,
                                        const RangeFactory &range_factory, const char *caller, const char *layout_mismatch_msg_code,
                                        bool *error) const {
    bool skip = false;
    const auto *subresource_map = cb_state.GetImageSubresourceLayoutMap(image_state);
    if (!subresource_map) return skip;

    LayoutUseCheckAndMessage layout_check(explicit_layout, aspect_mask);
    skip |= subresource_map->AnyInRange(
        range_factory(*subresource_map), [this, subresource_map, &cb_state, &image_state, &layout_check, layout_mismatch_msg_code,
                                          caller, error](const LayoutRange &range, const LayoutEntry &state) {
            bool subres_skip = false;
            if (!layout_check.Check(state)) {
                *error = true;
                auto subres = subresource_map->Decode(range.begin);
                subres_skip |= LogError(cb_state.commandBuffer(), layout_mismatch_msg_code,
                                        "%s: Cannot use %s (layer=%" PRIu32 " mip=%" PRIu32
                                        ") with specific layout %s that doesn't match the "
                                        "%s layout %s.",
                                        caller, report_data->FormatHandle(image_state.Handle()).c_str(), subres.arrayLayer,
                                        subres.mipLevel, string_VkImageLayout(layout_check.expected_layout), layout_check.message,
                                        string_VkImageLayout(layout_check.layout));
            }
            return subres_skip;
        });

    return skip;
}

bool CoreChecks::VerifyImageLayout(const CMD_BUFFER_STATE &cb_state, const IMAGE_STATE &image_state,
                                   const VkImageSubresourceRange &range, VkImageAspectFlags aspect_mask,
                                   VkImageLayout explicit_layout, VkImageLayout optimal_layout, const char *caller,
                                   const char *layout_invalid_msg_code, const char *layout_mismatch_msg_code, bool *error) const {
    if (disabled[image_layout_validation]) return false;
    bool skip = false;

    VkImageSubresourceRange normalized_isr = image_state.NormalizeSubresourceRange(range);
    auto range_factory = [&normalized_isr](const ImageSubresourceLayoutMap &map) { return map.RangeGen(normalized_isr); };
    skip |= VerifyImageLayoutRange(cb_state, image_state, aspect_mask, explicit_layout, range_factory, caller,
                                   layout_mismatch_msg_code, error);

    // If optimal_layout is not UNDEFINED, check that layout matches optimal for this case
    if ((VK_IMAGE_LAYOUT_UNDEFINED != optimal_layout) && (explicit_layout != optimal_layout)) {
        if (VK_IMAGE_LAYOUT_GENERAL == explicit_layout) {
            if (image_state.createInfo.tiling != VK_IMAGE_TILING_LINEAR) {
                // LAYOUT_GENERAL is allowed, but may not be performance optimal, flag as perf warning.
                skip |= LogPerformanceWarning(cb_state.commandBuffer(), kVUID_Core_DrawState_InvalidImageLayout,
                                              "%s: For optimal performance %s layout should be %s instead of GENERAL.", caller,
                                              report_data->FormatHandle(image_state.Handle()).c_str(),
                                              string_VkImageLayout(optimal_layout));
            }
        } else if (IsExtEnabled(device_extensions.vk_khr_shared_presentable_image)) {
            if (image_state.shared_presentable) {
                if (VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR != explicit_layout) {
                    skip |=
                        LogError(device, layout_invalid_msg_code,
                                 "%s: Layout for shared presentable image is %s but must be VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR.",
                                 caller, string_VkImageLayout(optimal_layout));
                }
            }
        } else {
            *error = true;
            skip |= LogError(cb_state.commandBuffer(), layout_invalid_msg_code,
                             "%s: Layout for %s is %s but can only be %s or VK_IMAGE_LAYOUT_GENERAL.", caller,
                             report_data->FormatHandle(image_state.Handle()).c_str(), string_VkImageLayout(explicit_layout),
                             string_VkImageLayout(optimal_layout));
        }
    }
    return skip;
}
bool CoreChecks::VerifyImageLayout(const CMD_BUFFER_STATE &cb_state, const IMAGE_STATE &image_state,
                                   const VkImageSubresourceLayers &subLayers, VkImageLayout explicit_layout,
                                   VkImageLayout optimal_layout, const char *caller, const char *layout_invalid_msg_code,
                                   const char *layout_mismatch_msg_code, bool *error) const {
    return VerifyImageLayout(cb_state, image_state, RangeFromLayers(subLayers), explicit_layout, optimal_layout, caller,
                             layout_invalid_msg_code, layout_mismatch_msg_code, error);
}

bool CoreChecks::VerifyImageLayout(const CMD_BUFFER_STATE &cb_state, const IMAGE_VIEW_STATE &image_view_state,
                                   VkImageLayout explicit_layout, const char *caller, const char *layout_mismatch_msg_code,
                                   bool *error) const {
    if (disabled[image_layout_validation]) return false;
    assert(image_view_state.image_state);
    auto range_factory = [&image_view_state](const ImageSubresourceLayoutMap &map) {
        return image_layout_map::RangeGenerator(image_view_state.range_generator);
    };

    return VerifyImageLayoutRange(cb_state, *image_view_state.image_state, image_view_state.create_info.subresourceRange.aspectMask,
                                  explicit_layout, range_factory, caller, layout_mismatch_msg_code, error);
}

bool CoreChecks::VerifyImageLayout(const CMD_BUFFER_STATE &cb_state, const IMAGE_STATE &image_state,
                                   const VkImageSubresourceRange &range, VkImageLayout explicit_layout, const char *caller,
                                   const char *layout_mismatch_msg_code, bool *error) const {
    if (disabled[image_layout_validation]) return false;

    auto range_factory = [&range](const ImageSubresourceLayoutMap &map) { return map.RangeGen(range); };

    return VerifyImageLayoutRange(cb_state, image_state, range.aspectMask, explicit_layout, range_factory, caller,
                                  layout_mismatch_msg_code, error);
}

void CoreChecks::TransitionFinalSubpassLayouts(CMD_BUFFER_STATE *cb_state, const VkRenderPassBeginInfo *pRenderPassBegin,
                                               FRAMEBUFFER_STATE *framebuffer_state) {
    auto render_pass = Get<RENDER_PASS_STATE>(pRenderPassBegin->renderPass);
    if (!render_pass) return;

    const VkRenderPassCreateInfo2 *render_pass_info = render_pass->createInfo.ptr();
    if (framebuffer_state) {
        for (uint32_t i = 0; i < render_pass_info->attachmentCount; ++i) {
            auto *view_state = cb_state->GetActiveAttachmentImageViewState(i);
            if (view_state) {
                VkImageLayout stencil_layout = kInvalidLayout;
                const auto *attachment_description_stencil_layout =
                    LvlFindInChain<VkAttachmentDescriptionStencilLayout>(render_pass_info->pAttachments[i].pNext);
                if (attachment_description_stencil_layout) {
                    stencil_layout = attachment_description_stencil_layout->stencilFinalLayout;
                }
                cb_state->SetImageViewLayout(*view_state, render_pass_info->pAttachments[i].finalLayout, stencil_layout);
            }
        }
    }
}

bool CoreChecks::ValidateImageFormatFeatures(const VkImageCreateInfo *pCreateInfo) const {
    bool skip = false;

    // validates based on imageCreateFormatFeatures from vkspec.html#resources-image-creation-limits
    VkFormatFeatureFlags2KHR tiling_features = 0;
    const VkImageTiling image_tiling = pCreateInfo->tiling;
    const VkFormat image_format = pCreateInfo->format;

    if (image_format == VK_FORMAT_UNDEFINED) {
        // VU 01975 states format can't be undefined unless an android externalFormat
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        const VkExternalFormatANDROID *ext_fmt_android = LvlFindInChain<VkExternalFormatANDROID>(pCreateInfo->pNext);
        if ((image_tiling == VK_IMAGE_TILING_OPTIMAL) && (ext_fmt_android != nullptr) && (0 != ext_fmt_android->externalFormat)) {
            auto it = ahb_ext_formats_map.find(ext_fmt_android->externalFormat);
            if (it != ahb_ext_formats_map.end()) {
                tiling_features = it->second;
            }
        }
#endif
    } else if (image_tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        layer_data::unordered_set<uint64_t> drm_format_modifiers;
        const VkImageDrmFormatModifierExplicitCreateInfoEXT *drm_explicit =
            LvlFindInChain<VkImageDrmFormatModifierExplicitCreateInfoEXT>(pCreateInfo->pNext);
        const VkImageDrmFormatModifierListCreateInfoEXT *drm_implicit =
            LvlFindInChain<VkImageDrmFormatModifierListCreateInfoEXT>(pCreateInfo->pNext);

        if (drm_explicit != nullptr) {
            drm_format_modifiers.insert(drm_explicit->drmFormatModifier);
        } else {
            // VUID 02261 makes sure its only explict or implict in parameter checking
            assert(drm_implicit != nullptr);
            for (uint32_t i = 0; i < drm_implicit->drmFormatModifierCount; i++) {
                drm_format_modifiers.insert(drm_implicit->pDrmFormatModifiers[i]);
            }
        }

        auto fmt_drm_props = LvlInitStruct<VkDrmFormatModifierPropertiesListEXT>();
        auto fmt_props_2 = LvlInitStruct<VkFormatProperties2>(&fmt_drm_props);
        DispatchGetPhysicalDeviceFormatProperties2(physical_device, image_format, &fmt_props_2);
        std::vector<VkDrmFormatModifierPropertiesEXT> drm_properties;
        drm_properties.resize(fmt_drm_props.drmFormatModifierCount);
        fmt_drm_props.pDrmFormatModifierProperties = drm_properties.data();
        DispatchGetPhysicalDeviceFormatProperties2(physical_device, image_format, &fmt_props_2);

        for (uint32_t i = 0; i < fmt_drm_props.drmFormatModifierCount; i++) {
            if (drm_format_modifiers.find(fmt_drm_props.pDrmFormatModifierProperties[i].drmFormatModifier) !=
                drm_format_modifiers.end()) {
                tiling_features |= fmt_drm_props.pDrmFormatModifierProperties[i].drmFormatModifierTilingFeatures;
            }
        }
    } else {
        VkFormatProperties3KHR format_properties = GetPDFormatProperties(image_format);
        tiling_features = (image_tiling == VK_IMAGE_TILING_LINEAR) ? format_properties.linearTilingFeatures
                                                                   : format_properties.optimalTilingFeatures;
    }

    // Lack of disjoint format feature support while using the flag
    if (FormatIsMultiplane(image_format) && ((pCreateInfo->flags & VK_IMAGE_CREATE_DISJOINT_BIT) != 0) &&
        ((tiling_features & VK_FORMAT_FEATURE_2_DISJOINT_BIT_KHR) == 0)) {
        skip |= LogError(device, "VUID-VkImageCreateInfo-imageCreateFormatFeatures-02260",
                         "vkCreateImage(): can't use VK_IMAGE_CREATE_DISJOINT_BIT because %s doesn't support "
                         "VK_FORMAT_FEATURE_DISJOINT_BIT based on imageCreateFormatFeatures.",
                         string_VkFormat(pCreateInfo->format));
    }

    return skip;
}

bool CoreChecks::PreCallValidateCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                                            const VkAllocationCallbacks *pAllocator, VkImage *pImage) const {
    bool skip = false;

    if (IsExtEnabled(device_extensions.vk_android_external_memory_android_hardware_buffer)) {
        skip |= ValidateCreateImageANDROID(report_data, pCreateInfo);
    } else {  // These checks are omitted or replaced when Android HW Buffer extension is active
        if (pCreateInfo->format == VK_FORMAT_UNDEFINED) {
            return LogError(device, "VUID-VkImageCreateInfo-format-00943",
                            "vkCreateImage(): VkFormat for image must not be VK_FORMAT_UNDEFINED.");
        }
    }

    if (pCreateInfo->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
        if (VK_IMAGE_TYPE_2D != pCreateInfo->imageType) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-flags-00949",
                             "vkCreateImage(): Image type must be VK_IMAGE_TYPE_2D when VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT "
                             "flag bit is set");
        }
    }

    const VkPhysicalDeviceLimits *device_limits = &phys_dev_props.limits;
    VkImageUsageFlags attach_flags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if ((pCreateInfo->usage & attach_flags) && (pCreateInfo->extent.width > device_limits->maxFramebufferWidth)) {
        skip |= LogError(device, "VUID-VkImageCreateInfo-usage-00964",
                         "vkCreateImage(): Image usage flags include a frame buffer attachment bit and image width (%" PRIu32
                         ") exceeds "
                         "device maxFramebufferWidth (%" PRIu32 ").",
                         pCreateInfo->extent.width, device_limits->maxFramebufferWidth);
    }

    if ((pCreateInfo->usage & attach_flags) && (pCreateInfo->extent.height > device_limits->maxFramebufferHeight)) {
        skip |= LogError(device, "VUID-VkImageCreateInfo-usage-00965",
                         "vkCreateImage(): Image usage flags include a frame buffer attachment bit and image height (%" PRIu32
                         ") exceeds "
                         "device maxFramebufferHeight (%" PRIu32 ").",
                         pCreateInfo->extent.height, device_limits->maxFramebufferHeight);
    }

    VkImageCreateFlags sparseFlags =
        VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
    if ((pCreateInfo->flags & sparseFlags) && (pCreateInfo->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)) {
        skip |= LogError(device, "VUID-VkImageCreateInfo-None-01925",
                         "vkCreateImage(): images using sparse memory cannot have VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT set");
    }

    if (!enabled_features.fragment_density_map_offset_features.fragmentDensityMapOffset) {
        uint32_t ceiling_width = static_cast<uint32_t>(ceilf(
            static_cast<float>(device_limits->maxFramebufferWidth) /
            std::max(static_cast<float>(phys_dev_ext_props.fragment_density_map_props.minFragmentDensityTexelSize.width), 1.0f)));
        if ((pCreateInfo->usage & VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT) && (pCreateInfo->extent.width > ceiling_width)) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-fragmentDensityMapOffset-06514",
                             "vkCreateImage(): Image usage flags include a fragment density map bit and image width (%" PRIu32
                             ") exceeds the "
                             "ceiling of device "
                             "maxFramebufferWidth (%" PRIu32 ") / minFragmentDensityTexelSize.width (%" PRIu32
                             "). The ceiling value: %" PRIu32 "",
                             pCreateInfo->extent.width, device_limits->maxFramebufferWidth,
                             phys_dev_ext_props.fragment_density_map_props.minFragmentDensityTexelSize.width, ceiling_width);
        }

        uint32_t ceiling_height = static_cast<uint32_t>(ceilf(
            static_cast<float>(device_limits->maxFramebufferHeight) /
            std::max(static_cast<float>(phys_dev_ext_props.fragment_density_map_props.minFragmentDensityTexelSize.height), 1.0f)));
        if ((pCreateInfo->usage & VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT) && (pCreateInfo->extent.height > ceiling_height)) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-fragmentDensityMapOffset-06515",
                             "vkCreateImage(): Image usage flags include a fragment density map bit and image height (%" PRIu32
                             ") exceeds the "
                             "ceiling of device "
                             "maxFramebufferHeight (%" PRIu32 ") / minFragmentDensityTexelSize.height (%" PRIu32
                             "). The ceiling value: %" PRIu32 "",
                             pCreateInfo->extent.height, device_limits->maxFramebufferHeight,
                             phys_dev_ext_props.fragment_density_map_props.minFragmentDensityTexelSize.height, ceiling_height);
        }
    }

    VkImageFormatProperties format_limits = {};
    VkResult result = VK_SUCCESS;
    if (pCreateInfo->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        result = DispatchGetPhysicalDeviceImageFormatProperties(physical_device, pCreateInfo->format, pCreateInfo->imageType,
                                                                pCreateInfo->tiling, pCreateInfo->usage, pCreateInfo->flags,
                                                                &format_limits);
    } else {
        auto modifier_list = LvlFindInChain<VkImageDrmFormatModifierListCreateInfoEXT>(pCreateInfo->pNext);
        auto explicit_modifier = LvlFindInChain<VkImageDrmFormatModifierExplicitCreateInfoEXT>(pCreateInfo->pNext);
        if (modifier_list) {
            for (uint32_t i = 0; i < modifier_list->drmFormatModifierCount; i++) {
                auto drm_format_modifier = LvlInitStruct<VkPhysicalDeviceImageDrmFormatModifierInfoEXT>();
                drm_format_modifier.drmFormatModifier = modifier_list->pDrmFormatModifiers[i];
                auto image_format_info = LvlInitStruct<VkPhysicalDeviceImageFormatInfo2>(&drm_format_modifier);
                image_format_info.type = pCreateInfo->imageType;
                image_format_info.format = pCreateInfo->format;
                image_format_info.tiling = pCreateInfo->tiling;
                image_format_info.usage = pCreateInfo->usage;
                image_format_info.flags = pCreateInfo->flags;
                auto image_format_properties = LvlInitStruct<VkImageFormatProperties2>();

                result =
                    DispatchGetPhysicalDeviceImageFormatProperties2(physical_device, &image_format_info, &image_format_properties);
                format_limits = image_format_properties.imageFormatProperties;

                /* The application gives a list of modifier and the driver
                 * selects one. If one is wrong, stop there.
                 */
                if (result != VK_SUCCESS) break;
            }
        } else if (explicit_modifier) {
            auto drm_format_modifier = LvlInitStruct<VkPhysicalDeviceImageDrmFormatModifierInfoEXT>();
            drm_format_modifier.drmFormatModifier = explicit_modifier->drmFormatModifier;
            auto image_format_info = LvlInitStruct<VkPhysicalDeviceImageFormatInfo2>(&drm_format_modifier);
            image_format_info.type = pCreateInfo->imageType;
            image_format_info.format = pCreateInfo->format;
            image_format_info.tiling = pCreateInfo->tiling;
            image_format_info.usage = pCreateInfo->usage;
            image_format_info.flags = pCreateInfo->flags;
            auto image_format_properties = LvlInitStruct<VkImageFormatProperties2>();

            result = DispatchGetPhysicalDeviceImageFormatProperties2(physical_device, &image_format_info, &image_format_properties);
            format_limits = image_format_properties.imageFormatProperties;
        }
    }

    // 1. vkGetPhysicalDeviceImageFormatProperties[2] only success code is VK_SUCCESS
    // 2. If call returns an error, then "imageCreateImageFormatPropertiesList" is defined to be the empty list
    // 3. All values in 02251 are undefined if "imageCreateImageFormatPropertiesList" is empty.
    if (result != VK_SUCCESS) {
        // External memory will always have a "imageCreateImageFormatPropertiesList" so skip
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        if (!LvlFindInChain<VkExternalFormatANDROID>(pCreateInfo->pNext)) {
#endif  // VK_USE_PLATFORM_ANDROID_KHR
            skip |= LogError(device, "VUID-VkImageCreateInfo-imageCreateMaxMipLevels-02251",
                             "vkCreateImage(): Format %s is not supported for this combination of parameters and "
                             "VkGetPhysicalDeviceImageFormatProperties returned back %s.",
                             string_VkFormat(pCreateInfo->format), string_VkResult(result));
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        }
#endif  // VK_USE_PLATFORM_ANDROID_KHR
    } else {
        if (pCreateInfo->mipLevels > format_limits.maxMipLevels) {
            const char *format_string = string_VkFormat(pCreateInfo->format);
            skip |= LogError(device, "VUID-VkImageCreateInfo-mipLevels-02255",
                             "vkCreateImage(): Image mip levels=%d exceed image format maxMipLevels=%d for format %s.",
                             pCreateInfo->mipLevels, format_limits.maxMipLevels, format_string);
        }

        uint64_t texel_count = static_cast<uint64_t>(pCreateInfo->extent.width) *
                               static_cast<uint64_t>(pCreateInfo->extent.height) *
                               static_cast<uint64_t>(pCreateInfo->extent.depth) * static_cast<uint64_t>(pCreateInfo->arrayLayers) *
                               static_cast<uint64_t>(pCreateInfo->samples);

        // Depth/Stencil formats size can't be accurately calculated
        if (!FormatIsDepthAndStencil(pCreateInfo->format)) {
            uint64_t total_size =
                static_cast<uint64_t>(std::ceil(FormatTexelSize(pCreateInfo->format) * static_cast<double>(texel_count)));

            // Round up to imageGranularity boundary
            VkDeviceSize image_granularity = phys_dev_props.limits.bufferImageGranularity;
            uint64_t ig_mask = image_granularity - 1;
            total_size = (total_size + ig_mask) & ~ig_mask;

            if (total_size > format_limits.maxResourceSize) {
                skip |= LogWarning(device, kVUID_Core_Image_InvalidFormatLimitsViolation,
                                   "vkCreateImage(): resource size exceeds allowable maximum Image resource size = 0x%" PRIxLEAST64
                                   ", maximum resource size = 0x%" PRIxLEAST64 " ",
                                   total_size, format_limits.maxResourceSize);
            }
        }

        if (pCreateInfo->arrayLayers > format_limits.maxArrayLayers) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-arrayLayers-02256",
                             "vkCreateImage(): arrayLayers=%d exceeds allowable maximum supported by format of %d.",
                             pCreateInfo->arrayLayers, format_limits.maxArrayLayers);
        }

        if ((pCreateInfo->samples & format_limits.sampleCounts) == 0) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-samples-02258",
                             "vkCreateImage(): samples %s is not supported by format 0x%.8X.",
                             string_VkSampleCountFlagBits(pCreateInfo->samples), format_limits.sampleCounts);
        }

        if (pCreateInfo->extent.width > format_limits.maxExtent.width) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-extent-02252",
                             "vkCreateImage(): extent.width %" PRIu32 " exceeds allowable maximum image extent width %" PRIu32 ".",
                             pCreateInfo->extent.width, format_limits.maxExtent.width);
        }

        if (pCreateInfo->extent.height > format_limits.maxExtent.height) {
            skip |=
                LogError(device, "VUID-VkImageCreateInfo-extent-02253",
                         "vkCreateImage(): extent.height %" PRIu32 " exceeds allowable maximum image extent height %" PRIu32 ".",
                         pCreateInfo->extent.height, format_limits.maxExtent.height);
        }

        if (pCreateInfo->extent.depth > format_limits.maxExtent.depth) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-extent-02254",
                             "vkCreateImage(): extent.depth %" PRIu32 " exceeds allowable maximum image extent depth %" PRIu32 ".",
                             pCreateInfo->extent.depth, format_limits.maxExtent.depth);
        }
    }

    // Tests for "Formats requiring sampler YCBCR conversion for VK_IMAGE_ASPECT_COLOR_BIT image views"
    if (FormatRequiresYcbcrConversionExplicitly(pCreateInfo->format)) {
        if (pCreateInfo->mipLevels != 1) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-format-06410",
                             "vkCreateImage(): mipLevels = %d, but when using a YCbCr Conversion format, mipLevels must be 1",
                             pCreateInfo->mipLevels);
        }

        if (pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT) {
            skip |= LogError(
                device, "VUID-VkImageCreateInfo-format-06411",
                "vkCreateImage(): samples = %s, but when using a YCbCr Conversion format, samples must be VK_SAMPLE_COUNT_1_BIT",
                string_VkSampleCountFlagBits(pCreateInfo->samples));
        }

        if (pCreateInfo->imageType != VK_IMAGE_TYPE_2D) {
            skip |= LogError(
                device, "VUID-VkImageCreateInfo-format-06412",
                "vkCreateImage(): imageType = %s, but when using a YCbCr Conversion format, imageType must be VK_IMAGE_TYPE_2D ",
                string_VkImageType(pCreateInfo->imageType));
        }
    }

    if (IsExtEnabled(device_extensions.vk_khr_maintenance2)) {
        if (pCreateInfo->flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) {
            if (!FormatIsCompressed(pCreateInfo->format)) {
                skip |= LogError(device, "VUID-VkImageCreateInfo-flags-01572",
                                 "vkCreateImage(): If pCreateInfo->flags contains VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT, "
                                 "format must be a compressed image format, but is %s",
                                 string_VkFormat(pCreateInfo->format));
            }
            if (!(pCreateInfo->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) {
                skip |= LogError(device, "VUID-VkImageCreateInfo-flags-01573",
                                 "vkCreateImage(): If pCreateInfo->flags contains VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT, "
                                 "flags must also contain VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT.");
            }
        }
    }

    if (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT && pCreateInfo->pQueueFamilyIndices) {
        const char *vuid = IsExtEnabled(device_extensions.vk_khr_get_physical_device_properties2)
                               ? "VUID-VkImageCreateInfo-sharingMode-01420"
                               : "VUID-VkImageCreateInfo-sharingMode-01392";
        skip |= ValidatePhysicalDeviceQueueFamilies(pCreateInfo->queueFamilyIndexCount, pCreateInfo->pQueueFamilyIndices,
                                                    "vkCreateImage", "pCreateInfo->pQueueFamilyIndices", vuid);
    }

    if (!FormatIsMultiplane(pCreateInfo->format) && !(pCreateInfo->flags & VK_IMAGE_CREATE_ALIAS_BIT) &&
        (pCreateInfo->flags & VK_IMAGE_CREATE_DISJOINT_BIT)) {
        skip |=
            LogError(device, "VUID-VkImageCreateInfo-format-01577",
                     "vkCreateImage(): format is %s and flags are %s. The flags should not include VK_IMAGE_CREATE_DISJOINT_BIT.",
                     string_VkFormat(pCreateInfo->format), string_VkImageCreateFlags(pCreateInfo->flags).c_str());
    }

    const auto swapchain_create_info = LvlFindInChain<VkImageSwapchainCreateInfoKHR>(pCreateInfo->pNext);
    if (swapchain_create_info != nullptr) {
        if (swapchain_create_info->swapchain != VK_NULL_HANDLE) {
            auto swapchain_state = Get<SWAPCHAIN_NODE>(swapchain_create_info->swapchain);
            const VkSwapchainCreateFlagsKHR swapchain_flags = swapchain_state->createInfo.flags;

            // Validate rest of Swapchain Image create check that require swapchain state
            const char *vuid = "VUID-VkImageSwapchainCreateInfoKHR-swapchain-00995";
            if (((swapchain_flags & VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR) != 0) &&
                ((pCreateInfo->flags & VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT) == 0)) {
                skip |= LogError(
                    device, vuid,
                    "vkCreateImage(): Swapchain was created with VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR flag so "
                    "all swapchain images must have the VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT flag set.");
            }
            if (((swapchain_flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) != 0) &&
                ((pCreateInfo->flags & VK_IMAGE_CREATE_PROTECTED_BIT) == 0)) {
                skip |= LogError(device, vuid,
                                 "vkCreateImage(): Swapchain was created with VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR flag so all "
                                 "swapchain images must have the VK_IMAGE_CREATE_PROTECTED_BIT flag set.");
            }
            const VkImageCreateFlags mutable_flags = (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
            if (((swapchain_flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0) &&
                ((pCreateInfo->flags & mutable_flags) != mutable_flags)) {
                skip |= LogError(device, vuid,
                                 "vkCreateImage(): Swapchain was created with VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR flag so "
                                 "all swapchain images must have the VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and "
                                 "VK_IMAGE_CREATE_EXTENDED_USAGE_BIT flags both set.");
            }
        }
    }

    if ((pCreateInfo->flags & VK_IMAGE_CREATE_PROTECTED_BIT) != 0) {
        if (enabled_features.core11.protectedMemory == VK_FALSE) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-flags-01890",
                             "vkCreateImage(): the protectedMemory device feature is disabled: Images cannot be created with the "
                             "VK_IMAGE_CREATE_PROTECTED_BIT set.");
        }
        const VkImageCreateFlags invalid_flags =
            VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
        if ((pCreateInfo->flags & invalid_flags) != 0) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-None-01891",
                             "vkCreateImage(): VK_IMAGE_CREATE_PROTECTED_BIT is set so no sparse create flags can be used at same "
                             "time (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | "
                             "VK_IMAGE_CREATE_SPARSE_ALIASED_BIT).");
        }
    }

    if ((pCreateInfo->flags & VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT) != 0) {
        if (!(enabled_features.multisampled_render_to_single_sampled_features.multisampledRenderToSingleSampled)) {
            skip |= LogError(
                device, "VUID-VkImageCreateInfo-multisampledRenderToSingleSampled-06882",
                "vkCreateImage(): pCreateInfo.flags contains VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT "
                "but the multisampledRenderToSingleSampled feature is not enabled");
        }
        if (pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT) {
            skip |= LogError(
                device, "VUID-VkImageCreateInfo-flags-06883",
                "vkCreateImage(): pCreateInfo.flags contains VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT "
                "but samples (%s) is not equal to VK_SAMPLE_COUNT_1_BIT",
                string_VkSampleCountFlagBits(pCreateInfo->samples));
        }
    }

    skip |= ValidateImageFormatFeatures(pCreateInfo);

    // Check compatibility with VK_KHR_portability_subset
    if (IsExtEnabled(device_extensions.vk_khr_portability_subset)) {
        if (VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT & pCreateInfo->flags &&
            VK_FALSE == enabled_features.portability_subset_features.imageView2DOn3DImage) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-imageView2DOn3DImage-04459",
                             "vkCreateImage (portability error): VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT is not supported.");
        }
        if ((VK_SAMPLE_COUNT_1_BIT != pCreateInfo->samples) && (1 != pCreateInfo->arrayLayers) &&
            (VK_FALSE == enabled_features.portability_subset_features.multisampleArrayImage)) {
            skip |=
                LogError(device, "VUID-VkImageCreateInfo-multisampleArrayImage-04460",
                         "vkCreateImage (portability error): Cannot create an image with samples/texel > 1 && arrayLayers != 1");
        }
    }

    const auto external_memory_create_info_nv = LvlFindInChain<VkExternalMemoryImageCreateInfoNV>(pCreateInfo->pNext);
    const auto external_memory_create_info = LvlFindInChain<VkExternalMemoryImageCreateInfo>(pCreateInfo->pNext);
    if (external_memory_create_info_nv != nullptr && external_memory_create_info != nullptr) {
        skip |= LogError(device, "VUID-VkImageCreateInfo-pNext-00988",
                         "vkCreateImage(): VkImageCreateInfo struct has both VkExternalMemoryImageCreateInfoNV and "
                         "VkExternalMemoryImageCreateInfo chained structs.");
    }
    if (external_memory_create_info && external_memory_create_info->handleTypes != 0) {
        if (pCreateInfo->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            skip |= LogError(
                device, "VUID-VkImageCreateInfo-pNext-01443",
                "vkCreateImage: VkImageCreateInfo pNext chain includes VkExternalMemoryImageCreateInfo with handleTypes %" PRIu32
                " but pCreateInfo->initialLayout is %s.",
                external_memory_create_info->handleTypes, string_VkImageLayout(pCreateInfo->initialLayout));
        }
        // Check external memory handle types compatibility
        const uint32_t any_type = 1u << MostSignificantBit(external_memory_create_info->handleTypes);
        auto external_image_info = LvlInitStruct<VkPhysicalDeviceExternalImageFormatInfo>();
        external_image_info.handleType = static_cast<VkExternalMemoryHandleTypeFlagBits>(any_type);
        auto image_info = LvlInitStruct<VkPhysicalDeviceImageFormatInfo2>(&external_image_info);
        image_info.format = pCreateInfo->format;
        image_info.type = pCreateInfo->imageType;
        image_info.tiling = pCreateInfo->tiling;
        image_info.usage = pCreateInfo->usage;
        image_info.flags = pCreateInfo->flags;

        auto external_image_properties = LvlInitStruct<VkExternalImageFormatProperties>();
        auto image_properties = LvlInitStruct<VkImageFormatProperties2>(&external_image_properties);
        result = DispatchGetPhysicalDeviceImageFormatProperties2(physical_device, &image_info, &image_properties);
        const auto compatible_types = external_image_properties.externalMemoryProperties.compatibleHandleTypes;

        if (result != VK_SUCCESS) {
            skip |= LogError(
                device, "VUID-VkImageCreateInfo-pNext-00990",
                "vkCreateImage(): The handle type (%s), format (%s), type (%s), tiling (%s), usage (%s), flags (%s) "
                "is not supported combination of parameters and vkGetPhysicalDeviceImageFormatProperties2 returned back %s.",
                string_VkExternalMemoryHandleTypeFlagBits(external_image_info.handleType), string_VkFormat(image_info.format),
                string_VkImageType(image_info.type), string_VkImageTiling(image_info.tiling),
                string_VkImageUsageFlags(image_info.usage).c_str(), string_VkImageCreateFlags(image_info.flags).c_str(),
                string_VkResult(result));
        } else if ((external_memory_create_info->handleTypes & compatible_types) != external_memory_create_info->handleTypes) {
            const bool single_flag = GetBitSetCount(external_memory_create_info->handleTypes) == 1;
            skip |= LogError(device, "VUID-VkImageCreateInfo-pNext-00990",
                             "vkCreateImage(): VkImageCreateInfo pNext chain contains VkExternalMemoryImageCreateInfo with "
                             "%s (%s).",
                             single_flag ? "unsupported flag" : "incompatible flags",
                             string_VkExternalMemoryHandleTypeFlags(external_memory_create_info->handleTypes).c_str());
        }
    } else if (external_memory_create_info_nv && external_memory_create_info_nv->handleTypes != 0) {
        if (pCreateInfo->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            skip |= LogError(
                device, "VUID-VkImageCreateInfo-pNext-01443",
                "vkCreateImage: VkImageCreateInfo pNext chain includes VkExternalMemoryImageCreateInfoNV with handleTypes %" PRIu32
                " but pCreateInfo->initialLayout is %s.",
                external_memory_create_info_nv->handleTypes, string_VkImageLayout(pCreateInfo->initialLayout));
        }
        // Check external memory handle types compatibility
        const uint32_t any_type = 1u << MostSignificantBit(external_memory_create_info_nv->handleTypes);
        auto handle_type = static_cast<VkExternalMemoryHandleTypeFlagBitsNV>(any_type);
        VkExternalImageFormatPropertiesNV external_image_properties = {};
        result = DispatchGetPhysicalDeviceExternalImageFormatPropertiesNV(
            physical_device, pCreateInfo->format, pCreateInfo->imageType, pCreateInfo->tiling, pCreateInfo->usage,
            pCreateInfo->flags, handle_type, &external_image_properties);
        const auto compatible_types = external_image_properties.compatibleHandleTypes;

        if (result != VK_SUCCESS) {
            skip |= LogError(device, "VUID-VkImageCreateInfo-pNext-00991",
                             "vkCreateImage(): The handle type (%s), format (%s), type (%s), tiling (%s), usage (%s), flags (%s) "
                             "is not supported combination of parameters and vkGetPhysicalDeviceExternalImageFormatPropertiesNV "
                             "returned back %s.",
                             string_VkExternalMemoryHandleTypeFlagBitsNV(handle_type), string_VkFormat(pCreateInfo->format),
                             string_VkImageType(pCreateInfo->imageType), string_VkImageTiling(pCreateInfo->tiling),
                             string_VkImageUsageFlags(pCreateInfo->usage).c_str(),
                             string_VkImageCreateFlags(pCreateInfo->flags).c_str(), string_VkResult(result));
        } else if ((external_memory_create_info_nv->handleTypes & compatible_types) !=
                   external_memory_create_info_nv->handleTypes) {
            const bool single_flag = GetBitSetCount(external_memory_create_info_nv->handleTypes) == 1;
            skip |= LogError(device, "VUID-VkImageCreateInfo-pNext-00991",
                             "vkCreateImage(): VkImageCreateInfo pNext chain contains VkExternalMemoryImageCreateInfoNV with "
                             "%s (%s).",
                             single_flag ? "unsupported flag" : "incompatible flags",
                             string_VkExternalMemoryHandleTypeFlagsNV(external_memory_create_info_nv->handleTypes).c_str());
        }
    }

    if (device_group_create_info.physicalDeviceCount == 1) {
        if (pCreateInfo->flags & VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT) {
            skip |= LogError(
                device, "VUID-VkImageCreateInfo-physicalDeviceCount-01421",
                "vkCreateImage: Device was created with VkDeviceGroupDeviceCreateInfo::physicalDeviceCount equal to 1, but "
                "flags contain VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT bit. Device creation with "
                "VkDeviceGroupDeviceCreateInfo::physicalDeviceCount equal to 1 may have been implicit.");
        }
    }

    if ((pCreateInfo->flags & VK_IMAGE_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT) &&
        !enabled_features.descriptor_buffer_features.descriptorBufferCaptureReplay) {
        skip |=
            LogError(device, "VUID-VkImageCreateInfo-flags-08104",
                     "vkCreateImage(): the descriptorBufferCaptureReplay device feature is disabled: Images cannot be created with "
                     "the VK_IMAGE_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT.");
    }

    auto opaque_capture_descriptor_buffer = LvlFindInChain<VkOpaqueCaptureDescriptorDataCreateInfoEXT>(pCreateInfo->pNext);
    if (opaque_capture_descriptor_buffer && !(pCreateInfo->flags & VK_IMAGE_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT)) {
        skip |= LogError(device, "VUID-VkImageCreateInfo-pNext-08105",
                         "vkCreateImage(): VkOpaqueCaptureDescriptorDataCreateInfoEXT is in pNext chain, but "
                         "VK_IMAGE_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT is not set.");
    }

    bool has_decode_usage =
        pCreateInfo->usage & (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR |
                              VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR);
    bool has_encode_usage =
        pCreateInfo->usage & (VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                              VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR);
    if (has_decode_usage || has_encode_usage) {
        const auto *video_profiles = LvlFindInChain<VkVideoProfileListInfoKHR>(pCreateInfo->pNext);
        skip |= ValidateVideoProfileListInfo(video_profiles, device, "vkCreateImage", has_decode_usage,
                                             "VUID-VkImageCreateInfo-usage-04815", has_encode_usage,
                                             "VUID-VkImageCreateInfo-usage-04816");

        if (video_profiles && video_profiles->profileCount > 0) {
            auto format_props_list = GetVideoFormatProperties(pCreateInfo->usage, video_profiles);

            bool supported_video_format = false;
            for (auto &format_props : format_props_list) {
                if (pCreateInfo->format == format_props.format &&
                    (pCreateInfo->flags & format_props.imageCreateFlags) == pCreateInfo->flags &&
                    pCreateInfo->imageType == format_props.imageType && pCreateInfo->tiling == format_props.imageTiling &&
                    (pCreateInfo->usage & format_props.imageUsageFlags) == pCreateInfo->usage) {
                    supported_video_format = true;
                }
            }

            if (!supported_video_format) {
                skip |=
                    LogError(device, "VUID-VkImageCreateInfo-pNext-06811",
                             "vkCreateImage: image creation parameters (flags: 0x%08x, format: %s, imageType: %s, "
                             "tiling: %s) are not supported by any of the supported video format properties for "
                             "the video profiles specified in the VkVideoProfileListInfoKHR structure included in "
                             "the pCreateInfo->pNext chain, as reported by "
                             "vkGetPhysicalDeviceVideoFormatPropertiesKHR for the same video profiles "
                             "and the image usage flags specified in pCreateInfo->usage (0x%08x)",
                             pCreateInfo->flags, string_VkFormat(pCreateInfo->format), string_VkImageType(pCreateInfo->imageType),
                             string_VkImageTiling(pCreateInfo->tiling), pCreateInfo->usage);
            }
        }
    }

    return skip;
}

void CoreChecks::PostCallRecordCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                                           const VkAllocationCallbacks *pAllocator, VkImage *pImage, VkResult result) {
    if (VK_SUCCESS != result) return;

    StateTracker::PostCallRecordCreateImage(device, pCreateInfo, pAllocator, pImage, result);
    if ((pCreateInfo->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) != 0) {
        // non-sparse images set up their layout maps when memory is bound
        auto image_state = Get<IMAGE_STATE>(*pImage);
        image_state->SetInitialLayoutMap();
    }
}

bool CoreChecks::PreCallValidateDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) const {
    auto image_state = Get<IMAGE_STATE>(image);
    bool skip = false;
    if (image_state) {
        if (image_state->IsSwapchainImage() && image_state->owned_by_swapchain) {
            skip |= LogError(device, "VUID-vkDestroyImage-image-04882",
                             "vkDestroyImage(): %s is a presentable image and it is controlled by the implementation and is "
                             "destroyed with vkDestroySwapchainKHR.",
                             report_data->FormatHandle(image_state->image()).c_str());
        }
        skip |= ValidateObjectNotInUse(image_state.get(), "vkDestroyImage", "VUID-vkDestroyImage-image-01000");
    }
    return skip;
}

void CoreChecks::PreCallRecordDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {
    // Clean up validation specific data
    auto image_state = Get<IMAGE_STATE>(image);
    qfo_release_image_barrier_map.erase(image);
    // Clean up generic image state
    StateTracker::PreCallRecordDestroyImage(device, image, pAllocator);
}

bool CoreChecks::ValidateImageAttributes(const IMAGE_STATE *image_state, const VkImageSubresourceRange &range,
                                         const char *param_name) const {
    bool skip = false;
    const VkImage image = image_state->image();
    const VkFormat format = image_state->createInfo.format;

    if (range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
        skip |= LogError(image, "VUID-vkCmdClearColorImage-aspectMask-02498",
                         "vkCmdClearColorImage(): %s.aspectMasks must only be set to VK_IMAGE_ASPECT_COLOR_BIT.", param_name);
    }

    if (FormatIsDepthOrStencil(format)) {
        skip |= LogError(image, "VUID-vkCmdClearColorImage-image-00007",
                         "vkCmdClearColorImage(): %s called with image %s which has a depth/stencil format (%s).", param_name,
                         report_data->FormatHandle(image).c_str(), string_VkFormat(format));
    } else if (FormatIsCompressed(format)) {
        skip |= LogError(image, "VUID-vkCmdClearColorImage-image-00007",
                         "vkCmdClearColorImage(): %s called with image %s which has a compressed format (%s).", param_name,
                         report_data->FormatHandle(image).c_str(), string_VkFormat(format));
    }

    if (!(image_state->createInfo.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        skip |=
            LogError(image, "VUID-vkCmdClearColorImage-image-00002",
                     "vkCmdClearColorImage() %s called with image %s which was created without VK_IMAGE_USAGE_TRANSFER_DST_BIT.",
                     param_name, report_data->FormatHandle(image).c_str());
    }
    return skip;
}

bool CoreChecks::VerifyClearImageLayout(const CMD_BUFFER_STATE &cb_state, const IMAGE_STATE *image_state,
                                        const VkImageSubresourceRange &range, VkImageLayout dest_image_layout,
                                        const char *func_name) const {
    bool skip = false;
    if (strcmp(func_name, "vkCmdClearDepthStencilImage()") == 0) {
        if ((dest_image_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) && (dest_image_layout != VK_IMAGE_LAYOUT_GENERAL)) {
            skip |= LogError(image_state->image(), "VUID-vkCmdClearDepthStencilImage-imageLayout-00012",
                             "%s: Layout for cleared image is %s but can only be TRANSFER_DST_OPTIMAL or GENERAL.", func_name,
                             string_VkImageLayout(dest_image_layout));
        }

    } else {
        assert(strcmp(func_name, "vkCmdClearColorImage()") == 0);
        if (!IsExtEnabled(device_extensions.vk_khr_shared_presentable_image)) {
            if ((dest_image_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) && (dest_image_layout != VK_IMAGE_LAYOUT_GENERAL)) {
                skip |= LogError(image_state->image(), "VUID-vkCmdClearColorImage-imageLayout-00005",
                                 "%s: Layout for cleared image is %s but can only be TRANSFER_DST_OPTIMAL or GENERAL.", func_name,
                                 string_VkImageLayout(dest_image_layout));
            }
        } else {
            if ((dest_image_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) && (dest_image_layout != VK_IMAGE_LAYOUT_GENERAL) &&
                (dest_image_layout != VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR)) {
                skip |= LogError(
                    image_state->image(), "VUID-vkCmdClearColorImage-imageLayout-01394",
                    "%s: Layout for cleared image is %s but can only be TRANSFER_DST_OPTIMAL, SHARED_PRESENT_KHR, or GENERAL.",
                    func_name, string_VkImageLayout(dest_image_layout));
            }
        }
    }

    // Cast to const to prevent creation at validate time.
    const auto *subresource_map = cb_state.GetImageSubresourceLayoutMap(*image_state);
    if (subresource_map) {
        LayoutUseCheckAndMessage layout_check(dest_image_layout);
        auto normalized_isr = image_state->NormalizeSubresourceRange(range);
        // IncrementInterval skips over all the subresources that have the same state as we just checked, incrementing to
        // the next "constant value" range
        skip |= subresource_map->AnyInRange(
            normalized_isr, [this, &cb_state, &layout_check, func_name](const LayoutRange &range, const LayoutEntry &state) {
                bool subres_skip = false;
                if (!layout_check.Check(state)) {
                    const char *error_code = "VUID-vkCmdClearColorImage-imageLayout-00004";
                    if (strcmp(func_name, "vkCmdClearDepthStencilImage()") == 0) {
                        error_code = "VUID-vkCmdClearDepthStencilImage-imageLayout-00011";
                    } else {
                        assert(strcmp(func_name, "vkCmdClearColorImage()") == 0);
                    }
                    subres_skip |= LogError(cb_state.commandBuffer(), error_code,
                                            "%s: Cannot clear an image whose layout is %s and doesn't match the %s layout %s.",
                                            func_name, string_VkImageLayout(layout_check.expected_layout), layout_check.message,
                                            string_VkImageLayout(layout_check.layout));
                }
                return subres_skip;
            });
    }

    return skip;
}

bool CoreChecks::PreCallValidateCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
                                                   const VkClearColorValue *pColor, uint32_t rangeCount,
                                                   const VkImageSubresourceRange *pRanges) const {
    bool skip = false;
    // TODO : Verify memory is in VK_IMAGE_STATE_CLEAR state
    auto cb_state_ptr = GetRead<CMD_BUFFER_STATE>(commandBuffer);
    auto image_state = Get<IMAGE_STATE>(image);
    if (cb_state_ptr && image_state) {
        const CMD_BUFFER_STATE &cb_state = *cb_state_ptr;
        skip |= ValidateMemoryIsBoundToImage(commandBuffer, *image_state, "vkCmdClearColorImage()",
                                             "VUID-vkCmdClearColorImage-image-00003");
        skip |= ValidateCmd(cb_state, CMD_CLEARCOLORIMAGE);
        if (IsExtEnabled(device_extensions.vk_khr_maintenance1)) {
            skip |= ValidateImageFormatFeatureFlags(commandBuffer, *image_state, VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT_KHR,
                                                    "vkCmdClearColorImage", "VUID-vkCmdClearColorImage-image-01993");
        }
        skip |= ValidateProtectedImage(cb_state, *image_state, "vkCmdClearColorImage()",
                                       "VUID-vkCmdClearColorImage-commandBuffer-01805");
        skip |= ValidateUnprotectedImage(cb_state, *image_state, "vkCmdClearColorImage()",
                                         "VUID-vkCmdClearColorImage-commandBuffer-01806");
        for (uint32_t i = 0; i < rangeCount; ++i) {
            std::string param_name = "pRanges[" + std::to_string(i) + "]";
            skip |= ValidateCmdClearColorSubresourceRange(image_state.get(), pRanges[i], param_name.c_str());
            skip |= ValidateImageAttributes(image_state.get(), pRanges[i], param_name.c_str());
            skip |= VerifyClearImageLayout(cb_state, image_state.get(), pRanges[i], imageLayout, "vkCmdClearColorImage()");
        }
        // Tests for "Formats requiring sampler Y’CBCR conversion for VK_IMAGE_ASPECT_COLOR_BIT image views"
        if (FormatRequiresYcbcrConversionExplicitly(image_state->createInfo.format)) {
            skip |= LogError(device, "VUID-vkCmdClearColorImage-image-01545",
                             "vkCmdClearColorImage(): format (%s) must not be one of the formats requiring sampler YCBCR "
                             "conversion for VK_IMAGE_ASPECT_COLOR_BIT image views",
                             string_VkFormat(image_state->createInfo.format));
        }
    }
    return skip;
}

void CoreChecks::PreCallRecordCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
                                                 const VkClearColorValue *pColor, uint32_t rangeCount,
                                                 const VkImageSubresourceRange *pRanges) {
    StateTracker::PreCallRecordCmdClearColorImage(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);

    auto cb_state_ptr = GetWrite<CMD_BUFFER_STATE>(commandBuffer);
    auto image_state = Get<IMAGE_STATE>(image);
    if (cb_state_ptr && image_state) {
        for (uint32_t i = 0; i < rangeCount; ++i) {
            cb_state_ptr->SetImageInitialLayout(image, pRanges[i], imageLayout);
        }
    }
}

bool CoreChecks::ValidateClearDepthStencilValue(VkCommandBuffer commandBuffer, VkClearDepthStencilValue clearValue,
                                                const char *apiName) const {
    bool skip = false;

    // The extension was not created with a feature bit whichs prevents displaying the 2 variations of the VUIDs
    if (!IsExtEnabled(device_extensions.vk_ext_depth_range_unrestricted)) {
        if (!(clearValue.depth >= 0.0) || !(clearValue.depth <= 1.0)) {
            // Also VUID-VkClearDepthStencilValue-depth-00022
            skip |= LogError(commandBuffer, "VUID-VkClearDepthStencilValue-depth-02506",
                             "%s: VK_EXT_depth_range_unrestricted extension is not enabled and VkClearDepthStencilValue::depth "
                             "(=%f) is not within the [0.0, 1.0] range.",
                             apiName, clearValue.depth);
        }
    }

    return skip;
}

bool CoreChecks::PreCallValidateCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
                                                          const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount,
                                                          const VkImageSubresourceRange *pRanges) const {
    bool skip = false;

    // TODO : Verify memory is in VK_IMAGE_STATE_CLEAR state
    auto cb_state_ptr = GetRead<CMD_BUFFER_STATE>(commandBuffer);
    auto image_state = Get<IMAGE_STATE>(image);
    if (cb_state_ptr && image_state) {
        const CMD_BUFFER_STATE &cb_state = *cb_state_ptr;
        const VkFormat image_format = image_state->createInfo.format;
        skip |= ValidateMemoryIsBoundToImage(commandBuffer, *image_state, "vkCmdClearDepthStencilImage()",
                                             "VUID-vkCmdClearDepthStencilImage-image-00010");
        skip |= ValidateCmd(cb_state, CMD_CLEARDEPTHSTENCILIMAGE);
        if (IsExtEnabled(device_extensions.vk_khr_maintenance1)) {
            skip |= ValidateImageFormatFeatureFlags(commandBuffer, *image_state, VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT_KHR,
                                                    "vkCmdClearDepthStencilImage", "VUID-vkCmdClearDepthStencilImage-image-01994");
        }
        skip |= ValidateClearDepthStencilValue(commandBuffer, *pDepthStencil, "vkCmdClearDepthStencilImage()");
        skip |= ValidateProtectedImage(cb_state, *image_state, "vkCmdClearDepthStencilImage()",
                                       "VUID-vkCmdClearDepthStencilImage-commandBuffer-01807");
        skip |= ValidateUnprotectedImage(cb_state, *image_state, "vkCmdClearDepthStencilImage()",
                                         "VUID-vkCmdClearDepthStencilImage-commandBuffer-01808");

        bool any_include_aspect_depth_bit = false;
        bool any_include_aspect_stencil_bit = false;

        for (uint32_t i = 0; i < rangeCount; ++i) {
            std::string param_name = "pRanges[" + std::to_string(i) + "]";
            skip |= ValidateCmdClearDepthSubresourceRange(image_state.get(), pRanges[i], param_name.c_str());
            skip |= VerifyClearImageLayout(cb_state, image_state.get(), pRanges[i], imageLayout, "vkCmdClearDepthStencilImage()");
            // Image aspect must be depth or stencil or both
            VkImageAspectFlags valid_aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            if (((pRanges[i].aspectMask & valid_aspects) == 0) || ((pRanges[i].aspectMask & ~valid_aspects) != 0)) {
                skip |= LogError(commandBuffer, "VUID-vkCmdClearDepthStencilImage-aspectMask-02824",
                                 "vkCmdClearDepthStencilImage(): pRanges[%" PRIu32
                                 "].aspectMask can only be VK_IMAGE_ASPECT_DEPTH_BIT "
                                 "and/or VK_IMAGE_ASPECT_STENCIL_BIT.",
                                 i);
            }
            if ((pRanges[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
                any_include_aspect_depth_bit = true;
                if (FormatHasDepth(image_format) == false) {
                    skip |= LogError(commandBuffer, "VUID-vkCmdClearDepthStencilImage-image-02826",
                                     "vkCmdClearDepthStencilImage(): pRanges[%" PRIu32
                                     "].aspectMask has a VK_IMAGE_ASPECT_DEPTH_BIT but %s "
                                     "doesn't have a depth component.",
                                     i, string_VkFormat(image_format));
                }
            }
            if ((pRanges[i].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
                any_include_aspect_stencil_bit = true;
                if (FormatHasStencil(image_format) == false) {
                    skip |= LogError(commandBuffer, "VUID-vkCmdClearDepthStencilImage-image-02825",
                                     "vkCmdClearDepthStencilImage(): pRanges[%" PRIu32
                                     "].aspectMask has a VK_IMAGE_ASPECT_STENCIL_BIT but "
                                     "%s doesn't have a stencil component.",
                                     i, string_VkFormat(image_format));
                }
            }
        }
        if (any_include_aspect_stencil_bit) {
            const auto image_stencil_struct = LvlFindInChain<VkImageStencilUsageCreateInfo>(image_state->createInfo.pNext);
            if (image_stencil_struct != nullptr) {
                if ((image_stencil_struct->stencilUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
                    skip |=
                        LogError(device, "VUID-vkCmdClearDepthStencilImage-pRanges-02658",
                                 "vkCmdClearDepthStencilImage(): an element of pRanges.aspect includes VK_IMAGE_ASPECT_STENCIL_BIT "
                                 "and image was created with separate stencil usage, VK_IMAGE_USAGE_TRANSFER_DST_BIT must be "
                                 "included in VkImageStencilUsageCreateInfo::stencilUsage used to create image");
                }
            } else {
                if ((image_state->createInfo.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
                    skip |= LogError(
                        device, "VUID-vkCmdClearDepthStencilImage-pRanges-02659",
                        "vkCmdClearDepthStencilImage(): an element of pRanges.aspect includes VK_IMAGE_ASPECT_STENCIL_BIT and "
                        "image was not created with separate stencil usage, VK_IMAGE_USAGE_TRANSFER_DST_BIT must be included "
                        "in VkImageCreateInfo::usage used to create image");
                }
            }
        }
        if (any_include_aspect_depth_bit && (image_state->createInfo.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
            skip |= LogError(device, "VUID-vkCmdClearDepthStencilImage-pRanges-02660",
                             "vkCmdClearDepthStencilImage(): an element of pRanges.aspect includes VK_IMAGE_ASPECT_DEPTH_BIT, "
                             "VK_IMAGE_USAGE_TRANSFER_DST_BIT must be included in VkImageCreateInfo::usage used to create image");
        }
        if (image_state && !FormatIsDepthOrStencil(image_format)) {
            skip |= LogError(image, "VUID-vkCmdClearDepthStencilImage-image-00014",
                             "vkCmdClearDepthStencilImage(): called with image %s which doesn't have a depth/stencil format (%s).",
                             report_data->FormatHandle(image).c_str(), string_VkFormat(image_format));
        }
        if (VK_IMAGE_USAGE_TRANSFER_DST_BIT != (VK_IMAGE_USAGE_TRANSFER_DST_BIT & image_state->createInfo.usage)) {
            skip |= LogError(image, "VUID-vkCmdClearDepthStencilImage-image-00009",
                             "vkCmdClearDepthStencilImage(): called with image %s which was not created with the "
                             "VK_IMAGE_USAGE_TRANSFER_DST_BIT set.",
                             report_data->FormatHandle(image).c_str());
        }
    }
    return skip;
}

void CoreChecks::PreCallRecordCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
                                                        const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount,
                                                        const VkImageSubresourceRange *pRanges) {
    StateTracker::PreCallRecordCmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);

    auto cb_state_ptr = GetWrite<CMD_BUFFER_STATE>(commandBuffer);
    auto image_state = Get<IMAGE_STATE>(image);
    if (cb_state_ptr && image_state) {
        for (uint32_t i = 0; i < rangeCount; ++i) {
            cb_state_ptr->SetImageInitialLayout(image, pRanges[i], imageLayout);
        }
    }
}

// Test if two VkExtent3D structs are equivalent
static inline bool IsExtentEqual(const VkExtent3D &extent, const VkExtent3D &other_extent) {
    bool result = true;
    if ((extent.width != other_extent.width) || (extent.height != other_extent.height) || (extent.depth != other_extent.depth)) {
        result = false;
    }
    return result;
}

// Test if the extent argument has all dimensions set to 0.
static inline bool IsExtentAllZeroes(const VkExtent3D &extent) {
    return ((extent.width == 0) && (extent.height == 0) && (extent.depth == 0));
}

// Returns the image transfer granularity for a specific image scaled by compressed block size if necessary.
VkExtent3D CoreChecks::GetScaledItg(const CMD_BUFFER_STATE &cb_state, const IMAGE_STATE *img) const {
    // Default to (0, 0, 0) granularity in case we can't find the real granularity for the physical device.
    VkExtent3D granularity = {0, 0, 0};
    const auto pool = cb_state.command_pool;
    if (pool) {
        granularity = physical_device_state->queue_family_properties[pool->queueFamilyIndex].minImageTransferGranularity;
        if (FormatIsBlockedImage(img->createInfo.format)) {
            auto block_size = FormatTexelBlockExtent(img->createInfo.format);
            granularity.width *= block_size.width;
            granularity.height *= block_size.height;
        }
    }
    return granularity;
}

// Test elements of a VkExtent3D structure against alignment constraints contained in another VkExtent3D structure
static inline bool IsExtentAligned(const VkExtent3D &extent, const VkExtent3D &granularity) {
    bool valid = true;
    if ((SafeModulo(extent.depth, granularity.depth) != 0) || (SafeModulo(extent.width, granularity.width) != 0) ||
        (SafeModulo(extent.height, granularity.height) != 0)) {
        valid = false;
    }
    return valid;
}

// Check elements of a VkOffset3D structure against a queue family's Image Transfer Granularity values
bool CoreChecks::CheckItgOffset(const LogObjectList &objlist, const VkOffset3D &offset, const VkExtent3D &granularity,
                                const uint32_t i, const char *function, const char *member, const char *vuid) const {
    bool skip = false;
    VkExtent3D offset_extent = {};
    offset_extent.width = static_cast<uint32_t>(abs(offset.x));
    offset_extent.height = static_cast<uint32_t>(abs(offset.y));
    offset_extent.depth = static_cast<uint32_t>(abs(offset.z));
    if (IsExtentAllZeroes(granularity)) {
        // If the queue family image transfer granularity is (0, 0, 0), then the offset must always be (0, 0, 0)
        if (IsExtentAllZeroes(offset_extent) == false) {
            skip |= LogError(objlist, vuid,
                             "%s: pRegion[%d].%s (x=%d, y=%d, z=%d) must be (x=0, y=0, z=0) when the command buffer's queue family "
                             "image transfer granularity is (w=0, h=0, d=0).",
                             function, i, member, offset.x, offset.y, offset.z);
        }
    } else {
        // If the queue family image transfer granularity is not (0, 0, 0), then the offset dimensions must always be even
        // integer multiples of the image transfer granularity.
        if (IsExtentAligned(offset_extent, granularity) == false) {
            skip |= LogError(objlist, vuid,
                             "%s: pRegion[%d].%s (x=%d, y=%d, z=%d) dimensions must be even integer multiples of this command "
                             "buffer's queue family image transfer granularity (w=%d, h=%d, d=%d).",
                             function, i, member, offset.x, offset.y, offset.z, granularity.width, granularity.height,
                             granularity.depth);
        }
    }
    return skip;
}

// Check elements of a VkExtent3D structure against a queue family's Image Transfer Granularity values
bool CoreChecks::CheckItgExtent(const LogObjectList &objlist, const VkExtent3D &extent, const VkOffset3D &offset,
                                const VkExtent3D &granularity, const VkExtent3D &subresource_extent, const VkImageType image_type,
                                const uint32_t i, const char *function, const char *member, const char *vuid) const {
    bool skip = false;
    if (IsExtentAllZeroes(granularity)) {
        // If the queue family image transfer granularity is (0, 0, 0), then the extent must always match the image
        // subresource extent.
        if (IsExtentEqual(extent, subresource_extent) == false) {
            skip |= LogError(objlist, vuid,
                             "%s: pRegion[%d].%s (w=%d, h=%d, d=%d) must match the image subresource extents (w=%d, h=%d, d=%d) "
                             "when the command buffer's queue family image transfer granularity is (w=0, h=0, d=0).",
                             function, i, member, extent.width, extent.height, extent.depth, subresource_extent.width,
                             subresource_extent.height, subresource_extent.depth);
        }
    } else {
        // If the queue family image transfer granularity is not (0, 0, 0), then the extent dimensions must always be even
        // integer multiples of the image transfer granularity or the offset + extent dimensions must always match the image
        // subresource extent dimensions.
        VkExtent3D offset_extent_sum = {};
        offset_extent_sum.width = static_cast<uint32_t>(abs(offset.x)) + extent.width;
        offset_extent_sum.height = static_cast<uint32_t>(abs(offset.y)) + extent.height;
        offset_extent_sum.depth = static_cast<uint32_t>(abs(offset.z)) + extent.depth;
        bool x_ok = true;
        bool y_ok = true;
        bool z_ok = true;
        switch (image_type) {
            case VK_IMAGE_TYPE_3D:
                z_ok =
                    ((0 == SafeModulo(extent.depth, granularity.depth)) || (subresource_extent.depth == offset_extent_sum.depth));
                [[fallthrough]];
            case VK_IMAGE_TYPE_2D:
                y_ok = ((0 == SafeModulo(extent.height, granularity.height)) ||
                        (subresource_extent.height == offset_extent_sum.height));
                [[fallthrough]];
            case VK_IMAGE_TYPE_1D:
                x_ok =
                    ((0 == SafeModulo(extent.width, granularity.width)) || (subresource_extent.width == offset_extent_sum.width));
                break;
            default:
                // Unrecognized or new IMAGE_TYPE enums will be caught in parameter_validation
                assert(false);
        }
        if (!(x_ok && y_ok && z_ok)) {
            skip |= LogError(objlist, vuid,
                             "%s: pRegion[%d].%s (w=%d, h=%d, d=%d) dimensions must be even integer multiples of this command "
                             "buffer's queue family image transfer granularity (w=%d, h=%d, d=%d) or offset (x=%d, y=%d, z=%d) + "
                             "extent (w=%d, h=%d, d=%d) must match the image subresource extents (w=%d, h=%d, d=%d).",
                             function, i, member, extent.width, extent.height, extent.depth, granularity.width, granularity.height,
                             granularity.depth, offset.x, offset.y, offset.z, extent.width, extent.height, extent.depth,
                             subresource_extent.width, subresource_extent.height, subresource_extent.depth);
        }
    }
    return skip;
}

bool CoreChecks::ValidateImageMipLevel(const CMD_BUFFER_STATE &cb_state, const IMAGE_STATE &img, uint32_t mip_level,
                                       const uint32_t i, const char *function, const char *member, const char *vuid) const {
    bool skip = false;
    if (mip_level >= img.createInfo.mipLevels) {
        LogObjectList objlist(cb_state.Handle(), img.Handle());
        skip |= LogError(objlist, vuid,
                         "In %s, pRegions[%" PRIu32 "].%s.mipLevel is %" PRIu32 ", but provided %s has %" PRIu32 " mip levels.",
                         function, i, member, mip_level, report_data->FormatHandle(img.image()).c_str(), img.createInfo.mipLevels);
    }
    return skip;
}

bool CoreChecks::ValidateImageArrayLayerRange(const CMD_BUFFER_STATE &cb_state, const IMAGE_STATE &img, const uint32_t base_layer,
                                              const uint32_t layer_count, const uint32_t i, const char *function,
                                              const char *member, const char *vuid) const {
    bool skip = false;
    if (base_layer >= img.createInfo.arrayLayers || layer_count > img.createInfo.arrayLayers ||
        (base_layer + layer_count) > img.createInfo.arrayLayers) {
        if (layer_count == VK_REMAINING_ARRAY_LAYERS) {
            LogObjectList objlist(cb_state.Handle(), img.Handle());
            skip |= LogError(objlist, vuid,
                             "In %s, pRegions[%" PRIu32
                             "].%s.layerCount is VK_REMAINING_ARRAY_LAYERS, "
                             "but this special value is not supported here.",
                             function, i, member);
        } else {
            LogObjectList objlist(cb_state.Handle(), img.Handle());
            skip |= LogError(objlist, vuid,
                             "In %s, pRegions[%" PRIu32 "].%s.baseArrayLayer is %" PRIu32
                             " and .layerCount is "
                             "%" PRIu32 ", but provided %s has %" PRIu32 " array layers.",
                             function, i, member, base_layer, layer_count, report_data->FormatHandle(img.image()).c_str(),
                             img.createInfo.arrayLayers);
        }
    }
    return skip;
}

// Returns true if sub_rect is entirely contained within rect
static inline bool ContainsRect(VkRect2D rect, VkRect2D sub_rect) {
    if ((sub_rect.offset.x < rect.offset.x) || (sub_rect.offset.x + sub_rect.extent.width > rect.offset.x + rect.extent.width) ||
        (sub_rect.offset.y < rect.offset.y) || (sub_rect.offset.y + sub_rect.extent.height > rect.offset.y + rect.extent.height)) {
        return false;
    }
    return true;
}

bool CoreChecks::ValidateClearAttachmentExtent(const CMD_BUFFER_STATE &cb_state, uint32_t attachment_index,
                                               const IMAGE_VIEW_STATE *image_view_state, const VkRect2D &render_area,
                                               uint32_t rect_count, const VkClearRect *clear_rects) const {
    bool skip = false;

    for (uint32_t j = 0; j < rect_count; j++) {
        if (!ContainsRect(render_area, clear_rects[j].rect)) {
            skip |= LogError(cb_state.Handle(), "VUID-vkCmdClearAttachments-pRects-00016",
                             "vkCmdClearAttachments(): The area defined by pRects[%d] is not contained in the area of "
                             "the current render pass instance.",
                             j);
        }

        if (image_view_state) {
            // The layers specified by a given element of pRects must be contained within every attachment that
            // pAttachments refers to
            const uint32_t attachment_layer_count = image_view_state->GetAttachmentLayerCount();
            if ((clear_rects[j].baseArrayLayer >= attachment_layer_count) ||
                (clear_rects[j].baseArrayLayer + clear_rects[j].layerCount > attachment_layer_count)) {
                skip |= LogError(cb_state.Handle(), "VUID-vkCmdClearAttachments-pRects-06937",
                                 "vkCmdClearAttachments(): The layers defined in pRects[%d] are not contained in the layers "
                                 "of pAttachment[%d].",
                                 j, attachment_index);
            }
        }
    }
    return skip;
}

bool CoreChecks::PreCallValidateCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount,
                                                    const VkClearAttachment *pAttachments, uint32_t rectCount,
                                                    const VkClearRect *pRects) const {
    bool skip = false;
    auto cb_state_ptr = GetRead<CMD_BUFFER_STATE>(commandBuffer);
    if (!cb_state_ptr) {
        return skip;
    }
    const CMD_BUFFER_STATE &cb_state = *cb_state_ptr;

    skip |= ValidateCmd(cb_state, CMD_CLEARATTACHMENTS);

    // Validate that attachments are in reference list of active subpass
    if (cb_state.activeRenderPass) {
        const auto &render_area = (cb_state.activeRenderPass->use_dynamic_rendering)
                                      ? cb_state.activeRenderPass->dynamic_rendering_begin_rendering_info.renderArea
                                      : cb_state.activeRenderPassBeginInfo.renderArea;

        for (uint32_t attachment_index = 0; attachment_index < attachmentCount; attachment_index++) {
            auto clear_desc = &pAttachments[attachment_index];

            const VkImageAspectFlags aspect_mask = clear_desc->aspectMask;

            bool is_valid_color_attachment_index = false;
            const IMAGE_VIEW_STATE *color_view_state = nullptr;
            uint32_t color_attachment_count = 0;

            bool has_valid_depth_attachment = false;
            const IMAGE_VIEW_STATE *depth_view_state = nullptr;

            bool has_valid_stencil_attachment = false;
            const IMAGE_VIEW_STATE *stencil_view_state = nullptr;

            uint32_t view_mask = 0;

            std::string renderpass_info;

            if (cb_state.activeRenderPass->UsesDynamicRendering()) {
                is_valid_color_attachment_index = cb_state.IsValidDynamicColorAttachmentImageIndex(clear_desc->colorAttachment);
                color_view_state = cb_state.GetActiveAttachmentImageViewState(
                    cb_state.GetDynamicColorAttachmentImageIndex(clear_desc->colorAttachment));
                color_attachment_count = cb_state.GetDynamicColorAttachmentCount();

                has_valid_depth_attachment = cb_state.HasValidDynamicDepthAttachment();
                depth_view_state = cb_state.GetActiveAttachmentImageViewState(cb_state.GetDynamicDepthAttachmentImageIndex());

                has_valid_stencil_attachment = cb_state.HasValidDynamicStencilAttachment();
                stencil_view_state = cb_state.GetActiveAttachmentImageViewState(cb_state.GetDynamicStencilAttachmentImageIndex());

                view_mask = cb_state.activeRenderPass->dynamic_rendering_begin_rendering_info.viewMask;
            } else {
                const auto *renderpass_create_info = cb_state.activeRenderPass->createInfo.ptr();
                const auto *subpass_desc = &renderpass_create_info->pSubpasses[cb_state.activeSubpass];
                const auto *framebuffer = cb_state.activeFramebuffer.get();

                is_valid_color_attachment_index = (clear_desc->colorAttachment == VK_ATTACHMENT_UNUSED);
                if (subpass_desc) {
                    is_valid_color_attachment_index |= clear_desc->colorAttachment < subpass_desc->colorAttachmentCount;

                    if (framebuffer && (clear_desc->colorAttachment != VK_ATTACHMENT_UNUSED) &&
                        (clear_desc->colorAttachment < subpass_desc->colorAttachmentCount)) {
                        if (subpass_desc->pColorAttachments[clear_desc->colorAttachment].attachment <
                            framebuffer->createInfo.attachmentCount) {
                            color_view_state = cb_state.GetActiveAttachmentImageViewState(
                                subpass_desc->pColorAttachments[clear_desc->colorAttachment].attachment);
                        }
                    }

                    color_attachment_count = subpass_desc->colorAttachmentCount;

                    has_valid_depth_attachment =
                        (subpass_desc->pDepthStencilAttachment != nullptr) &&
                        ((subpass_desc->pDepthStencilAttachment->attachment == VK_ATTACHMENT_UNUSED) ||
                         (subpass_desc->pDepthStencilAttachment->attachment < renderpass_create_info->attachmentCount));
                    has_valid_stencil_attachment = has_valid_depth_attachment;
                    if (subpass_desc->pDepthStencilAttachment) {
                        depth_view_state =
                            cb_state.GetActiveAttachmentImageViewState(subpass_desc->pDepthStencilAttachment->attachment);
                    }

                    stencil_view_state = depth_view_state;

                    view_mask = subpass_desc->viewMask;
                }

                renderpass_info = " for " + report_data->FormatHandle(cb_state.activeRenderPass->renderPass()) + " subpass " +
                                  std::to_string(cb_state.activeSubpass);
            }

            if (aspect_mask & VK_IMAGE_ASPECT_METADATA_BIT) {
                skip |= LogError(commandBuffer, "VUID-VkClearAttachment-aspectMask-00020",
                                 "vkCmdClearAttachments() pAttachments[%" PRIu32 "] mask contains VK_IMAGE_ASPECT_METADATA_BIT.",
                                 attachment_index);
            } else if (aspect_mask & (VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT | VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT |
                                      VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT | VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT)) {
                skip |= LogError(commandBuffer, "VUID-VkClearAttachment-aspectMask-02246",
                                 "vkCmdClearAttachments() pAttachments[%" PRIu32
                                 "] mask contains a VK_IMAGE_ASPECT_MEMORY_PLANE_*_BIT_EXT bit.",
                                 attachment_index);
            } else if (aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) {
                if (is_valid_color_attachment_index) {
                    if (color_view_state &&
                        !(color_view_state->safe_create_info.subresourceRange.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)) {
                        skip |= LogError(commandBuffer, "VUID-vkCmdClearAttachments-pAttachments-07270",
                                         "vkCmdClearAttachments() pAttachments[%" PRIu32 "].colorAttachment=%" PRIu32
                                         " is backed by an "
                                         "image view that does not have aspect VK_IMAGE_ASPECT_COLOR_BIT%s.",
                                         attachment_index, clear_desc->colorAttachment, renderpass_info.c_str());
                    }
                } else {
                    skip |= LogError(commandBuffer, "VUID-vkCmdClearAttachments-aspectMask-07271",
                                     "vkCmdClearAttachments() pAttachments[%" PRIu32 "].colorAttachment=%" PRIu32
                                     " out of range%s."
                                     " colorAttachmentCount=%" PRIu32 ".",
                                     attachment_index, clear_desc->colorAttachment,

                                     renderpass_info.c_str(), color_attachment_count);
                }

                if ((aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) || (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT)) {
                    skip |= LogError(commandBuffer, "VUID-VkClearAttachment-aspectMask-00019",
                                     "vkCmdClearAttachments() pAttachments[%" PRIu32
                                     "] aspectMask must set only VK_IMAGE_ASPECT_COLOR_BIT "
                                     "of a color attachment.",
                                     attachment_index);
                }

            } else {  // Must be depth and/or stencil
                if (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) {
                    if (has_valid_depth_attachment) {
                        if (depth_view_state &&
                            !(depth_view_state->safe_create_info.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                            skip |= LogError(commandBuffer, "VUID-vkCmdClearAttachments-pAttachments-07270",
                                             "vkCmdClearAttachments() pAttachments[%" PRIu32
                                             "] is backed by an "
                                             "image view that does not have aspect VK_IMAGE_ASPECT_DEPTH_BIT%s.",
                                             attachment_index, renderpass_info.c_str());
                        }
                    } else {
                        skip |= LogError(commandBuffer, "VUID-vkCmdClearAttachments-pAttachments-07270",
                                         "vkCmdClearAttachments() pAttachments[%" PRIu32
                                         "] does not refer to a valid depth attachment%s.",
                                         attachment_index, renderpass_info.c_str());
                    }
                    skip |= ValidateClearDepthStencilValue(commandBuffer, clear_desc->clearValue.depthStencil,
                                                           "vkCmdClearAttachments()");
                }

                if (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) {
                    if (has_valid_stencil_attachment) {
                        if (stencil_view_state &&
                            !(stencil_view_state->safe_create_info.subresourceRange.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)) {
                            skip |= LogError(commandBuffer, "VUID-vkCmdClearAttachments-pAttachments-07270",
                                             "vkCmdClearAttachments() pAttachments[%" PRIu32
                                             "] is backed by an "
                                             "image view that does not have aspect VK_IMAGE_ASPECT_STENCIL_BIT%s.",
                                             attachment_index, renderpass_info.c_str());
                        }
                    } else {
                        skip |= LogError(commandBuffer, "VUID-vkCmdClearAttachments-pAttachments-07270",
                                         "vkCmdClearAttachments() pAttachments[%" PRIu32
                                         "] does not refer to a valid stencil attachment%s.",
                                         attachment_index, renderpass_info.c_str());
                    }
                }
            }

            std::array<const IMAGE_VIEW_STATE *, 3> image_views = {nullptr, nullptr, nullptr};
            if (aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) {
                image_views[0] = color_view_state;
            }
            if (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) {
                image_views[1] = depth_view_state;
            }
            if (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) {
                image_views[2] = stencil_view_state;
            }
            if (image_views[1] == image_views[2]) {
                image_views[2] = nullptr;
            }

            if (cb_state.createInfo.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
                for (auto image_view : image_views) {
                    if (image_view) {
                        skip |=
                            ValidateClearAttachmentExtent(cb_state, attachment_index, image_view, render_area, rectCount, pRects);
                    }
                }
            }

            for (auto image_view : image_views) {
                if (image_view) {
                    skip |= ValidateProtectedImage(cb_state, *image_view->image_state, "vkCmdClearAttachments()",
                                                   "VUID-vkCmdClearAttachments-commandBuffer-02504");
                    skip |= ValidateUnprotectedImage(cb_state, *image_view->image_state, "vkCmdClearAttachments()",
                                                     "VUID-vkCmdClearAttachments-commandBuffer-02505");
                }
            }

            // With a non-zero view mask, multiview functionality is considered to be enabled
            if (view_mask > 0) {
                for (uint32_t i = 0; i < rectCount; ++i) {
                    if (pRects[i].baseArrayLayer != 0 || pRects[i].layerCount != 1) {
                        skip |= LogError(commandBuffer, "VUID-vkCmdClearAttachments-baseArrayLayer-00018",
                                         "vkCmdClearAttachments(): pRects[%" PRIu32 "] baseArrayLayer is %" PRIu32
                                         " and layerCount is %" PRIu32 ", but the render pass instance uses multiview.",
                                         i, pRects[i].baseArrayLayer, pRects[i].layerCount);
                    }
                }
            }
        }
    }
    return skip;
}

void CoreChecks::PreCallRecordCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount,
                                                  const VkClearAttachment *pAttachments, uint32_t rectCount,
                                                  const VkClearRect *pRects) {
    auto cb_state_ptr = GetWrite<CMD_BUFFER_STATE>(commandBuffer);
    if (!cb_state_ptr) {
        return;
    }
    const CMD_BUFFER_STATE &cb_state = *cb_state_ptr;
    if (cb_state.activeRenderPass && (cb_state.createInfo.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)) {
        std::shared_ptr<std::vector<VkClearRect>> clear_rect_copy;
        if (cb_state.activeRenderPass->use_dynamic_rendering_inherited) {
            for (uint32_t attachment_index = 0; attachment_index < attachmentCount; attachment_index++) {
                const auto clear_desc = &pAttachments[attachment_index];
                auto colorAttachmentCount = cb_state.activeRenderPass->inheritance_rendering_info.colorAttachmentCount;
                int image_index = -1;
                if ((clear_desc->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) && (clear_desc->colorAttachment < colorAttachmentCount)) {
                    image_index = cb_state.GetDynamicColorAttachmentImageIndex(clear_desc->colorAttachment);
                } else if (clear_desc->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT)) {
                    image_index = cb_state.GetDynamicDepthAttachmentImageIndex();
                } else if (clear_desc->aspectMask & (VK_IMAGE_ASPECT_STENCIL_BIT)) {
                    image_index = cb_state.GetDynamicStencilAttachmentImageIndex();
                }

                if (image_index != -1) {
                    if (!clear_rect_copy) {
                        // We need a copy of the clear rectangles that will persist until the last lambda executes
                        // but we want to create it as lazily as possible
                        clear_rect_copy.reset(new std::vector<VkClearRect>(pRects, pRects + rectCount));
                    }
                    // if a secondary level command buffer inherits the framebuffer from the primary command buffer
                    // (see VkCommandBufferInheritanceInfo), this validation must be deferred until queue submit time
                    auto val_fn = [this, attachment_index, image_index, rectCount, clear_rect_copy](
                                      const CMD_BUFFER_STATE &secondary, const CMD_BUFFER_STATE *prim_cb,
                                      const FRAMEBUFFER_STATE *fb) {
                        assert(rectCount == clear_rect_copy->size());
                        bool skip = false;
                        const IMAGE_VIEW_STATE *image_view_state = nullptr;
                        if (image_index != -1) {
                            image_view_state = (*prim_cb->active_attachments)[image_index];
                        }
                        skip = ValidateClearAttachmentExtent(
                            secondary, attachment_index, image_view_state,
                            prim_cb->activeRenderPass->dynamic_rendering_begin_rendering_info.renderArea, rectCount,
                            clear_rect_copy->data());
                        return skip;
                    };
                    cb_state_ptr->cmd_execute_commands_functions.emplace_back(val_fn);
                }
            }
        } else if (cb_state.activeRenderPass->use_dynamic_rendering == false) {
            const VkRenderPassCreateInfo2 *renderpass_create_info = cb_state.activeRenderPass->createInfo.ptr();
            const VkSubpassDescription2 *subpass_desc = &renderpass_create_info->pSubpasses[cb_state.activeSubpass];

            for (uint32_t attachment_index = 0; attachment_index < attachmentCount; attachment_index++) {
                const auto clear_desc = &pAttachments[attachment_index];
                uint32_t fb_attachment = VK_ATTACHMENT_UNUSED;
                if ((clear_desc->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) &&
                    (clear_desc->colorAttachment < subpass_desc->colorAttachmentCount)) {
                    fb_attachment = subpass_desc->pColorAttachments[clear_desc->colorAttachment].attachment;
                } else if ((clear_desc->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) &&
                           subpass_desc->pDepthStencilAttachment) {
                    fb_attachment = subpass_desc->pDepthStencilAttachment->attachment;
                }
                if (fb_attachment != VK_ATTACHMENT_UNUSED) {
                    if (!clear_rect_copy) {
                        // We need a copy of the clear rectangles that will persist until the last lambda executes
                        // but we want to create it as lazily as possible
                        clear_rect_copy.reset(new std::vector<VkClearRect>(pRects, pRects + rectCount));
                    }
                    // if a secondary level command buffer inherits the framebuffer from the primary command buffer
                    // (see VkCommandBufferInheritanceInfo), this validation must be deferred until queue submit time
                    auto val_fn = [this, attachment_index, fb_attachment, rectCount, clear_rect_copy](
                                      const CMD_BUFFER_STATE &secondary, const CMD_BUFFER_STATE *prim_cb,
                                      const FRAMEBUFFER_STATE *fb) {
                        assert(rectCount == clear_rect_copy->size());
                        const auto &render_area = prim_cb->activeRenderPassBeginInfo.renderArea;
                        bool skip = false;
                        const IMAGE_VIEW_STATE *image_view_state = nullptr;
                        if (fb && (fb_attachment != VK_ATTACHMENT_UNUSED) && (fb_attachment < fb->createInfo.attachmentCount)) {
                            image_view_state = prim_cb->GetActiveAttachmentImageViewState(fb_attachment);
                        }
                        skip = ValidateClearAttachmentExtent(secondary, attachment_index, image_view_state, render_area, rectCount,
                                                             clear_rect_copy->data());
                        return skip;
                    };
                    cb_state_ptr->cmd_execute_commands_functions.emplace_back(val_fn);
                }
            }
        }
    }
}

// Validate that an image's sampleCount matches the requirement for a specific API call
bool CoreChecks::ValidateImageSampleCount(VkCommandBuffer cb, const IMAGE_STATE &image_state, VkSampleCountFlagBits sample_count,
                                          const char *location, const std::string &msgCode) const {
    bool skip = false;
    if (image_state.createInfo.samples != sample_count) {
        LogObjectList objlist(cb, image_state.Handle());
        skip = LogError(objlist, msgCode, "%s for %s was created with a sample count of %s but must be %s.", location,
                        report_data->FormatHandle(image_state.Handle()).c_str(),
                        string_VkSampleCountFlagBits(image_state.createInfo.samples), string_VkSampleCountFlagBits(sample_count));
    }
    return skip;
}

static GlobalImageLayoutRangeMap *GetLayoutRangeMap(GlobalImageLayoutMap &map, const IMAGE_STATE &image_state) {
    // This approach allows for a single hash lookup or/create new
    auto &layout_map = map[&image_state];
    if (!layout_map) {
        layout_map.emplace(image_state.subresource_encoder.SubresourceCount());
    }
    return &(*layout_map);
}

// Helper to update the Global or Overlay layout map
struct GlobalLayoutUpdater {
    bool update(VkImageLayout &dst, const image_layout_map::ImageSubresourceLayoutMap::LayoutEntry &src) const {
        if (src.current_layout != image_layout_map::kInvalidLayout && dst != src.current_layout) {
            dst = src.current_layout;
            return true;
        }
        return false;
    }

    std::optional<VkImageLayout> insert(const image_layout_map::ImageSubresourceLayoutMap::LayoutEntry &src) const {
        std::optional<VkImageLayout> result;
        if (src.current_layout != image_layout_map::kInvalidLayout) {
            result.emplace(src.current_layout);
        }
        return result;
    }
};

// This validates that the initial layout specified in the command buffer for the IMAGE is the same as the global IMAGE layout
bool CoreChecks::ValidateCmdBufImageLayouts(const Location &loc, const CMD_BUFFER_STATE &cb_state,
                                            GlobalImageLayoutMap &overlayLayoutMap) const {
    if (disabled[image_layout_validation]) return false;
    bool skip = false;
    // Iterate over the layout maps for each referenced image
    GlobalImageLayoutRangeMap empty_map(1);
    for (const auto &layout_map_entry : cb_state.image_layout_map) {
        const auto *image_state = layout_map_entry.first;
        const auto &subres_map = layout_map_entry.second;
        const auto &layout_map = subres_map->GetLayoutMap();
        // Validate the initial_uses for each subresource referenced
        if (layout_map.empty()) continue;

        auto *overlay_map = GetLayoutRangeMap(overlayLayoutMap, *image_state);
        const auto *global_map = image_state->layout_range_map.get();
        assert(global_map);
        auto global_map_guard = global_map->ReadLock();

        // Note: don't know if it would matter
        // if (global_map->empty() && overlay_map->empty()) // skip this next loop...;

        auto pos = layout_map.begin();
        const auto end = layout_map.end();
        sparse_container::parallel_iterator<const GlobalImageLayoutRangeMap> current_layout(*overlay_map, *global_map,
                                                                                            pos->first.begin);
        while (pos != end) {
            VkImageLayout initial_layout = pos->second.initial_layout;
            assert(initial_layout != image_layout_map::kInvalidLayout);
            if (initial_layout == image_layout_map::kInvalidLayout) {
                continue;
            }

            VkImageLayout image_layout = kInvalidLayout;

            if (current_layout->range.empty()) break;  // When we are past the end of data in overlay and global... stop looking
            if (current_layout->pos_A->valid) {        // pos_A denotes the overlay map in the parallel iterator
                image_layout = current_layout->pos_A->lower_bound->second;
            } else if (current_layout->pos_B->valid) {  // pos_B denotes the global map in the parallel iterator
                image_layout = current_layout->pos_B->lower_bound->second;
            }
            const auto intersected_range = pos->first & current_layout->range;
            if (initial_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                // TODO: Set memory invalid which is in mem_tracker currently
            } else if (image_layout != initial_layout) {
                const auto aspect_mask = image_state->subresource_encoder.Decode(intersected_range.begin).aspectMask;
                const bool matches = ImageLayoutMatches(aspect_mask, image_layout, initial_layout);
                if (!matches) {
                    // We can report all the errors for the intersected range directly
                    for (auto index : sparse_container::range_view<decltype(intersected_range)>(intersected_range)) {
                        const auto subresource = image_state->subresource_encoder.Decode(index);
                        skip |= LogError(cb_state.commandBuffer(), kVUID_Core_DrawState_InvalidImageLayout,
                                         "%s command buffer %s expects %s (subresource: aspectMask 0x%X array layer %" PRIu32
                                         ", mip level %" PRIu32
                                         ") "
                                         "to be in layout %s--instead, current layout is %s.",
                                         loc.Message().c_str(), report_data->FormatHandle(cb_state.commandBuffer()).c_str(),
                                         report_data->FormatHandle(image_state->Handle()).c_str(), subresource.aspectMask,
                                         subresource.arrayLayer, subresource.mipLevel, string_VkImageLayout(initial_layout),
                                         string_VkImageLayout(image_layout));
                    }
                }
            }
            if (pos->first.includes(intersected_range.end)) {
                current_layout.seek(intersected_range.end);
            } else {
                ++pos;
                if (pos != end) {
                    current_layout.seek(pos->first.begin);
                }
            }
        }
        // Update all layout set operations (which will be a subset of the initial_layouts)
        sparse_container::splice(*overlay_map, subres_map->GetLayoutMap(), GlobalLayoutUpdater());
    }

    return skip;
}

void CoreChecks::UpdateCmdBufImageLayouts(const CMD_BUFFER_STATE *cb_state) {
    for (const auto &layout_map_entry : cb_state->image_layout_map) {
        const auto *image_state = layout_map_entry.first;
        const auto &subres_map = layout_map_entry.second;
        auto guard = image_state->layout_range_map->WriteLock();
        sparse_container::splice(*image_state->layout_range_map, subres_map->GetLayoutMap(), GlobalLayoutUpdater());
    }
}

// ValidateLayoutVsAttachmentDescription is a general function where we can validate various state associated with the
// VkAttachmentDescription structs that are used by the sub-passes of a renderpass. Initial check is to make sure that READ_ONLY
// layout attachments don't have CLEAR as their loadOp.
bool CoreChecks::ValidateLayoutVsAttachmentDescription(const debug_report_data *report_data, RenderPassCreateVersion rp_version,
                                                       const VkImageLayout first_layout, const uint32_t attachment,
                                                       const VkAttachmentDescription2 &attachment_description) const {
    bool skip = false;
    const bool use_rp2 = (rp_version == RENDER_PASS_VERSION_2);

    // Verify that initial loadOp on READ_ONLY attachments is not CLEAR
    // for both loadOp and stencilLoaOp rp2 has it in 1 VU while rp1 has it in 2 VU with half behind Maintenance2 extension
    // Each is VUID is below in following order: rp2 -> rp1 with Maintenance2 -> rp1 with no extenstion
    if (attachment_description.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
        if (use_rp2 && ((first_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) ||
                        (first_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
                        (first_layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL))) {
            skip |= LogError(device, "VUID-VkRenderPassCreateInfo2-pAttachments-02522",
                             "vkCreateRenderPass2(): Cannot clear attachment %d with invalid first layout %s.", attachment,
                             string_VkImageLayout(first_layout));
        } else if ((use_rp2 == false) && IsExtEnabled(device_extensions.vk_khr_maintenance2) &&
                   (first_layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL)) {
            skip |= LogError(device, "VUID-VkRenderPassCreateInfo-pAttachments-01566",
                             "vkCreateRenderPass(): Cannot clear attachment %d with invalid first layout %s.", attachment,
                             string_VkImageLayout(first_layout));
        } else if ((use_rp2 == false) && ((first_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) ||
                                          (first_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))) {
            skip |= LogError(device, "VUID-VkRenderPassCreateInfo-pAttachments-00836",
                             "vkCreateRenderPass(): Cannot clear attachment %d with invalid first layout %s.", attachment,
                             string_VkImageLayout(first_layout));
        }
    }

    // Same as above for loadOp, but for stencilLoadOp
    if (attachment_description.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
        if (use_rp2 && ((first_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) ||
                        (first_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
                        (first_layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL))) {
            skip |= LogError(device, "VUID-VkRenderPassCreateInfo2-pAttachments-02523",
                             "vkCreateRenderPass2(): Cannot clear attachment %d with invalid first layout %s.", attachment,
                             string_VkImageLayout(first_layout));
        } else if ((use_rp2 == false) && IsExtEnabled(device_extensions.vk_khr_maintenance2) &&
                   (first_layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL)) {
            skip |= LogError(device, "VUID-VkRenderPassCreateInfo-pAttachments-01567",
                             "vkCreateRenderPass(): Cannot clear attachment %d with invalid first layout %s.", attachment,
                             string_VkImageLayout(first_layout));
        } else if ((use_rp2 == false) && ((first_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) ||
                                          (first_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))) {
            skip |= LogError(device, "VUID-VkRenderPassCreateInfo-pAttachments-02511",
                             "vkCreateRenderPass(): Cannot clear attachment %d with invalid first layout %s.", attachment,
                             string_VkImageLayout(first_layout));
        }
    }

    return skip;
}

// Helper function to validate correct usage bits set for buffers or images. Verify that (actual & desired) flags != 0 or, if strict
// is true, verify that (actual & desired) flags == desired
bool CoreChecks::ValidateUsageFlags(VkFlags actual, VkFlags desired, VkBool32 strict, const LogObjectList &objlist,
                                    const VulkanTypedHandle &typed_handle, const char *msgCode, char const *func_name,
                                    char const *usage_str) const {
    bool correct_usage = false;
    bool skip = false;
    const char *type_str = object_string[typed_handle.type];
    if (strict) {
        correct_usage = ((actual & desired) == desired);
    } else {
        correct_usage = ((actual & desired) != 0);
    }

    if (!correct_usage) {
        // All callers should have a valid VUID
        assert(msgCode != kVUIDUndefined);
        skip =
            LogError(objlist, msgCode, "Invalid usage flag for %s used by %s. In this case, %s should have %s set during creation.",
                     report_data->FormatHandle(typed_handle).c_str(), func_name, type_str, usage_str);
    }
    return skip;
}

// Helper function to validate usage flags for buffers. For given buffer_state send actual vs. desired usage off to helper above
// where an error will be flagged if usage is not correct
bool CoreChecks::ValidateImageUsageFlags(VkCommandBuffer cb, IMAGE_STATE const &image_state, VkImageUsageFlags desired, bool strict,
                                         const char *msgCode, char const *func_name) const {
    LogObjectList objlist(cb, image_state.Handle());
    return ValidateUsageFlags(image_state.createInfo.usage, desired, strict, objlist, image_state.Handle(), msgCode, func_name,
                              string_VkImageUsageFlags(desired).c_str());
}

bool CoreChecks::ValidateImageFormatFeatureFlags(VkCommandBuffer cb, IMAGE_STATE const &image_state,
                                                 VkFormatFeatureFlags2KHR desired, char const *func_name, const char *vuid) const {
    bool skip = false;
    const VkFormatFeatureFlags2KHR image_format_features = image_state.format_features;
    if ((image_format_features & desired) != desired) {
        const LogObjectList objlist(cb, image_state.Handle());
        // Same error, but more details if it was an AHB external format
        if (image_state.HasAHBFormat()) {
            skip |= LogError(objlist, vuid,
                             "In %s, VkFormatFeatureFlags (0x%" PRIxLEAST64
                             ") does not support required feature %s for the external format "
                             "found in VkAndroidHardwareBufferFormatPropertiesANDROID::formatFeatures used by %s.",
                             func_name, image_format_features, string_VkFormatFeatureFlags2KHR(desired).c_str(),
                             report_data->FormatHandle(image_state.image()).c_str());
        } else {
            skip |= LogError(
                objlist, vuid,
                "In %s, VkFormatFeatureFlags (0x%" PRIxLEAST64 ") does not support required feature %s for format %" PRIu32
                " used by %s "
                "with tiling %s.",
                func_name, image_format_features, string_VkFormatFeatureFlags2KHR(desired).c_str(), image_state.createInfo.format,
                report_data->FormatHandle(image_state.image()).c_str(), string_VkImageTiling(image_state.createInfo.tiling));
        }
    }
    return skip;
}

bool CoreChecks::ValidateImageSubresourceLayers(const CMD_BUFFER_STATE &cb_state,
                                                const VkImageSubresourceLayers *subresource_layers, char const *func_name,
                                                char const *member, uint32_t i) const {
    bool skip = false;
    const VkImageAspectFlags apsect_mask = subresource_layers->aspectMask;
    // layerCount must not be zero
    if (subresource_layers->layerCount == 0) {
        skip |= LogError(cb_state.commandBuffer(), "VUID-VkImageSubresourceLayers-layerCount-01700",
                         "In %s, pRegions[%" PRIu32 "].%s.layerCount must not be zero.", func_name, i, member);
    }
    // aspectMask must not contain VK_IMAGE_ASPECT_METADATA_BIT
    if (apsect_mask & VK_IMAGE_ASPECT_METADATA_BIT) {
        skip |= LogError(cb_state.commandBuffer(), "VUID-VkImageSubresourceLayers-aspectMask-00168",
                         "In %s, pRegions[%" PRIu32 "].%s.aspectMask has VK_IMAGE_ASPECT_METADATA_BIT set.", func_name, i, member);
    }
    // if aspectMask contains COLOR, it must not contain either DEPTH or STENCIL
    if ((apsect_mask & VK_IMAGE_ASPECT_COLOR_BIT) && (apsect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        skip |= LogError(cb_state.commandBuffer(), "VUID-VkImageSubresourceLayers-aspectMask-00167",
                         "In %s, pRegions[%" PRIu32
                         "].%s.aspectMask has VK_IMAGE_ASPECT_COLOR_BIT and either VK_IMAGE_ASPECT_DEPTH_BIT or "
                         "VK_IMAGE_ASPECT_STENCIL_BIT set.",
                         func_name, i, member);
    }
    // aspectMask must not contain VK_IMAGE_ASPECT_MEMORY_PLANE_i_BIT_EXT
    if (apsect_mask & (VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT | VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT |
                       VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT | VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT)) {
        skip |= LogError(cb_state.commandBuffer(), "VUID-VkImageSubresourceLayers-aspectMask-02247",
                         "In %s, pRegions[%" PRIu32 "].%s.aspectMask has a VK_IMAGE_ASPECT_MEMORY_PLANE_*_BIT_EXT bit set.",
                         func_name, i, member);
    }
    return skip;
}

// Helper function to validate usage flags for buffers. For given buffer_state send actual vs. desired usage off to helper above
// where an error will be flagged if usage is not correct
template <typename HandleT>
bool CoreChecks::ValidateBufferUsageFlags(HandleT handle, BUFFER_STATE const &buffer_state, VkFlags desired, bool strict,
                                          const char *msgCode, char const *func_name, char const *usage_string) const {
    LogObjectList objlist(handle, buffer_state.Handle());
    return ValidateUsageFlags(buffer_state.createInfo.usage, desired, strict, objlist, buffer_state.Handle(), msgCode, func_name,
                              usage_string);
}
// explictly instantiate templates needed by other .cpp files
template bool CoreChecks::ValidateBufferUsageFlags(VkCommandBuffer, BUFFER_STATE const &, unsigned int, bool, char const *,
                                                   char const *, char const *) const;
template bool CoreChecks::ValidateBufferUsageFlags(VkDevice, BUFFER_STATE const &, unsigned int, bool, char const *, char const *,
                                                   char const *) const;

bool CoreChecks::ValidateBufferViewRange(const BUFFER_STATE *buffer_state, const VkBufferViewCreateInfo *pCreateInfo,
                                         const VkPhysicalDeviceLimits *device_limits) const {
    bool skip = false;

    const VkDeviceSize &range = pCreateInfo->range;
    if (range != VK_WHOLE_SIZE) {
        // Range must be greater than 0
        if (range <= 0) {
            skip |= LogError(buffer_state->buffer(), "VUID-VkBufferViewCreateInfo-range-00928",
                             "vkCreateBufferView(): If VkBufferViewCreateInfo range (%" PRIuLEAST64
                             ") does not equal VK_WHOLE_SIZE, range must be greater than 0.",
                             range);
        }
        // Range must be a multiple of the element size of format
        const uint32_t format_size = FormatElementSize(pCreateInfo->format);
        if (SafeModulo(range, format_size) != 0) {
            skip |= LogError(buffer_state->buffer(), "VUID-VkBufferViewCreateInfo-range-00929",
                             "vkCreateBufferView(): If VkBufferViewCreateInfo range (%" PRIuLEAST64
                             ") does not equal VK_WHOLE_SIZE, range must be a multiple of the element size of the format "
                             "(%" PRIu32 ").",
                             range, format_size);
        }
        // Range divided by the element size of format must be less than or equal to VkPhysicalDeviceLimits::maxTexelBufferElements
        if (SafeDivision(range, format_size) > device_limits->maxTexelBufferElements) {
            skip |= LogError(buffer_state->buffer(), "VUID-VkBufferViewCreateInfo-range-00930",
                             "vkCreateBufferView(): If VkBufferViewCreateInfo range (%" PRIuLEAST64
                             ") does not equal VK_WHOLE_SIZE, range divided by the element size of the format (%" PRIu32
                             ") must be less than or equal to VkPhysicalDeviceLimits::maxTexelBufferElements (%" PRIuLEAST32 ").",
                             range, format_size, device_limits->maxTexelBufferElements);
        }
        // The sum of range and offset must be less than or equal to the size of buffer
        if (range + pCreateInfo->offset > buffer_state->createInfo.size) {
            skip |= LogError(buffer_state->buffer(), "VUID-VkBufferViewCreateInfo-offset-00931",
                             "vkCreateBufferView(): If VkBufferViewCreateInfo range (%" PRIuLEAST64
                             ") does not equal VK_WHOLE_SIZE, the sum of offset (%" PRIuLEAST64
                             ") and range must be less than or equal to the size of the buffer (%" PRIuLEAST64 ").",
                             range, pCreateInfo->offset, buffer_state->createInfo.size);
        }
    } else {
        const uint32_t format_size = FormatElementSize(pCreateInfo->format);

        // Size of buffer - offset, divided by the element size of format must be less than or equal to
        // VkPhysicalDeviceLimits::maxTexelBufferElements
        if (SafeDivision(buffer_state->createInfo.size - pCreateInfo->offset, format_size) >
            device_limits->maxTexelBufferElements) {
            skip |= LogError(buffer_state->buffer(), "VUID-VkBufferViewCreateInfo-range-04059",
                             "vkCreateBufferView(): If VkBufferViewCreateInfo range (%" PRIuLEAST64
                             ") equals VK_WHOLE_SIZE, the buffer's size (%" PRIuLEAST64 ") minus the offset (%" PRIuLEAST64
                             "), divided by the element size of the format (%" PRIu32
                             ") must be less than or equal to VkPhysicalDeviceLimits::maxTexelBufferElements (%" PRIuLEAST32 ").",
                             range, buffer_state->createInfo.size, pCreateInfo->offset, format_size,
                             device_limits->maxTexelBufferElements);
        }
    }
    return skip;
}

bool CoreChecks::ValidateBufferViewBuffer(const BUFFER_STATE *buffer_state, const VkBufferViewCreateInfo *pCreateInfo) const {
    bool skip = false;
    const VkFormatProperties3KHR format_properties = GetPDFormatProperties(pCreateInfo->format);
    if ((buffer_state->createInfo.usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT) &&
        !(format_properties.bufferFeatures & VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT_KHR)) {
        skip |= LogError(buffer_state->buffer(), "VUID-VkBufferViewCreateInfo-buffer-00933",
                         "vkCreateBufferView(): If buffer was created with `usage` containing "
                         "VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, format (%s) must "
                         "be supported for uniform texel buffers",
                         string_VkFormat(pCreateInfo->format));
    }
    if ((buffer_state->createInfo.usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT) &&
        !(format_properties.bufferFeatures & VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT_KHR)) {
        skip |= LogError(buffer_state->buffer(), "VUID-VkBufferViewCreateInfo-buffer-00934",
                         "vkCreateBufferView(): If buffer was created with `usage` containing "
                         "VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT, format (%s) must "
                         "be supported for storage texel buffers",
                         string_VkFormat(pCreateInfo->format));
    }
    return skip;
}

bool CoreChecks::PreCallValidateCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                                             const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) const {
    bool skip = false;

    auto chained_devaddr_struct = LvlFindInChain<VkBufferDeviceAddressCreateInfoEXT>(pCreateInfo->pNext);
    if (chained_devaddr_struct) {
        if (!(pCreateInfo->flags & VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT) &&
            chained_devaddr_struct->deviceAddress != 0) {
            skip |= LogError(device, "VUID-VkBufferCreateInfo-deviceAddress-02604",
                             "vkCreateBuffer(): Non-zero VkBufferDeviceAddressCreateInfoEXT::deviceAddress "
                             "requires VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT.");
        }
    }

    auto chained_opaqueaddr_struct = LvlFindInChain<VkBufferOpaqueCaptureAddressCreateInfo>(pCreateInfo->pNext);
    if (chained_opaqueaddr_struct) {
        if (!(pCreateInfo->flags & VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT) &&
            chained_opaqueaddr_struct->opaqueCaptureAddress != 0) {
            skip |= LogError(device, "VUID-VkBufferCreateInfo-opaqueCaptureAddress-03337",
                             "vkCreateBuffer(): Non-zero VkBufferOpaqueCaptureAddressCreateInfo::opaqueCaptureAddress"
                             "requires VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT.");
        }
    }

    auto dedicated_allocation_buffer = LvlFindInChain<VkDedicatedAllocationBufferCreateInfoNV>(pCreateInfo->pNext);
    if (dedicated_allocation_buffer && dedicated_allocation_buffer->dedicatedAllocation == VK_TRUE) {
        if (pCreateInfo->flags &
            (VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT)) {
            skip |= LogError(device, "VUID-VkBufferCreateInfo-pNext-01571",
                             "vkCreateBuffer(): pCreateInfos->flags must not include VK_BUFFER_CREATE_SPARSE_BINDING_BIT, "
                             "VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT, or VK_BUFFER_CREATE_SPARSE_ALIASED_BIT when "
                             "VkDedicatedAllocationBufferCreateInfoNV is in pNext chain with dedicatedAllocation VK_TRUE.");
        }
    }

    if ((pCreateInfo->flags & VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT) &&
        !enabled_features.core12.bufferDeviceAddressCaptureReplay &&
        !enabled_features.buffer_device_address_ext_features.bufferDeviceAddressCaptureReplay) {
        skip |= LogError(
            device, "VUID-VkBufferCreateInfo-flags-03338",
            "vkCreateBuffer(): the bufferDeviceAddressCaptureReplay device feature is disabled: Buffers cannot be created with "
            "the VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT set.");
    }

    if (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT && pCreateInfo->pQueueFamilyIndices) {
        const char *vuid = IsExtEnabled(device_extensions.vk_khr_get_physical_device_properties2)
                               ? "VUID-VkBufferCreateInfo-sharingMode-01419"
                               : "VUID-VkBufferCreateInfo-sharingMode-01391";
        skip |= ValidatePhysicalDeviceQueueFamilies(pCreateInfo->queueFamilyIndexCount, pCreateInfo->pQueueFamilyIndices,
                                                    "vkCreateBuffer", "pCreateInfo->pQueueFamilyIndices", vuid);
    }

    if ((pCreateInfo->flags & VK_BUFFER_CREATE_PROTECTED_BIT) != 0) {
        if (enabled_features.core11.protectedMemory == VK_FALSE) {
            skip |= LogError(device, "VUID-VkBufferCreateInfo-flags-01887",
                             "vkCreateBuffer(): the protectedMemory device feature is disabled: Buffers cannot be created with the "
                             "VK_BUFFER_CREATE_PROTECTED_BIT set.");
        }
        const VkBufferCreateFlags invalid_flags =
            VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT;
        if ((pCreateInfo->flags & invalid_flags) != 0) {
            skip |= LogError(device, "VUID-VkBufferCreateInfo-None-01888",
                             "vkCreateBuffer(): VK_BUFFER_CREATE_PROTECTED_BIT is set so no sparse create flags can be used at "
                             "same time (VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | "
                             "VK_BUFFER_CREATE_SPARSE_ALIASED_BIT).");
        }
    }

    bool has_decode_usage =
        pCreateInfo->usage & (VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR | VK_BUFFER_USAGE_VIDEO_DECODE_DST_BIT_KHR);
    bool has_encode_usage =
        pCreateInfo->usage & (VK_BUFFER_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR);
    if (has_decode_usage || has_encode_usage) {
        const auto *video_profiles = LvlFindInChain<VkVideoProfileListInfoKHR>(pCreateInfo->pNext);
        skip |= ValidateVideoProfileListInfo(video_profiles, device, "vkCreateBuffer", has_decode_usage,
                                             "VUID-VkBufferCreateInfo-usage-04813", has_encode_usage,
                                             "VUID-VkBufferCreateInfo-usage-04814");
    }

    if (pCreateInfo->usage & VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT) {
        if (pCreateInfo->size + samplerDescriptorBufferAddressSpaceSize >
            phys_dev_ext_props.descriptor_buffer_props.samplerDescriptorBufferAddressSpaceSize) {
            skip |= LogError(
                device, "VUID-VkBufferCreateInfo-usage-08097",
                "vkCreateBuffer(): Requested buffer size (%" PRIuLEAST64 ") plus current total (%" PRIuLEAST64
                ") is greater than specified in properties field samplerDescriptorBufferAddressSpaceSize (%" PRIuLEAST64 ").",
                pCreateInfo->size, samplerDescriptorBufferAddressSpaceSize.load(),
                phys_dev_ext_props.descriptor_buffer_props.samplerDescriptorBufferAddressSpaceSize);
        }

        if (pCreateInfo->size + descriptorBufferAddressSpaceSize >
            phys_dev_ext_props.descriptor_buffer_props.descriptorBufferAddressSpaceSize) {
            skip |= LogError(device, "VUID-VkBufferCreateInfo-usage-08097",
                             "vkCreateBuffer(): Requested buffer size (%" PRIuLEAST64 ") plus current total (%" PRIuLEAST64
                             ") is greater than specified in properties field descriptorBufferAddressSpaceSize (%" PRIuLEAST64 ")",
                             pCreateInfo->size, descriptorBufferAddressSpaceSize.load(),
                             phys_dev_ext_props.descriptor_buffer_props.descriptorBufferAddressSpaceSize);
        }
    }

    if (pCreateInfo->usage & VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT) {
        if (pCreateInfo->size + resourceDescriptorBufferAddressSpaceSize >
            phys_dev_ext_props.descriptor_buffer_props.resourceDescriptorBufferAddressSpaceSize) {
            skip |= LogError(
                device, "VUID-VkBufferCreateInfo-usage-08098",
                "vkCreateBuffer(): Requested buffer size (%" PRIuLEAST64 ") plus current total (%" PRIuLEAST64
                ") is greater than specified in properties field resourceDescriptorBufferAddressSpaceSize (%" PRIuLEAST64 ").",
                pCreateInfo->size, resourceDescriptorBufferAddressSpaceSize.load(),
                phys_dev_ext_props.descriptor_buffer_props.resourceDescriptorBufferAddressSpaceSize);
        }

        if (pCreateInfo->size + descriptorBufferAddressSpaceSize >
            phys_dev_ext_props.descriptor_buffer_props.descriptorBufferAddressSpaceSize) {
            skip |= LogError(device, "VUID-VkBufferCreateInfo-usage-08098",
                             "vkCreateBuffer(): Requested buffer size (%" PRIuLEAST64 ") plus current total (%" PRIuLEAST64
                             ") is greater than specified in properties field descriptorBufferAddressSpaceSize (%" PRIuLEAST64 ").",
                             pCreateInfo->size, descriptorBufferAddressSpaceSize.load(),
                             phys_dev_ext_props.descriptor_buffer_props.descriptorBufferAddressSpaceSize);
        }
    }

    if ((pCreateInfo->flags & VK_BUFFER_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT) &&
        !enabled_features.descriptor_buffer_features.descriptorBufferCaptureReplay) {
        skip |= LogError(
            device, "VUID-VkBufferCreateInfo-flags-08099",
            "vkCreateBuffer(): the descriptorBufferCaptureReplay device feature is disabled: Buffers cannot be created with "
            "the VK_BUFFER_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT.");
    }

    auto opaque_capture_descriptor_buffer = LvlFindInChain<VkOpaqueCaptureDescriptorDataCreateInfoEXT>(pCreateInfo->pNext);
    if (opaque_capture_descriptor_buffer && !(pCreateInfo->flags & VK_BUFFER_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT)) {
        skip |= LogError(device, "VUID-VkBufferCreateInfo-pNext-08100",
                         "vkCreateBuffer(): VkOpaqueCaptureDescriptorDataCreateInfoEXT is in pNext chain, but "
                         "VK_BUFFER_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT is not set.");
    }

    if (pCreateInfo->usage & VK_BUFFER_USAGE_PUSH_DESCRIPTORS_DESCRIPTOR_BUFFER_BIT_EXT) {
        if (!enabled_features.descriptor_buffer_features.descriptorBufferPushDescriptors) {
            skip |= LogError(
                device, "VUID-VkBufferCreateInfo-usage-08101",
                "vkCreateBuffer(): the descriptorBufferPushDescriptors device feature is disabled: Buffers cannot be created with "
                "VK_BUFFER_USAGE_PUSH_DESCRIPTORS_DESCRIPTOR_BUFFER_BIT_EXT set.");
        }

        if (phys_dev_ext_props.descriptor_buffer_props.bufferlessPushDescriptors) {
            skip |= LogError(
                device, "VUID-VkBufferCreateInfo-usage-08102",
                "vkCreateBuffer(): the bufferlessPushDescriptors device feature is VK_TRUE: Buffers cannot be created with "
                "VK_BUFFER_USAGE_PUSH_DESCRIPTORS_DESCRIPTOR_BUFFER_BIT_EXT set.");
        }

        if (!(pCreateInfo->usage &
              (VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT))) {
            skip |= LogError(
                device, "VUID-VkBufferCreateInfo-usage-08103",
                "vkCreateBuffer(): If VK_BUFFER_USAGE_PUSH_DESCRIPTORS_DESCRIPTOR_BUFFER_BIT_EXT is set, usage must also contain "
                "one of VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT or VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT.");
        }
    }

    auto external_memory_info = LvlFindInChain<VkExternalMemoryBufferCreateInfo>(pCreateInfo->pNext);
    if (external_memory_info && external_memory_info->handleTypes) {
        const uint32_t any_type = 1u << MostSignificantBit(external_memory_info->handleTypes);
        auto external_buffer_info = LvlInitStruct<VkPhysicalDeviceExternalBufferInfo>();
        external_buffer_info.flags = pCreateInfo->flags;
        external_buffer_info.usage = pCreateInfo->usage;
        external_buffer_info.handleType = static_cast<VkExternalMemoryHandleTypeFlagBits>(any_type);
        auto external_buffer_properties = LvlInitStruct<VkExternalBufferProperties>();
        DispatchGetPhysicalDeviceExternalBufferProperties(physical_device, &external_buffer_info, &external_buffer_properties);
        const auto compatible_types = external_buffer_properties.externalMemoryProperties.compatibleHandleTypes;

        if ((external_memory_info->handleTypes & compatible_types) != external_memory_info->handleTypes) {
            skip |= LogError(device, "VUID-VkBufferCreateInfo-pNext-00920",
                             "vkCreateBuffer(): VkBufferCreateInfo pNext chain contains VkExternalMemoryBufferCreateInfo with "
                             "unsupported or incompatible handleTypes flags (%s).",
                             string_VkExternalMemoryHandleTypeFlags(external_memory_info->handleTypes).c_str());
        }
    }

    return skip;
}

bool CoreChecks::PreCallValidateCreateBufferView(VkDevice device, const VkBufferViewCreateInfo *pCreateInfo,
                                                 const VkAllocationCallbacks *pAllocator, VkBufferView *pView) const {
    bool skip = false;
    auto buffer_state = Get<BUFFER_STATE>(pCreateInfo->buffer);

    if (FormatIsDepthOrStencil(pCreateInfo->format)) {
        // Should never hopefully get here, but there are known driver advertising the wrong feature flags
        // see https://gitlab.khronos.org/vulkan/vulkan/-/merge_requests/4849
        skip |= LogError(device, kVUID_Core_invalidDepthStencilFormat,
                         "vkCreateBufferView(): format is a depth/stencil format (%s) but depth/stencil formats do not have a "
                         "defined sizes for alignment, replace with a color format.",
                         string_VkFormat(pCreateInfo->format));
    }

    // If this isn't a sparse buffer, it needs to have memory backing it at CreateBufferView time
    if (buffer_state) {
        skip |= ValidateMemoryIsBoundToBuffer(device, *buffer_state, "vkCreateBufferView()",
                                              "VUID-VkBufferViewCreateInfo-buffer-00935");
        // In order to create a valid buffer view, the buffer must have been created with at least one of the following flags:
        // UNIFORM_TEXEL_BUFFER_BIT or STORAGE_TEXEL_BUFFER_BIT
        skip |= ValidateBufferUsageFlags(device, *buffer_state,
                                         VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT, false,
                                         "VUID-VkBufferViewCreateInfo-buffer-00932", "vkCreateBufferView()",
                                         "VK_BUFFER_USAGE_[STORAGE|UNIFORM]_TEXEL_BUFFER_BIT");

        // Buffer view offset must be less than the size of buffer
        if (pCreateInfo->offset >= buffer_state->createInfo.size) {
            skip |= LogError(buffer_state->buffer(), "VUID-VkBufferViewCreateInfo-offset-00925",
                             "vkCreateBufferView(): VkBufferViewCreateInfo offset (%" PRIuLEAST64
                             ") must be less than the size of the buffer (%" PRIuLEAST64 ").",
                             pCreateInfo->offset, buffer_state->createInfo.size);
        }

        const VkPhysicalDeviceLimits *device_limits = &phys_dev_props.limits;
        // Buffer view offset must be a multiple of VkPhysicalDeviceLimits::minTexelBufferOffsetAlignment
        if ((pCreateInfo->offset % device_limits->minTexelBufferOffsetAlignment) != 0 &&
            !enabled_features.texel_buffer_alignment_features.texelBufferAlignment) {
            const char *vuid = IsExtEnabled(device_extensions.vk_ext_texel_buffer_alignment)
                                   ? "VUID-VkBufferViewCreateInfo-offset-02749"
                                   : "VUID-VkBufferViewCreateInfo-offset-00926";
            skip |= LogError(buffer_state->buffer(), vuid,
                             "vkCreateBufferView(): VkBufferViewCreateInfo offset (%" PRIuLEAST64
                             ") must be a multiple of VkPhysicalDeviceLimits::minTexelBufferOffsetAlignment (%" PRIuLEAST64 ").",
                             pCreateInfo->offset, device_limits->minTexelBufferOffsetAlignment);
        }

        if (enabled_features.texel_buffer_alignment_features.texelBufferAlignment) {
            VkDeviceSize element_size = FormatElementSize(pCreateInfo->format);
            if ((element_size % 3) == 0) {
                element_size /= 3;
            }
            if (buffer_state->createInfo.usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT) {
                VkDeviceSize alignment_requirement =
                    phys_dev_ext_props.texel_buffer_alignment_props.storageTexelBufferOffsetAlignmentBytes;
                if (phys_dev_ext_props.texel_buffer_alignment_props.storageTexelBufferOffsetSingleTexelAlignment) {
                    alignment_requirement = std::min(alignment_requirement, element_size);
                }
                if (SafeModulo(pCreateInfo->offset, alignment_requirement) != 0) {
                    skip |= LogError(
                        buffer_state->buffer(), "VUID-VkBufferViewCreateInfo-buffer-02750",
                        "vkCreateBufferView(): If buffer was created with usage containing "
                        "VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT, "
                        "VkBufferViewCreateInfo offset (%" PRIuLEAST64
                        ") must be a multiple of the lesser of "
                        "VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT::storageTexelBufferOffsetAlignmentBytes (%" PRIuLEAST64
                        ") or, if VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT::storageTexelBufferOffsetSingleTexelAlignment "
                        "(%" PRId32
                        ") is VK_TRUE, the size of a texel of the requested format. "
                        "If the size of a texel is a multiple of three bytes, then the size of a "
                        "single component of format is used instead",
                        pCreateInfo->offset, phys_dev_ext_props.texel_buffer_alignment_props.storageTexelBufferOffsetAlignmentBytes,
                        phys_dev_ext_props.texel_buffer_alignment_props.storageTexelBufferOffsetSingleTexelAlignment);
                }
            }
            if (buffer_state->createInfo.usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT) {
                VkDeviceSize alignment_requirement =
                    phys_dev_ext_props.texel_buffer_alignment_props.uniformTexelBufferOffsetAlignmentBytes;
                if (phys_dev_ext_props.texel_buffer_alignment_props.uniformTexelBufferOffsetSingleTexelAlignment) {
                    alignment_requirement = std::min(alignment_requirement, element_size);
                }
                if (SafeModulo(pCreateInfo->offset, alignment_requirement) != 0) {
                    skip |= LogError(
                        buffer_state->buffer(), "VUID-VkBufferViewCreateInfo-buffer-02751",
                        "vkCreateBufferView(): If buffer was created with usage containing "
                        "VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, "
                        "VkBufferViewCreateInfo offset (%" PRIuLEAST64
                        ") must be a multiple of the lesser of "
                        "VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT::uniformTexelBufferOffsetAlignmentBytes (%" PRIuLEAST64
                        ") or, if VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT::uniformTexelBufferOffsetSingleTexelAlignment "
                        "(%" PRId32
                        ") is VK_TRUE, the size of a texel of the requested format. "
                        "If the size of a texel is a multiple of three bytes, then the size of a "
                        "single component of format is used instead",
                        pCreateInfo->offset, phys_dev_ext_props.texel_buffer_alignment_props.uniformTexelBufferOffsetAlignmentBytes,
                        phys_dev_ext_props.texel_buffer_alignment_props.uniformTexelBufferOffsetSingleTexelAlignment);
                }
            }
        }

        skip |= ValidateBufferViewRange(buffer_state.get(), pCreateInfo, device_limits);

        skip |= ValidateBufferViewBuffer(buffer_state.get(), pCreateInfo);
    }
    return skip;
}

// For the given format verify that the aspect masks make sense
bool CoreChecks::ValidateImageAspectMask(VkImage image, VkFormat format, VkImageAspectFlags aspect_mask, bool is_image_disjoint,
                                         const char *func_name, const char *vuid) const {
    bool skip = false;
    // checks color format and (single-plane or non-disjoint)
    // if ycbcr extension is not supported then single-plane and non-disjoint are always both true
    if ((FormatIsColor(format)) && ((FormatIsMultiplane(format) == false) || (is_image_disjoint == false))) {
        if ((aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) != VK_IMAGE_ASPECT_COLOR_BIT) {
            skip |= LogError(
                image, vuid,
                "%s: Using format (%s) with aspect flags (%s) but color image formats must have the VK_IMAGE_ASPECT_COLOR_BIT set.",
                func_name, string_VkFormat(format), string_VkImageAspectFlags(aspect_mask).c_str());
        } else if ((aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) != aspect_mask) {
            skip |= LogError(image, vuid,
                             "%s: Using format (%s) with aspect flags (%s) but color image formats must have ONLY the "
                             "VK_IMAGE_ASPECT_COLOR_BIT set.",
                             func_name, string_VkFormat(format), string_VkImageAspectFlags(aspect_mask).c_str());
        }
    } else if (FormatIsDepthAndStencil(format)) {
        if ((aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) == 0) {
            skip |= LogError(image, vuid,
                             "%s: Using format (%s) with aspect flags (%s) but depth/stencil image formats must have at least one "
                             "of VK_IMAGE_ASPECT_DEPTH_BIT and VK_IMAGE_ASPECT_STENCIL_BIT set.",
                             func_name, string_VkFormat(format), string_VkImageAspectFlags(aspect_mask).c_str());
        } else if ((aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != aspect_mask) {
            skip |= LogError(image, vuid,
                             "%s: Using format (%s) with aspect flags (%s) but combination depth/stencil image formats can have "
                             "only the VK_IMAGE_ASPECT_DEPTH_BIT and VK_IMAGE_ASPECT_STENCIL_BIT set.",
                             func_name, string_VkFormat(format), string_VkImageAspectFlags(aspect_mask).c_str());
        }
    } else if (FormatIsDepthOnly(format)) {
        if ((aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != VK_IMAGE_ASPECT_DEPTH_BIT) {
            skip |= LogError(image, vuid,
                             "%s: Using format (%s) with aspect flags (%s) but depth-only image formats must have the "
                             "VK_IMAGE_ASPECT_DEPTH_BIT set.",
                             func_name, string_VkFormat(format), string_VkImageAspectFlags(aspect_mask).c_str());
        } else if ((aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != aspect_mask) {
            skip |= LogError(image, vuid,
                             "%s: Using format (%s) with aspect flags (%s) but depth-only image formats can have only the "
                             "VK_IMAGE_ASPECT_DEPTH_BIT set.",
                             func_name, string_VkFormat(format), string_VkImageAspectFlags(aspect_mask).c_str());
        }
    } else if (FormatIsStencilOnly(format)) {
        if ((aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != VK_IMAGE_ASPECT_STENCIL_BIT) {
            skip |= LogError(image, vuid,
                             "%s: Using format (%s) with aspect flags (%s) but stencil-only image formats must have the "
                             "VK_IMAGE_ASPECT_STENCIL_BIT set.",
                             func_name, string_VkFormat(format), string_VkImageAspectFlags(aspect_mask).c_str());
        } else if ((aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != aspect_mask) {
            skip |= LogError(image, vuid,
                             "%s: Using format (%s) with aspect flags (%s) but stencil-only image formats can have only the "
                             "VK_IMAGE_ASPECT_STENCIL_BIT set.",
                             func_name, string_VkFormat(format), string_VkImageAspectFlags(aspect_mask).c_str());
        }
    } else if (FormatIsMultiplane(format)) {
        VkImageAspectFlags valid_flags = VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
        if (3 == FormatPlaneCount(format)) {
            valid_flags = valid_flags | VK_IMAGE_ASPECT_PLANE_2_BIT;
        }
        if ((aspect_mask & valid_flags) != aspect_mask) {
            skip |= LogError(image, vuid,
                             "%s: Using format (%s) with aspect flags (%s) but multi-plane image formats may have only "
                             "VK_IMAGE_ASPECT_COLOR_BIT or VK_IMAGE_ASPECT_PLANE_n_BITs set, where n = [0, 1, 2].",
                             func_name, string_VkFormat(format), string_VkImageAspectFlags(aspect_mask).c_str());
        }
    }
    return skip;
}

bool CoreChecks::ValidateImageAcquired(IMAGE_STATE const &image_state, const char *func_name) const {
    bool skip = false;

    return skip;
}

bool CoreChecks::ValidateImageSubresourceRange(const uint32_t image_mip_count, const uint32_t image_layer_count,
                                               const VkImageSubresourceRange &subresourceRange, const char *cmd_name,
                                               const char *param_name, const char *image_layer_count_var_name, const VkImage image,
                                               const SubresourceRangeErrorCodes &errorCodes) const {
    bool skip = false;

    // Validate mip levels
    if (subresourceRange.baseMipLevel >= image_mip_count) {
        skip |= LogError(image, errorCodes.base_mip_err,
                         "%s: %s.baseMipLevel (= %" PRIu32
                         ") is greater or equal to the mip level count of the image (i.e. greater or equal to %" PRIu32 ").",
                         cmd_name, param_name, subresourceRange.baseMipLevel, image_mip_count);
    }

    if (subresourceRange.levelCount != VK_REMAINING_MIP_LEVELS) {
        if (subresourceRange.levelCount == 0) {
            skip |=
                LogError(image, "VUID-VkImageSubresourceRange-levelCount-01720", "%s: %s.levelCount is 0.", cmd_name, param_name);
        } else {
            const uint64_t necessary_mip_count = uint64_t{subresourceRange.baseMipLevel} + uint64_t{subresourceRange.levelCount};

            if (necessary_mip_count > image_mip_count) {
                skip |= LogError(image, errorCodes.mip_count_err,
                                 "%s: %s.baseMipLevel + .levelCount (= %" PRIu32 " + %" PRIu32 " = %" PRIu64
                                 ") is greater than the mip level count of the image (i.e. greater than %" PRIu32 ").",
                                 cmd_name, param_name, subresourceRange.baseMipLevel, subresourceRange.levelCount,
                                 necessary_mip_count, image_mip_count);
            }
        }
    }

    // Validate array layers
    if (subresourceRange.baseArrayLayer >= image_layer_count) {
        skip |= LogError(image, errorCodes.base_layer_err,
                         "%s: %s.baseArrayLayer (= %" PRIu32
                         ") is greater or equal to the %s of the image when it was created (i.e. greater or equal to %" PRIu32 ").",
                         cmd_name, param_name, subresourceRange.baseArrayLayer, image_layer_count_var_name, image_layer_count);
    }

    if (subresourceRange.layerCount != VK_REMAINING_ARRAY_LAYERS) {
        if (subresourceRange.layerCount == 0) {
            skip |=
                LogError(image, "VUID-VkImageSubresourceRange-layerCount-01721", "%s: %s.layerCount is 0.", cmd_name, param_name);
        } else {
            const uint64_t necessary_layer_count =
                uint64_t{subresourceRange.baseArrayLayer} + uint64_t{subresourceRange.layerCount};

            if (necessary_layer_count > image_layer_count) {
                skip |= LogError(image, errorCodes.layer_count_err,
                                 "%s: %s.baseArrayLayer + .layerCount (= %" PRIu32 " + %" PRIu32 " = %" PRIu64
                                 ") is greater than the %s of the image when it was created (i.e. greater than %" PRIu32 ").",
                                 cmd_name, param_name, subresourceRange.baseArrayLayer, subresourceRange.layerCount,
                                 necessary_layer_count, image_layer_count_var_name, image_layer_count);
            }
        }
    }

    if (subresourceRange.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
        if (subresourceRange.aspectMask &
            (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) {
            skip |= LogError(image, "VUID-VkImageSubresourceRange-aspectMask-01670",
                             "%s: aspectMask includes both VK_IMAGE_ASPECT_COLOR_BIT and one of VK_IMAGE_ASPECT_PLANE_0_BIT, "
                             "VK_IMAGE_ASPECT_PLANE_1_BIT, or VK_IMAGE_ASPECT_PLANE_2_BIT.",
                             cmd_name);
        }
    }

    return skip;
}

bool CoreChecks::ValidateCreateImageViewSubresourceRange(const IMAGE_STATE *image_state, bool is_imageview_2d_type,
                                                         const VkImageSubresourceRange &subresourceRange) const {
    const bool is_khr_maintenance1 = IsExtEnabled(device_extensions.vk_khr_maintenance1);
    const bool is_2d_compatible =
        image_state->createInfo.flags & (VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT | VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT);
    const bool is_image_slicable = (image_state->createInfo.imageType == VK_IMAGE_TYPE_3D) && is_2d_compatible;
    const bool is_3_d_to_2_d_map = is_khr_maintenance1 && is_image_slicable && is_imageview_2d_type;

    uint32_t image_layer_count;

    if (is_3_d_to_2_d_map) {
        const auto layers = LayersFromRange(subresourceRange);
        const auto extent = image_state->GetSubresourceExtent(layers);
        image_layer_count = extent.depth;
    } else {
        image_layer_count = image_state->createInfo.arrayLayers;
    }

    const auto image_layer_count_var_name = is_3_d_to_2_d_map ? "extent.depth" : "arrayLayers";

    SubresourceRangeErrorCodes subresource_range_error_codes = {};
    subresource_range_error_codes.base_mip_err = "VUID-VkImageViewCreateInfo-subresourceRange-01478";
    subresource_range_error_codes.mip_count_err = "VUID-VkImageViewCreateInfo-subresourceRange-01718";
    subresource_range_error_codes.base_layer_err =
        is_khr_maintenance1 ? (is_3_d_to_2_d_map ? "VUID-VkImageViewCreateInfo-image-02724"
                                                 : (IsExtEnabled(device_extensions.vk_ext_image_2d_view_of_3d)
                                                        ? "VUID-VkImageViewCreateInfo-image-06724"
                                                        : "VUID-VkImageViewCreateInfo-image-01482"))
                            : "VUID-VkImageViewCreateInfo-subresourceRange-01480";
    subresource_range_error_codes.layer_count_err =
        is_khr_maintenance1 ? (is_3_d_to_2_d_map ? "VUID-VkImageViewCreateInfo-subresourceRange-02725"
                                                 : (IsExtEnabled(device_extensions.vk_ext_image_2d_view_of_3d)
                                                        ? "VUID-VkImageViewCreateInfo-subresourceRange-06725"
                                                        : "VUID-VkImageViewCreateInfo-subresourceRange-01483"))
                            : "VUID-VkImageViewCreateInfo-subresourceRange-01719";

    return ValidateImageSubresourceRange(image_state->createInfo.mipLevels, image_layer_count, subresourceRange,
                                         "vkCreateImageView", "pCreateInfo->subresourceRange", image_layer_count_var_name,
                                         image_state->image(), subresource_range_error_codes);
}

bool CoreChecks::ValidateCmdClearColorSubresourceRange(const IMAGE_STATE *image_state,
                                                       const VkImageSubresourceRange &subresourceRange,
                                                       const char *param_name) const {
    SubresourceRangeErrorCodes subresource_range_error_codes = {};
    subresource_range_error_codes.base_mip_err = "VUID-vkCmdClearColorImage-baseMipLevel-01470";
    subresource_range_error_codes.mip_count_err = "VUID-vkCmdClearColorImage-pRanges-01692";
    subresource_range_error_codes.base_layer_err = "VUID-vkCmdClearColorImage-baseArrayLayer-01472";
    subresource_range_error_codes.layer_count_err = "VUID-vkCmdClearColorImage-pRanges-01693";

    return ValidateImageSubresourceRange(image_state->createInfo.mipLevels, image_state->createInfo.arrayLayers, subresourceRange,
                                         "vkCmdClearColorImage", param_name, "arrayLayers", image_state->image(),
                                         subresource_range_error_codes);
}

bool CoreChecks::ValidateCmdClearDepthSubresourceRange(const IMAGE_STATE *image_state,
                                                       const VkImageSubresourceRange &subresourceRange,
                                                       const char *param_name) const {
    SubresourceRangeErrorCodes subresource_range_error_codes = {};
    subresource_range_error_codes.base_mip_err = "VUID-vkCmdClearDepthStencilImage-baseMipLevel-01474";
    subresource_range_error_codes.mip_count_err = "VUID-vkCmdClearDepthStencilImage-pRanges-01694";
    subresource_range_error_codes.base_layer_err = "VUID-vkCmdClearDepthStencilImage-baseArrayLayer-01476";
    subresource_range_error_codes.layer_count_err = "VUID-vkCmdClearDepthStencilImage-pRanges-01695";

    return ValidateImageSubresourceRange(image_state->createInfo.mipLevels, image_state->createInfo.arrayLayers, subresourceRange,
                                         "vkCmdClearDepthStencilImage", param_name, "arrayLayers", image_state->image(),
                                         subresource_range_error_codes);
}

bool CoreChecks::ValidateImageBarrierSubresourceRange(const Location &loc, const IMAGE_STATE *image_state,
                                                      const VkImageSubresourceRange &subresourceRange) const {
    return ValidateImageSubresourceRange(image_state->createInfo.mipLevels, image_state->createInfo.arrayLayers, subresourceRange,
                                         loc.StringFunc().c_str(), loc.StringField().c_str(), "arrayLayers", image_state->image(),
                                         sync_vuid_maps::GetSubResourceVUIDs(loc));
}

namespace barrier_queue_families {
using sync_vuid_maps::GetBarrierQueueVUID;
using sync_vuid_maps::kQueueErrorSummary;
using sync_vuid_maps::QueueError;

class ValidatorState {
  public:
    ValidatorState(const ValidationStateTracker *device_data, LogObjectList &&obj, const core_error::Location &location,
                   const VulkanTypedHandle &barrier_handle, const VkSharingMode sharing_mode)
        : device_data_(device_data),
          objects_(std::move(obj)),
          loc_(location),
          barrier_handle_(barrier_handle),
          sharing_mode_(sharing_mode),
          limit_(static_cast<uint32_t>(device_data->physical_device_state->queue_family_properties.size())),
          mem_ext_(IsExtEnabled(device_data->device_extensions.vk_khr_external_memory)) {}

    // Log the messages using boilerplate from object state, and Vu specific information from the template arg
    // One and two family versions, in the single family version, Vu holds the name of the passed parameter
    bool LogMsg(QueueError vu_index, uint32_t family, const char *param_name) const {
        const std::string val_code = GetBarrierQueueVUID(loc_, vu_index);
        const char *annotation = GetFamilyAnnotation(family);
        return device_data_->LogError(
            objects_, val_code, "%s Barrier using %s %s created with sharingMode %s, has %s %" PRIu32 "%s. %s",
            loc_.Message().c_str(), GetTypeString(), device_data_->report_data->FormatHandle(barrier_handle_).c_str(),
            GetModeString(), param_name, family, annotation, kQueueErrorSummary.at(vu_index).c_str());
    }

    bool LogMsg(QueueError vu_index, uint32_t src_family, uint32_t dst_family) const {
        const std::string val_code = GetBarrierQueueVUID(loc_, vu_index);
        const char *src_annotation = GetFamilyAnnotation(src_family);
        const char *dst_annotation = GetFamilyAnnotation(dst_family);
        return device_data_->LogError(objects_, val_code,
                                      "%s Barrier using %s %s created with sharingMode %s, has srcQueueFamilyIndex %" PRIu32
                                      "%s and dstQueueFamilyIndex %" PRIu32 "%s. %s",
                                      loc_.Message().c_str(), GetTypeString(),
                                      device_data_->report_data->FormatHandle(barrier_handle_).c_str(), GetModeString(), src_family,
                                      src_annotation, dst_family, dst_annotation, kQueueErrorSummary.at(vu_index).c_str());
    }

    // This abstract Vu can only be tested at submit time, thus we need a callback from the closure containing the needed
    // data. Note that the mem_barrier is copied to the closure as the lambda lifespan exceed the guarantees of validity for
    // application input.
    static bool ValidateAtQueueSubmit(const QUEUE_STATE *queue_state, const ValidationStateTracker *device_data,
                                      uint32_t src_family, uint32_t dst_family, const ValidatorState &val) {
        auto error_code = QueueError::kSubmitQueueMustMatchSrcOrDst;
        uint32_t queue_family = queue_state->queueFamilyIndex;
        if ((src_family != queue_family) && (dst_family != queue_family)) {
            const std::string val_code = GetBarrierQueueVUID(val.loc_, error_code);
            const char *src_annotation = val.GetFamilyAnnotation(src_family);
            const char *dst_annotation = val.GetFamilyAnnotation(dst_family);
            return device_data->LogError(queue_state->Handle(), val_code,
                                         "%s Barrier submitted to queue with family index %" PRIu32
                                         ", using %s %s created with sharingMode %s, has "
                                         "srcQueueFamilyIndex %" PRIu32 "%s and dstQueueFamilyIndex %" PRIu32 "%s. %s",
                                         val.loc_.Message().c_str(), queue_family, val.GetTypeString(),
                                         device_data->report_data->FormatHandle(val.barrier_handle_).c_str(), val.GetModeString(),
                                         src_family, src_annotation, dst_family, dst_annotation,
                                         kQueueErrorSummary.at(error_code).c_str());
        }
        return false;
    }
    // Logical helpers for semantic clarity
    inline bool KhrExternalMem() const { return mem_ext_; }
    inline bool IsValid(uint32_t queue_family) const { return (queue_family < limit_); }
    inline bool IsValidOrSpecial(uint32_t queue_family) const {
        return IsValid(queue_family) || (mem_ext_ && QueueFamilyIsExternal(queue_family)) ||
               (queue_family == VK_QUEUE_FAMILY_IGNORED);
    }

    // Helpers for LogMsg
    const char *GetModeString() const { return string_VkSharingMode(sharing_mode_); }

    // Descriptive text for the various types of queue family index
    const char *GetFamilyAnnotation(uint32_t family) const {
        const char *external = " (VK_QUEUE_FAMILY_EXTERNAL)";
        const char *foreign = " (VK_QUEUE_FAMILY_FOREIGN_EXT)";
        const char *ignored = " (VK_QUEUE_FAMILY_IGNORED)";
        const char *valid = " (VALID)";
        const char *invalid = " (INVALID)";
        switch (family) {
            case VK_QUEUE_FAMILY_EXTERNAL:
                return external;
            case VK_QUEUE_FAMILY_FOREIGN_EXT:
                return foreign;
            case VK_QUEUE_FAMILY_IGNORED:
                return ignored;
            default:
                if (IsValid(family)) {
                    return valid;
                }
                return invalid;
        };
    }
    const char *GetTypeString() const { return object_string[barrier_handle_.type]; }
    VkSharingMode GetSharingMode() const { return sharing_mode_; }

  protected:
    const ValidationStateTracker *device_data_;
    const LogObjectList objects_;
    const core_error::Location loc_;
    const VulkanTypedHandle barrier_handle_;
    const VkSharingMode sharing_mode_;
    const uint32_t limit_;
    const bool mem_ext_;
};

bool Validate(const CoreChecks *device_data, const CMD_BUFFER_STATE *cb_state, const ValidatorState &val,
              const uint32_t src_queue_family, const uint32_t dst_queue_family) {
    bool skip = false;

    const bool mode_concurrent = val.GetSharingMode() == VK_SHARING_MODE_CONCURRENT;
    const bool src_ignored = QueueFamilyIsIgnored(src_queue_family);
    const bool dst_ignored = QueueFamilyIsIgnored(dst_queue_family);
    if (val.KhrExternalMem()) {
        if (mode_concurrent) {
            const bool sync2 = device_data->enabled_features.core13.synchronization2 != 0;
            // this requirement is removed by VK_KHR_synchronization2
            if (!(src_ignored || dst_ignored) && !sync2) {
                skip |= val.LogMsg(QueueError::kSrcOrDstMustBeIgnore, src_queue_family, dst_queue_family);
            }
            if ((src_ignored && !(dst_ignored || QueueFamilyIsExternal(dst_queue_family))) ||
                (dst_ignored && !(src_ignored || QueueFamilyIsExternal(src_queue_family)))) {
                skip |= val.LogMsg(QueueError::kSpecialOrIgnoreOnly, src_queue_family, dst_queue_family);
            }
        } else {
            // VK_SHARING_MODE_EXCLUSIVE
            if (src_queue_family != dst_queue_family) {
                if (!val.IsValidOrSpecial(dst_queue_family)) {
                    skip |= val.LogMsg(QueueError::kSrcAndDstValidOrSpecial, dst_queue_family, "dstQueueFamilyIndex");
                }
                if (!val.IsValidOrSpecial(src_queue_family)) {
                    skip |= val.LogMsg(QueueError::kSrcAndDstValidOrSpecial, src_queue_family, "srcQueueFamilyIndex");
                }
            }
        }
    } else {
        // No memory extension
        if (mode_concurrent) {
            const bool sync2 = device_data->enabled_features.core13.synchronization2 != 0;
            // this requirement is removed by VK_KHR_synchronization2
            if ((!src_ignored || !dst_ignored) && !sync2) {
                skip |= val.LogMsg(QueueError::kSrcAndDestMustBeIgnore, src_queue_family, dst_queue_family);
            }
        } else {
            // VK_SHARING_MODE_EXCLUSIVE
            if ((src_queue_family != dst_queue_family) && !(val.IsValid(src_queue_family) && val.IsValid(dst_queue_family))) {
                skip |= val.LogMsg(QueueError::kSrcAndDstBothValid, src_queue_family, dst_queue_family);
            }
        }
    }
    return skip;
}
}  // namespace barrier_queue_families

bool CoreChecks::ValidateConcurrentBarrierAtSubmit(const Location &loc, const ValidationStateTracker &state_data,
                                                   const QUEUE_STATE &queue_state, const CMD_BUFFER_STATE &cb_state,
                                                   const VulkanTypedHandle &typed_handle, uint32_t src_queue_family,
                                                   uint32_t dst_queue_family) {
    using barrier_queue_families::ValidatorState;
    ValidatorState val(&state_data, LogObjectList(cb_state.Handle()), loc, typed_handle, VK_SHARING_MODE_CONCURRENT);
    return ValidatorState::ValidateAtQueueSubmit(&queue_state, &state_data, src_queue_family, dst_queue_family, val);
}

// Type specific wrapper for image barriers
template <typename ImgBarrier>
bool CoreChecks::ValidateBarrierQueueFamilies(const Location &loc, const CMD_BUFFER_STATE *cb_state, const ImgBarrier &barrier,
                                              const IMAGE_STATE *state_data) const {
    // State data is required
    if (!state_data) {
        return false;
    }

    // Create the validator state from the image state
    barrier_queue_families::ValidatorState val(this, LogObjectList(cb_state->commandBuffer()), loc, state_data->Handle(),
                                               state_data->createInfo.sharingMode);
    const uint32_t src_queue_family = barrier.srcQueueFamilyIndex;
    const uint32_t dst_queue_family = barrier.dstQueueFamilyIndex;
    return barrier_queue_families::Validate(this, cb_state, val, src_queue_family, dst_queue_family);
}

// Type specific wrapper for buffer barriers
template <typename BufBarrier>
bool CoreChecks::ValidateBarrierQueueFamilies(const Location &loc, const CMD_BUFFER_STATE *cb_state, const BufBarrier &barrier,
                                              const BUFFER_STATE *state_data) const {
    // State data is required
    if (!state_data) {
        return false;
    }

    // Create the validator state from the buffer state
    barrier_queue_families::ValidatorState val(this, LogObjectList(cb_state->commandBuffer()), loc, state_data->Handle(),
                                               state_data->createInfo.sharingMode);
    const uint32_t src_queue_family = barrier.srcQueueFamilyIndex;
    const uint32_t dst_queue_family = barrier.dstQueueFamilyIndex;
    return barrier_queue_families::Validate(this, cb_state, val, src_queue_family, dst_queue_family);
}

template <typename Barrier>
bool CoreChecks::ValidateBufferBarrier(const LogObjectList &objects, const Location &loc, const CMD_BUFFER_STATE *cb_state,
                                       const Barrier &mem_barrier) const {
    using sync_vuid_maps::BufferError;
    using sync_vuid_maps::GetBufferBarrierVUID;

    bool skip = false;

    skip |= ValidateQFOTransferBarrierUniqueness(loc, cb_state, mem_barrier, cb_state->qfo_transfer_buffer_barriers);

    // Validate buffer barrier queue family indices
    auto buffer_state = Get<BUFFER_STATE>(mem_barrier.buffer);
    if (buffer_state) {
        auto buf_loc = loc.dot(Field::buffer);
        const auto &mem_vuid = GetBufferBarrierVUID(buf_loc, BufferError::kNoMemory);
        skip |= ValidateMemoryIsBoundToBuffer(cb_state->commandBuffer(), *buffer_state, loc.StringFunc().c_str(), mem_vuid.c_str());

        skip |= ValidateBarrierQueueFamilies(buf_loc, cb_state, mem_barrier, buffer_state.get());

        auto buffer_size = buffer_state->createInfo.size;
        if (mem_barrier.offset >= buffer_size) {
            auto offset_loc = loc.dot(Field::offset);
            const auto &vuid = GetBufferBarrierVUID(offset_loc, BufferError::kOffsetTooBig);

            skip |= LogError(objects, vuid, "%s %s has offset 0x%" PRIx64 " which is not less than total size 0x%" PRIx64 ".",
                             offset_loc.Message().c_str(), report_data->FormatHandle(mem_barrier.buffer).c_str(),
                             HandleToUint64(mem_barrier.offset), HandleToUint64(buffer_size));
        } else if (mem_barrier.size != VK_WHOLE_SIZE && (mem_barrier.offset + mem_barrier.size > buffer_size)) {
            auto size_loc = loc.dot(Field::size);
            const auto &vuid = GetBufferBarrierVUID(size_loc, BufferError::kSizeOutOfRange);
            skip |= LogError(objects, vuid,
                             "%s %s has offset 0x%" PRIx64 " and size 0x%" PRIx64 " whose sum is greater than total size 0x%" PRIx64
                             ".",
                             size_loc.Message().c_str(), report_data->FormatHandle(mem_barrier.buffer).c_str(),
                             HandleToUint64(mem_barrier.offset), HandleToUint64(mem_barrier.size), HandleToUint64(buffer_size));
        }
        if (mem_barrier.size == 0) {
            auto size_loc = loc.dot(Field::size);
            const auto &vuid = GetBufferBarrierVUID(size_loc, BufferError::kSizeZero);
            skip |= LogError(objects, vuid, "%s %s has a size of 0.", loc.Message().c_str(),
                             report_data->FormatHandle(mem_barrier.buffer).c_str());
        }
    }

    if (mem_barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL &&
        mem_barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL) {
        auto size_loc = loc.dot(Field::srcQueueFamilyIndex);
        const auto &vuid = GetBufferBarrierVUID(size_loc, BufferError::kQueueFamilyExternal);
        skip |= LogError(objects, vuid, "Both srcQueueFamilyIndex and dstQueueFamilyIndex are VK_QUEUE_FAMILY_EXTERNAL.");
    }
    return skip;
}

template <typename Barrier>
bool CoreChecks::ValidateImageBarrier(const LogObjectList &objects, const Location &loc, const CMD_BUFFER_STATE *cb_state,
                                      const Barrier &mem_barrier) const {
    bool skip = false;

    skip |= ValidateQFOTransferBarrierUniqueness(loc, cb_state, mem_barrier, cb_state->qfo_transfer_image_barriers);

    bool is_ilt = true;
    if (enabled_features.core13.synchronization2) {
        is_ilt = mem_barrier.oldLayout != mem_barrier.newLayout;
    }

    if (is_ilt) {
        if (mem_barrier.newLayout == VK_IMAGE_LAYOUT_UNDEFINED || mem_barrier.newLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
            auto layout_loc = loc.dot(Field::newLayout);
            const auto &vuid = sync_vuid_maps::GetImageBarrierVUID(loc, sync_vuid_maps::ImageError::kBadLayout);
            skip |=
                LogError(cb_state->commandBuffer(), vuid, "%s Image Layout cannot be transitioned to UNDEFINED or PREINITIALIZED.",
                         layout_loc.Message().c_str());
        }
    }

    if (mem_barrier.newLayout == VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT) {
        if (!enabled_features.attachment_feedback_loop_layout_features.attachmentFeedbackLoopLayout) {
            auto layout_loc = loc.dot(Field::newLayout);
            const auto &vuid = sync_vuid_maps::GetImageBarrierVUID(loc, sync_vuid_maps::ImageError::kBadAttFeedbackLoopLayout);
            skip |= LogError(cb_state->commandBuffer(), vuid,
                             "%s Image Layout cannot be transitioned to VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT if "
                             "the attachmentFeedbackLoopLayout feature is not enabled",
                             layout_loc.Message().c_str());
        }
    }

    auto image_data = Get<IMAGE_STATE>(mem_barrier.image);
    if (image_data) {
        auto image_loc = loc.dot(Field::image);

        skip |= ValidateMemoryIsBoundToImage(cb_state->commandBuffer(), *image_data, loc);

        skip |= ValidateBarrierQueueFamilies(image_loc, cb_state, mem_barrier, image_data.get());

        skip |= ValidateImageAspectMask(image_data->image(), image_data->createInfo.format, mem_barrier.subresourceRange.aspectMask,
                                        image_data->disjoint, loc.StringFunc().c_str());

        skip |=
            ValidateImageBarrierSubresourceRange(loc.dot(Field::subresourceRange), image_data.get(), mem_barrier.subresourceRange);
        skip |= ValidateImageAcquired(*image_data, loc.StringFunc().c_str());
    }
    return skip;
}

bool CoreChecks::ValidateBarriers(const Location &outer_loc, const CMD_BUFFER_STATE *cb_state, VkPipelineStageFlags src_stage_mask,
                                  VkPipelineStageFlags dst_stage_mask, uint32_t memBarrierCount,
                                  const VkMemoryBarrier *pMemBarriers, uint32_t bufferBarrierCount,
                                  const VkBufferMemoryBarrier *pBufferMemBarriers, uint32_t imageMemBarrierCount,
                                  const VkImageMemoryBarrier *pImageMemBarriers) const {
    bool skip = false;
    LogObjectList objects(cb_state->commandBuffer());

    for (uint32_t i = 0; i < memBarrierCount; ++i) {
        const auto &mem_barrier = pMemBarriers[i];
        auto loc = outer_loc.dot(Struct::VkMemoryBarrier, Field::pMemoryBarriers, i);
        skip |= ValidateMemoryBarrier(objects, loc, cb_state, mem_barrier, src_stage_mask, dst_stage_mask);
    }
    for (uint32_t i = 0; i < imageMemBarrierCount; ++i) {
        const auto &mem_barrier = pImageMemBarriers[i];
        auto loc = outer_loc.dot(Struct::VkImageMemoryBarrier, Field::pImageMemoryBarriers, i);
        skip |= ValidateMemoryBarrier(objects, loc, cb_state, mem_barrier, src_stage_mask, dst_stage_mask);
        skip |= ValidateImageBarrier(objects, loc, cb_state, mem_barrier);
    }
    {
        Location loc(outer_loc.function, Struct::VkImageMemoryBarrier);
        skip |= ValidateBarriersToImages(loc, cb_state, imageMemBarrierCount, pImageMemBarriers);
    }
    for (uint32_t i = 0; i < bufferBarrierCount; ++i) {
        const auto &mem_barrier = pBufferMemBarriers[i];
        auto loc = outer_loc.dot(Struct::VkBufferMemoryBarrier, Field::pMemoryBarriers, i);
        skip |= ValidateMemoryBarrier(objects, loc, cb_state, mem_barrier, src_stage_mask, dst_stage_mask);
        skip |= ValidateBufferBarrier(objects, loc, cb_state, mem_barrier);
    }
    return skip;
}

bool CoreChecks::ValidateDependencyInfo(const LogObjectList &objects, const Location &outer_loc, const CMD_BUFFER_STATE *cb_state,
                                        const VkDependencyInfoKHR *dep_info) const {
    bool skip = false;

    if (cb_state->activeRenderPass) {
        skip |= ValidateRenderPassPipelineBarriers(outer_loc, cb_state, dep_info);
        if (skip) return true;  // Early return to avoid redundant errors from below calls
    }
    for (uint32_t i = 0; i < dep_info->memoryBarrierCount; ++i) {
        const auto &mem_barrier = dep_info->pMemoryBarriers[i];
        auto loc = outer_loc.dot(Struct::VkMemoryBarrier2, Field::pMemoryBarriers, i);
        skip |= ValidateMemoryBarrier(objects, loc, cb_state, mem_barrier);
    }
    for (uint32_t i = 0; i < dep_info->imageMemoryBarrierCount; ++i) {
        const auto &mem_barrier = dep_info->pImageMemoryBarriers[i];
        auto loc = outer_loc.dot(Struct::VkImageMemoryBarrier2, Field::pImageMemoryBarriers, i);
        skip |= ValidateMemoryBarrier(objects, loc, cb_state, mem_barrier);
        skip |= ValidateImageBarrier(objects, loc, cb_state, mem_barrier);
    }
    {
        Location loc(outer_loc.function, Struct::VkImageMemoryBarrier2);
        skip |= ValidateBarriersToImages(loc, cb_state, dep_info->imageMemoryBarrierCount, dep_info->pImageMemoryBarriers);
    }

    for (uint32_t i = 0; i < dep_info->bufferMemoryBarrierCount; ++i) {
        const auto &mem_barrier = dep_info->pBufferMemoryBarriers[i];
        auto loc = outer_loc.dot(Struct::VkBufferMemoryBarrier2, Field::pBufferMemoryBarriers, i);
        skip |= ValidateMemoryBarrier(objects, loc, cb_state, mem_barrier);
        skip |= ValidateBufferBarrier(objects, loc, cb_state, mem_barrier);
    }

    return skip;
}

// template to check all original barrier structures
template <typename Barrier>
bool CoreChecks::ValidateMemoryBarrier(const LogObjectList &objects, const Location &loc, const CMD_BUFFER_STATE *cb_state,
                                       const Barrier &barrier, VkPipelineStageFlags src_stage_mask,
                                       VkPipelineStageFlags dst_stage_mask) const {
    bool skip = false;
    assert(cb_state);
    auto queue_flags = cb_state->GetQueueFlags();

    if (!cb_state->IsAcquireOp(barrier)) {
        skip |= ValidateAccessMask(objects, loc.dot(Field::srcAccessMask), queue_flags, barrier.srcAccessMask, src_stage_mask);
    }
    if (!cb_state->IsReleaseOp(barrier)) {
        skip |= ValidateAccessMask(objects, loc.dot(Field::dstAccessMask), queue_flags, barrier.dstAccessMask, dst_stage_mask);
    }
    return skip;
}

// template to check all synchronization2 barrier structures
template <typename Barrier>
bool CoreChecks::ValidateMemoryBarrier(const LogObjectList &objects, const Location &loc, const CMD_BUFFER_STATE *cb_state,
                                       const Barrier &barrier) const {
    bool skip = false;
    assert(cb_state);
    auto queue_flags = cb_state->GetQueueFlags();

    skip |= ValidatePipelineStage(objects, loc.dot(Field::srcStageMask), queue_flags, barrier.srcStageMask);
    if (!cb_state->IsAcquireOp(barrier)) {
        skip |=
            ValidateAccessMask(objects, loc.dot(Field::srcAccessMask), queue_flags, barrier.srcAccessMask, barrier.srcStageMask);
    }

    skip |= ValidatePipelineStage(objects, loc.dot(Field::dstStageMask), queue_flags, barrier.dstStageMask);
    if (!cb_state->IsReleaseOp(barrier)) {
        skip |=
            ValidateAccessMask(objects, loc.dot(Field::dstAccessMask), queue_flags, barrier.dstAccessMask, barrier.dstStageMask);
    }
    return skip;
}

// VkSubpassDependency validation happens when vkCreateRenderPass() is called.
// Dependencies between subpasses can only use pipeline stages compatible with VK_QUEUE_GRAPHICS_BIT,
// for external subpasses we don't have a yet command buffer so we have to assume all of them are valid.
static inline VkQueueFlags SubpassToQueueFlags(uint32_t subpass) {
    return subpass == VK_SUBPASS_EXTERNAL ? sync_utils::kAllQueueTypes : static_cast<VkQueueFlags>(VK_QUEUE_GRAPHICS_BIT);
}

bool CoreChecks::ValidateSubpassDependency(const LogObjectList &objects, const Location &in_loc,
                                           const VkSubpassDependency2 &dependency) const {
    bool skip = false;
    Location loc = in_loc;
    VkMemoryBarrier2KHR converted_barrier;
    const auto *mem_barrier = LvlFindInChain<VkMemoryBarrier2KHR>(dependency.pNext);

    if (mem_barrier && enabled_features.core13.synchronization2) {
        if (dependency.srcAccessMask != 0) {
            skip |= LogError(objects, "UNASSIGNED-CoreChecks-VkSubpassDependency2-srcAccessMask",
                             "%s is non-zero when a VkMemoryBarrier2KHR is present in pNext.",
                             loc.dot(Field::srcAccessMask).Message().c_str());
        }
        if (dependency.dstAccessMask != 0) {
            skip |= LogError(objects, "UNASSIGNED-CoreChecks-VkSubpassDependency2-dstAccessMask",
                             "%s dstAccessMask is non-zero when a VkMemoryBarrier2KHR is present in pNext.",
                             loc.dot(Field::dstAccessMask).Message().c_str());
        }
        if (dependency.srcStageMask != 0) {
            skip |= LogError(objects, "UNASSIGNED-CoreChecks-VkSubpassDependency2-srcStageMask",
                             "%s srcStageMask is non-zero when a VkMemoryBarrier2KHR is present in pNext.",
                             loc.dot(Field::srcStageMask).Message().c_str());
        }
        if (dependency.dstStageMask != 0) {
            skip |= LogError(objects, "UNASSIGNED-CoreChecks-VkSubpassDependency2-dstStageMask",
                             "%s dstStageMask is non-zero when a VkMemoryBarrier2KHR is present in pNext.",
                             loc.dot(Field::dstStageMask).Message().c_str());
        }
        loc = in_loc.dot(Field::pNext);
        converted_barrier = *mem_barrier;
    } else {
        if (mem_barrier) {
            skip |= LogError(objects, "UNASSIGNED-CoreChecks-VkSubpassDependency2-pNext",
                             "%s a VkMemoryBarrier2KHR is present in pNext but synchronization2 is not enabled.",
                             loc.Message().c_str());
        }
        // use the subpass dependency flags, upconverted into wider synchronization2 fields.
        converted_barrier.srcStageMask = dependency.srcStageMask;
        converted_barrier.dstStageMask = dependency.dstStageMask;
        converted_barrier.srcAccessMask = dependency.srcAccessMask;
        converted_barrier.dstAccessMask = dependency.dstAccessMask;
    }
    auto src_queue_flags = SubpassToQueueFlags(dependency.srcSubpass);
    skip |= ValidatePipelineStage(objects, loc.dot(Field::srcStageMask), src_queue_flags, converted_barrier.srcStageMask);
    skip |= ValidateAccessMask(objects, loc.dot(Field::srcAccessMask), src_queue_flags, converted_barrier.srcAccessMask,
                               converted_barrier.srcStageMask);

    auto dst_queue_flags = SubpassToQueueFlags(dependency.dstSubpass);
    skip |= ValidatePipelineStage(objects, loc.dot(Field::dstStageMask), dst_queue_flags, converted_barrier.dstStageMask);
    skip |= ValidateAccessMask(objects, loc.dot(Field::dstAccessMask), dst_queue_flags, converted_barrier.dstAccessMask,
                               converted_barrier.dstStageMask);
    return skip;
}

bool CoreChecks::ValidateImageViewFormatFeatures(const IMAGE_STATE *image_state, const VkFormat view_format,
                                                 const VkImageUsageFlags image_usage) const {
    // Pass in image_usage here instead of extracting it from image_state in case there's a chained VkImageViewUsageCreateInfo
    bool skip = false;

    VkFormatFeatureFlags2KHR tiling_features = 0;
    const VkImageTiling image_tiling = image_state->createInfo.tiling;

    if (image_state->HasAHBFormat()) {
        // AHB image view and image share same feature sets
        tiling_features = image_state->format_features;
    } else if (image_tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        // Parameter validation should catch if this is used without VK_EXT_image_drm_format_modifier
        assert(IsExtEnabled(device_extensions.vk_ext_image_drm_format_modifier));
        VkImageDrmFormatModifierPropertiesEXT drm_format_properties = LvlInitStruct<VkImageDrmFormatModifierPropertiesEXT>();
        DispatchGetImageDrmFormatModifierPropertiesEXT(device, image_state->image(), &drm_format_properties);

        auto fmt_drm_props = LvlInitStruct<VkDrmFormatModifierPropertiesListEXT>();
        auto fmt_props_2 = LvlInitStruct<VkFormatProperties2>(&fmt_drm_props);
        DispatchGetPhysicalDeviceFormatProperties2(physical_device, view_format, &fmt_props_2);

        std::vector<VkDrmFormatModifierPropertiesEXT> drm_properties;
        drm_properties.resize(fmt_drm_props.drmFormatModifierCount);
        fmt_drm_props.pDrmFormatModifierProperties = drm_properties.data();

        DispatchGetPhysicalDeviceFormatProperties2(physical_device, view_format, &fmt_props_2);

        for (uint32_t i = 0; i < fmt_drm_props.drmFormatModifierCount; i++) {
            if (fmt_drm_props.pDrmFormatModifierProperties[i].drmFormatModifier == drm_format_properties.drmFormatModifier) {
                tiling_features = fmt_drm_props.pDrmFormatModifierProperties[i].drmFormatModifierTilingFeatures;
                break;
            }
        }
    } else {
        VkFormatProperties3KHR format_properties = GetPDFormatProperties(view_format);
        tiling_features = (image_tiling == VK_IMAGE_TILING_LINEAR) ? format_properties.linearTilingFeatures
                                                                   : format_properties.optimalTilingFeatures;
    }

    if (tiling_features == 0) {
        skip |= LogError(image_state->image(), "VUID-VkImageViewCreateInfo-None-02273",
                         "vkCreateImageView(): pCreateInfo->format %s with tiling %s has no supported format features on this "
                         "physical device.",
                         string_VkFormat(view_format), string_VkImageTiling(image_tiling));
    } else if ((image_usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(tiling_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        skip |= LogError(image_state->image(), "VUID-VkImageViewCreateInfo-usage-02274",
                         "vkCreateImageView(): pCreateInfo->format %s with tiling %s does not support usage that includes "
                         "VK_IMAGE_USAGE_SAMPLED_BIT.",
                         string_VkFormat(view_format), string_VkImageTiling(image_tiling));
    } else if ((image_usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(tiling_features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        skip |= LogError(image_state->image(), "VUID-VkImageViewCreateInfo-usage-02275",
                         "vkCreateImageView(): pCreateInfo->format %s with tiling %s does not support usage that includes "
                         "VK_IMAGE_USAGE_STORAGE_BIT.",
                         string_VkFormat(view_format), string_VkImageTiling(image_tiling));
    } else if ((image_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(tiling_features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        skip |= LogError(image_state->image(), "VUID-VkImageViewCreateInfo-usage-02276",
                         "vkCreateImageView(): pCreateInfo->format %s with tiling %s does not support usage that includes "
                         "VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT.",
                         string_VkFormat(view_format), string_VkImageTiling(image_tiling));
    } else if ((image_usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
               !(tiling_features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        skip |= LogError(image_state->image(), "VUID-VkImageViewCreateInfo-usage-02277",
                         "vkCreateImageView(): pCreateInfo->format %s with tiling %s does not support usage that includes "
                         "VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT.",
                         string_VkFormat(view_format), string_VkImageTiling(image_tiling));
    } else if ((image_usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) &&
               !(tiling_features & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        skip |= LogError(image_state->image(), "VUID-VkImageViewCreateInfo-usage-02652",
                         "vkCreateImageView(): pCreateInfo->format %s with tiling %s does not support usage that includes "
                         "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT or VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT.",
                         string_VkFormat(view_format), string_VkImageTiling(image_tiling));
    } else if ((image_usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) &&
               !(tiling_features & VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)) {
        if (enabled_features.fragment_shading_rate_features.attachmentFragmentShadingRate) {
            skip |= LogError(image_state->image(), "VUID-VkImageViewCreateInfo-usage-04550",
                             "vkCreateImageView(): pCreateInfo->format %s with tiling %s does not support usage that includes "
                             "VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR.",
                             string_VkFormat(view_format), string_VkImageTiling(image_tiling));
        }
    }

    return skip;
}

bool CoreChecks::PreCallValidateCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator, VkImageView *pView) const {
    bool skip = false;
    auto image_state = Get<IMAGE_STATE>(pCreateInfo->image);
    if (image_state) {
        const VkImageUsageFlags valid_usage_flags =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
            VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
            VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
            VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR | VK_IMAGE_USAGE_SAMPLE_WEIGHT_BIT_QCOM |
            VK_IMAGE_USAGE_SAMPLE_BLOCK_MATCH_BIT_QCOM;
        skip |= ValidateImageUsageFlags(VK_NULL_HANDLE, *image_state, valid_usage_flags, false,
                                        "VUID-VkImageViewCreateInfo-image-04441", "vkCreateImageView()");
        // If this isn't a sparse image, it needs to have memory backing it at CreateImageView time
        skip |= ValidateMemoryIsBoundToImage(device, *image_state, "vkCreateImageView()", "VUID-VkImageViewCreateInfo-image-01020");
        // Checks imported from image layer
        skip |= ValidateCreateImageViewSubresourceRange(
            image_state.get(),
            pCreateInfo->viewType == VK_IMAGE_VIEW_TYPE_2D || pCreateInfo->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            pCreateInfo->subresourceRange);

        const auto normalized_subresource_range = image_state->NormalizeSubresourceRange(pCreateInfo->subresourceRange);
        VkImageCreateFlags image_flags = image_state->createInfo.flags;
        VkFormat image_format = image_state->createInfo.format;
        VkImageUsageFlags image_usage = image_state->createInfo.usage;
        VkFormat view_format = pCreateInfo->format;
        VkImageAspectFlags aspect_mask = pCreateInfo->subresourceRange.aspectMask;
        VkImageType image_type = image_state->createInfo.imageType;
        VkImageViewType view_type = pCreateInfo->viewType;
        uint32_t layer_count = pCreateInfo->subresourceRange.layerCount;

        // If there's a chained VkImageViewUsageCreateInfo struct, modify image_usage to match
        auto chained_ivuci_struct = LvlFindInChain<VkImageViewUsageCreateInfo>(pCreateInfo->pNext);
        if (chained_ivuci_struct) {
            if (IsExtEnabled(device_extensions.vk_khr_maintenance2)) {
                if (!IsExtEnabled(device_extensions.vk_ext_separate_stencil_usage)) {
                    if ((image_usage | chained_ivuci_struct->usage) != image_usage) {
                        skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-pNext-02661",
                                         "vkCreateImageView(): pNext chain includes VkImageViewUsageCreateInfo, usage must not "
                                         "include any bits that were not set in VkImageCreateInfo::usage used to create image");
                    }
                } else {
                    const auto image_stencil_struct = LvlFindInChain<VkImageStencilUsageCreateInfo>(image_state->createInfo.pNext);
                    if (image_stencil_struct == nullptr) {
                        if ((image_usage | chained_ivuci_struct->usage) != image_usage) {
                            skip |= LogError(
                                pCreateInfo->image, "VUID-VkImageViewCreateInfo-pNext-02662",
                                "vkCreateImageView(): pNext chain includes VkImageViewUsageCreateInfo and image was not created "
                                "with a VkImageStencilUsageCreateInfo in pNext of vkImageCreateInfo, usage must not include "
                                "any bits that were not set in VkImageCreateInfo::usage used to create image");
                        }
                    } else {
                        if ((aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) == VK_IMAGE_ASPECT_STENCIL_BIT &&
                            (image_stencil_struct->stencilUsage | chained_ivuci_struct->usage) !=
                                image_stencil_struct->stencilUsage) {
                            skip |= LogError(
                                pCreateInfo->image, "VUID-VkImageViewCreateInfo-pNext-02663",
                                "vkCreateImageView(): pNext chain includes VkImageViewUsageCreateInfo, image was created with a "
                                "VkImageStencilUsageCreateInfo in pNext of vkImageCreateInfo, and subResourceRange.aspectMask "
                                "includes VK_IMAGE_ASPECT_STENCIL_BIT, VkImageViewUsageCreateInfo::usage must not include any "
                                "bits that were not set in VkImageStencilUsageCreateInfo::stencilUsage used to create image");
                        }
                        if ((aspect_mask & ~VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
                            (image_usage | chained_ivuci_struct->usage) != image_usage) {
                            skip |= LogError(
                                pCreateInfo->image, "VUID-VkImageViewCreateInfo-pNext-02664",
                                "vkCreateImageView(): pNext chain includes VkImageViewUsageCreateInfo, image was created with a "
                                "VkImageStencilUsageCreateInfo in pNext of vkImageCreateInfo, and subResourceRange.aspectMask "
                                "includes bits other than VK_IMAGE_ASPECT_STENCIL_BIT, VkImageViewUsageCreateInfo::usage must not "
                                "include any bits that were not set in VkImageCreateInfo::usage used to create image");
                        }
                    }
                }
            }

            image_usage = chained_ivuci_struct->usage;
        }

        // If image used VkImageFormatListCreateInfo need to make sure a format from list is used
        const auto format_list_info = LvlFindInChain<VkImageFormatListCreateInfo>(image_state->createInfo.pNext);
        if (format_list_info && (format_list_info->viewFormatCount > 0)) {
            bool foundFormat = false;
            for (uint32_t i = 0; i < format_list_info->viewFormatCount; i++) {
                if (format_list_info->pViewFormats[i] == view_format) {
                    foundFormat = true;
                    break;
                }
            }
            if (foundFormat == false) {
                skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-pNext-01585",
                                 "vkCreateImageView(): image was created with a VkImageFormatListCreateInfo in pNext of "
                                 "vkImageCreateInfo, but none of the formats match the VkImageViewCreateInfo::format (%s).",
                                 string_VkFormat(view_format));
            }
        }

        // Validate VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT state, if view/image formats differ
        if ((image_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && (image_format != view_format)) {
            if (FormatIsMultiplane(image_format)) {
                const VkFormat compat_format = FindMultiplaneCompatibleFormat(image_format, aspect_mask);
                auto image_class = FormatCompatibilityClass(compat_format);
                auto view_class = FormatCompatibilityClass(view_format);
                // Need to only check if one is NONE to handle edge case both are NONE
                if ((image_class != view_class) || (image_class == FORMAT_COMPATIBILITY_CLASS::NONE)) {
                    // View format must match the multiplane compatible format
                    std::stringstream ss;
                    ss << "vkCreateImageView(): ImageView format " << string_VkFormat(view_format)
                       << " is not compatible with plane " << GetPlaneIndex(aspect_mask) << " of underlying image format "
                       << string_VkFormat(image_format) << ", must be compatible with " << string_VkFormat(compat_format) << ".";
                    skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-01586", "%s", ss.str().c_str());
                }
            } else if (!(image_flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT)) {
                // Format MUST be compatible (in the same format compatibility class) as the format the image was created with
                auto image_class = FormatCompatibilityClass(image_format);
                auto view_class = FormatCompatibilityClass(view_format);
                // Need to only check if one is NONE to handle edge case both are NONE
                if ((image_class != view_class) || (image_class == FORMAT_COMPATIBILITY_CLASS::NONE)) {
                    const char *error_vuid;
                    if ((!IsExtEnabled(device_extensions.vk_khr_maintenance2)) &&
                        (!IsExtEnabled(device_extensions.vk_khr_sampler_ycbcr_conversion))) {
                        error_vuid = "VUID-VkImageViewCreateInfo-image-01018";
                    } else if ((IsExtEnabled(device_extensions.vk_khr_maintenance2)) &&
                               (!IsExtEnabled(device_extensions.vk_khr_sampler_ycbcr_conversion))) {
                        error_vuid = "VUID-VkImageViewCreateInfo-image-01759";
                    } else if ((!IsExtEnabled(device_extensions.vk_khr_maintenance2)) &&
                               (IsExtEnabled(device_extensions.vk_khr_sampler_ycbcr_conversion))) {
                        error_vuid = "VUID-VkImageViewCreateInfo-image-01760";
                    } else {
                        // both enabled
                        error_vuid = "VUID-VkImageViewCreateInfo-image-01761";
                    }
                    std::stringstream ss;
                    ss << "vkCreateImageView(): ImageView format " << string_VkFormat(view_format)
                       << " is not in the same format compatibility class as "
                       << report_data->FormatHandle(pCreateInfo->image).c_str() << "  format " << string_VkFormat(image_format)
                       << ".  Images created with the VK_IMAGE_CREATE_MUTABLE_FORMAT BIT "
                       << "can support ImageViews with differing formats but they must be in the same compatibility class.";
                    skip |= LogError(pCreateInfo->image, error_vuid, "%s", ss.str().c_str());
                }
            }
        } else {
            // Format MUST be IDENTICAL to the format the image was created with
            // Unless it is a multi-planar color bit aspect
            if ((image_format != view_format) &&
                ((FormatIsMultiplane(image_format) == false) || (aspect_mask != VK_IMAGE_ASPECT_COLOR_BIT))) {
                const char *vuid = IsExtEnabled(device_extensions.vk_khr_sampler_ycbcr_conversion)
                                       ? "VUID-VkImageViewCreateInfo-image-01762"
                                       : "VUID-VkImageViewCreateInfo-image-01019";
                std::stringstream ss;
                ss << "vkCreateImageView() format " << string_VkFormat(view_format) << " differs from "
                   << report_data->FormatHandle(pCreateInfo->image).c_str() << " format " << string_VkFormat(image_format)
                   << ".  Formats MUST be IDENTICAL unless VK_IMAGE_CREATE_MUTABLE_FORMAT BIT was set on image creation.";
                skip |= LogError(pCreateInfo->image, vuid, "%s", ss.str().c_str());
            }
        }

        if (image_state->createInfo.samples != VK_SAMPLE_COUNT_1_BIT && view_type != VK_IMAGE_VIEW_TYPE_2D &&
            view_type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
            skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-04972",
                             "vkCreateImageView(): image was created with sample count %s, but pCreateInfo->viewType is %s.",
                             string_VkSampleCountFlagBits(image_state->createInfo.samples), string_VkImageViewType(view_type));
        }

        if (image_state->createInfo.usage & (VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                                             VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR)) {
            if (view_type != VK_IMAGE_VIEW_TYPE_2D && view_type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
                skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-04818",
                                 "vkCreateImageView(): Image created with video encode usage flags, but pCreateInfo->viewType (%s) "
                                 "is not VK_IMAGE_VIEW_TYPE_2D or VK_IMAGE_VIEW_TYPE_2D_ARRAY.",
                                 string_VkImageViewType(view_type));
            }
            if (!IsIdentitySwizzle(pCreateInfo->components)) {
                skip |= LogError(
                    pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-04818",
                    "vkCreateImageView(): Image created with video encode usage flags, but not all members of "
                    "pCreateInfo->components have identity swizzle. Here are the actual swizzle values:\n"
                    "r swizzle = %s\n"
                    "g swizzle = %s\n"
                    "b swizzle = %s\n"
                    "a swizzle = %s\n",
                    string_VkComponentSwizzle(pCreateInfo->components.r), string_VkComponentSwizzle(pCreateInfo->components.g),
                    string_VkComponentSwizzle(pCreateInfo->components.b), string_VkComponentSwizzle(pCreateInfo->components.a));
            }
        }

        // Validate correct image aspect bits for desired formats and format consistency
        skip |=
            ValidateImageAspectMask(image_state->image(), image_format, aspect_mask, image_state->disjoint, "vkCreateImageView()");

        // Valdiate Image/ImageView type compatibility #resources-image-views-compatibility
        switch (image_type) {
            case VK_IMAGE_TYPE_1D:
                if (view_type != VK_IMAGE_VIEW_TYPE_1D && view_type != VK_IMAGE_VIEW_TYPE_1D_ARRAY) {
                    skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-subResourceRange-01021",
                                     "vkCreateImageView(): pCreateInfo->viewType %s is not compatible with image type %s.",
                                     string_VkImageViewType(view_type), string_VkImageType(image_type));
                }
                break;
            case VK_IMAGE_TYPE_2D:
                if (view_type != VK_IMAGE_VIEW_TYPE_2D && view_type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
                    if ((view_type == VK_IMAGE_VIEW_TYPE_CUBE || view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
                        !(image_flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) {
                        skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-01003",
                                         "vkCreateImageView(): pCreateInfo->viewType %s is not compatible with image type %s.",
                                         string_VkImageViewType(view_type), string_VkImageType(image_type));
                    } else if (view_type != VK_IMAGE_VIEW_TYPE_CUBE && view_type != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
                        skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-subResourceRange-01021",
                                         "vkCreateImageView(): pCreateInfo->viewType %s is not compatible with image type %s.",
                                         string_VkImageViewType(view_type), string_VkImageType(image_type));
                    }
                }
                break;
            case VK_IMAGE_TYPE_3D:
                if (IsExtEnabled(device_extensions.vk_khr_maintenance1)) {
                    if (view_type != VK_IMAGE_VIEW_TYPE_3D) {
                        if ((view_type == VK_IMAGE_VIEW_TYPE_2D || view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY)) {
                            if (IsExtEnabled(device_extensions.vk_ext_image_2d_view_of_3d)) {
                                if (!(image_flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) {
                                    if (view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
                                        skip |= LogError(
                                            pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-06723",
                                            "vkCreateImageView(): pCreateInfo->viewType %s is not compatible with image type "
                                            "%s since the image doesn't have VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT flag set.",
                                            string_VkImageViewType(view_type), string_VkImageType(image_type));
                                    } else if (view_type == VK_IMAGE_VIEW_TYPE_2D &&
                                               !(image_flags & VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT)) {
                                        skip |= LogError(
                                            pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-06728",
                                            "vkCreateImageView(): pCreateInfo->viewType %s is not compatible with image type "
                                            "%s since the image doesn't have VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT or "
                                            "VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT flag set.",
                                            string_VkImageViewType(view_type), string_VkImageType(image_type));
                                    }
                                }
                            } else if (!(image_flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) &&
                                       (view_type == VK_IMAGE_VIEW_TYPE_2D)) {
                                skip |=
                                    LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-06727",
                                             "vkCreateImageView(): pCreateInfo->viewType VK_IMAGE_VIEW_TYPE_2D is not compatible "
                                             "with image type "
                                             "%s since the image doesn't have VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT flag set.",
                                             string_VkImageType(image_type));
                            }
                            if ((image_flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                                                VK_IMAGE_CREATE_SPARSE_ALIASED_BIT))) {
                                skip |= LogError(
                                    pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-04971",
                                    "vkCreateImageView(): pCreateInfo->viewType %s is not compatible with image type %s "
                                    "when the VK_IMAGE_CREATE_SPARSE_BINDING_BIT, VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT, or "
                                    "VK_IMAGE_CREATE_SPARSE_ALIASED_BIT flags are enabled.",
                                    string_VkImageViewType(view_type), string_VkImageType(image_type));
                            } else if (pCreateInfo->subresourceRange.levelCount != 1) {
                                skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-04970",
                                                 "vkCreateImageView(): pCreateInfo->viewType %s is with image type %s must have a "
                                                 "levelCount of 1 but it is %" PRIu32 ".",
                                                 string_VkImageViewType(view_type), string_VkImageType(image_type),
                                                 pCreateInfo->subresourceRange.levelCount);
                            }
                        } else {
                            skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-subResourceRange-01021",
                                             "vkCreateImageView(): pCreateInfo->viewType %s is not compatible with image type %s.",
                                             string_VkImageViewType(view_type), string_VkImageType(image_type));
                        }
                    }
                } else {
                    if (view_type != VK_IMAGE_VIEW_TYPE_3D) {
                        // Help point to VK_KHR_maintenance1
                        if ((view_type == VK_IMAGE_VIEW_TYPE_2D || view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY)) {
                            skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-subResourceRange-01021",
                                             "vkCreateImageView(): pCreateInfo->viewType %s is not compatible with image type %s "
                                             "without VK_KHR_maintenance1 enabled which was promoted in Vulkan 1.0.",
                                             string_VkImageViewType(view_type), string_VkImageType(image_type));
                        } else {
                            skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-subResourceRange-01021",
                                             "vkCreateImageView(): pCreateInfo->viewType %s is not compatible with image type %s.",
                                             string_VkImageViewType(view_type), string_VkImageType(image_type));
                        }
                    }
                }
                break;
            default:
                break;
        }

        // External format checks needed when VK_ANDROID_external_memory_android_hardware_buffer enabled
        if (IsExtEnabled(device_extensions.vk_android_external_memory_android_hardware_buffer)) {
            skip |= ValidateCreateImageViewANDROID(pCreateInfo);
        }

        skip |= ValidateImageViewFormatFeatures(image_state.get(), view_format, image_usage);

        if (enabled_features.shading_rate_image_features.shadingRateImage) {
            if (image_usage & VK_IMAGE_USAGE_SHADING_RATE_IMAGE_BIT_NV) {
                if (view_format != VK_FORMAT_R8_UINT) {
                    skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-02087",
                                     "vkCreateImageView() If image was created with usage containing "
                                     "VK_IMAGE_USAGE_SHADING_RATE_IMAGE_BIT_NV, format must be VK_FORMAT_R8_UINT.");
                }
            }
        }

        if (enabled_features.shading_rate_image_features.shadingRateImage ||
            enabled_features.fragment_shading_rate_features.attachmentFragmentShadingRate) {
            if (image_usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) {
                if (view_type != VK_IMAGE_VIEW_TYPE_2D && view_type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
                    skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-02086",
                                     "vkCreateImageView() If image was created with usage containing "
                                     "VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, viewType must be "
                                     "VK_IMAGE_VIEW_TYPE_2D or VK_IMAGE_VIEW_TYPE_2D_ARRAY.");
                }
            }
        }

        if (enabled_features.fragment_shading_rate_features.attachmentFragmentShadingRate &&
            !phys_dev_ext_props.fragment_shading_rate_props.layeredShadingRateAttachments &&
            image_usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR && normalized_subresource_range.layerCount != 1) {
            skip |= LogError(device, "VUID-VkImageViewCreateInfo-usage-04551",
                             "vkCreateImageView(): subresourceRange.layerCount is %" PRIu32
                             " for a shading rate attachment image view.",
                             normalized_subresource_range.layerCount);
        }

        if (layer_count == VK_REMAINING_ARRAY_LAYERS) {
            const uint32_t remaining_layers = image_state->createInfo.arrayLayers - pCreateInfo->subresourceRange.baseArrayLayer;
            if (view_type == VK_IMAGE_VIEW_TYPE_CUBE && remaining_layers != 6) {
                skip |= LogError(device, "VUID-VkImageViewCreateInfo-viewType-02962",
                                 "vkCreateImageView(): subresourceRange.layerCount VK_REMAINING_ARRAY_LAYERS=(%d) must be 6",
                                 remaining_layers);
            }
            if (view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && ((remaining_layers) % 6) != 0) {
                skip |= LogError(
                    device, "VUID-VkImageViewCreateInfo-viewType-02963",
                    "vkCreateImageView(): subresourceRange.layerCount VK_REMAINING_ARRAY_LAYERS=(%d) must be a multiple of 6",
                    remaining_layers);
            }
            if ((remaining_layers != 1) && ((view_type == VK_IMAGE_VIEW_TYPE_1D) || (view_type == VK_IMAGE_VIEW_TYPE_2D) ||
                                            (view_type == VK_IMAGE_VIEW_TYPE_3D))) {
                skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-imageViewType-04974",
                                 "vkCreateImageView(): Using pCreateInfo->viewType %s and the subresourceRange.layerCount "
                                 "VK_REMAINING_ARRAY_LAYERS=(%d) and must 1 (try looking into VK_IMAGE_VIEW_TYPE_*_ARRAY).",
                                 string_VkImageViewType(view_type), remaining_layers);
            }
        } else {
            if ((layer_count != 1) && ((view_type == VK_IMAGE_VIEW_TYPE_1D) || (view_type == VK_IMAGE_VIEW_TYPE_2D) ||
                                       (view_type == VK_IMAGE_VIEW_TYPE_3D))) {
                skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-imageViewType-04973",
                                 "vkCreateImageView(): subresourceRange.layerCount (%" PRIu32
                                 ") must be 1 when using viewType %s (try looking into VK_IMAGE_VIEW_TYPE_*_ARRAY).",
                                 layer_count, string_VkImageViewType(view_type));
            }
        }

        if (image_usage & VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT) {
            if (normalized_subresource_range.levelCount != 1) {
                skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-02571",
                                 "vkCreateImageView(): If image was created with usage containing "
                                 "VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT, subresourceRange.levelCount (%d) must: be 1",
                                 pCreateInfo->subresourceRange.levelCount);
            }
        }
        if (pCreateInfo->flags & VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT) {
            if (!enabled_features.fragment_density_map_features.fragmentDensityMapDynamic) {
                skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-flags-02572",
                                 "vkCreateImageView(): If the fragmentDensityMapDynamic feature is not enabled, "
                                 "flags must not contain VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT");
            }
        } else {
            if (image_usage & VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT) {
                if (image_flags & (VK_IMAGE_CREATE_PROTECTED_BIT | VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                                   VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT)) {
                    skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-flags-04116",
                                     "vkCreateImageView(): If image was created with usage containing "
                                     "VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT flags must not contain any of "
                                     "VK_IMAGE_CREATE_PROTECTED_BIT, VK_IMAGE_CREATE_SPARSE_BINDING_BIT, "
                                     "VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT, or VK_IMAGE_CREATE_SPARSE_ALIASED_BIT");
                }
            }
        }

        if (image_flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) {
            if (!FormatIsCompressed(view_format)) {
                if (pCreateInfo->subresourceRange.levelCount != 1) {
                    skip |=
                        LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-07072",
                                 "vkCreateImageView(): Image was created with VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT bit, "
                                 "and format is not compressed, but subresourcesRange.levelCount (%" PRIu32 ") is not 1.",
                                 pCreateInfo->subresourceRange.levelCount);
                }
                if (pCreateInfo->subresourceRange.layerCount != 1) {
                    skip |=
                        LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-07072",
                                 "vkCreateImageView(): Image was created with VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT bit, "
                                 "and format is not compressed, but subresourcesRange.layerCount (%" PRIu32 ") is not 1.",
                                 pCreateInfo->subresourceRange.layerCount);
                }
            }

            const bool class_compatible = FormatCompatibilityClass(view_format) == FormatCompatibilityClass(image_format);
            // "uncompressed format that is size-compatible" so if compressed, same as not being compatible
            const bool size_compatible =
                FormatIsCompressed(view_format) ? false : FormatElementSize(view_format) == FormatElementSize(image_format);
            if (!class_compatible && !size_compatible) {
                skip |=
                    LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-01583",
                             "vkCreateImageView(): Image was created with VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT bit and "
                             "format (%s), but pCreateInfo->format (%s) are not compatible.",
                             string_VkFormat(image_format), string_VkFormat(view_format));
            }
        }

        if (pCreateInfo->flags & VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DEFERRED_BIT_EXT) {
            if (!enabled_features.fragment_density_map2_features.fragmentDensityMapDeferred) {
                skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-flags-03567",
                                 "vkCreateImageView(): If the fragmentDensityMapDeferred feature is not enabled, "
                                 "flags must not contain VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DEFERRED_BIT_EXT");
            }
            if (pCreateInfo->flags & VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT) {
                skip |=
                    LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-flags-03568",
                             "vkCreateImageView(): If flags contains VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DEFERRED_BIT_EXT, "
                             "flags must not contain VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT");
            }
        }
        if (IsExtEnabled(device_extensions.vk_ext_fragment_density_map2)) {
            if ((image_flags & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT) && (image_usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
                (layer_count > phys_dev_ext_props.fragment_density_map2_props.maxSubsampledArrayLayers)) {
                skip |= LogError(pCreateInfo->image, "VUID-VkImageViewCreateInfo-image-03569",
                                 "vkCreateImageView(): If image was created with flags containing "
                                 "VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT and usage containing VK_IMAGE_USAGE_SAMPLED_BIT "
                                 "subresourceRange.layerCount (%d) must: be less than or equal to maxSubsampledArrayLayers (%d)",
                                 layer_count, phys_dev_ext_props.fragment_density_map2_props.maxSubsampledArrayLayers);
            }
        }

        auto astc_decode_mode = LvlFindInChain<VkImageViewASTCDecodeModeEXT>(pCreateInfo->pNext);
        if (IsExtEnabled(device_extensions.vk_ext_astc_decode_mode) && (astc_decode_mode != nullptr)) {
            if ((enabled_features.astc_decode_features.decodeModeSharedExponent == VK_FALSE) &&
                (astc_decode_mode->decodeMode == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)) {
                skip |= LogError(device, "VUID-VkImageViewASTCDecodeModeEXT-decodeMode-02231",
                                 "vkCreateImageView(): decodeModeSharedExponent is not enabled but "
                                 "VkImageViewASTCDecodeModeEXT::decodeMode is VK_FORMAT_E5B9G9R9_UFLOAT_PACK32.");
            }
        }

        if (IsExtEnabled(device_extensions.vk_khr_portability_subset)) {
            // If swizzling is disabled, make sure it isn't used
            // NOTE: as of spec version 1.2.183, VUID 04465 states: "all elements of components _must_ be
            // VK_COMPONENT_SWIZZLE_IDENTITY."
            //       However, issue https://github.com/KhronosGroup/Vulkan-Portability/issues/27 points out that the identity can
            //       also be defined via R, G, B, A enums in the correct order.
            //       Spec change is at https://gitlab.khronos.org/vulkan/vulkan/-/merge_requests/4600
            if ((VK_FALSE == enabled_features.portability_subset_features.imageViewFormatSwizzle) &&
                !IsIdentitySwizzle(pCreateInfo->components)) {
                skip |= LogError(device, "VUID-VkImageViewCreateInfo-imageViewFormatSwizzle-04465",
                                 "vkCreateImageView (portability error): swizzle is disabled for this device.");
            }

            // Ensure ImageView's format has the same number of bits and components as Image's format if format reinterpretation is
            // disabled
            // TODO (ncesario): This is not correct for some cases (e.g., VK_FORMAT_B10G11R11_UFLOAT_PACK32 and
            // VK_FORMAT_E5B9G9R9_UFLOAT_PACK32), but requires additional information that should probably be generated from the
            // spec. See Github issue #2361.
            if ((VK_FALSE == enabled_features.portability_subset_features.imageViewFormatReinterpretation) &&
                ((FormatElementSize(pCreateInfo->format, VK_IMAGE_ASPECT_COLOR_BIT) !=
                  FormatElementSize(image_state->createInfo.format, VK_IMAGE_ASPECT_COLOR_BIT)) ||
                 (FormatComponentCount(pCreateInfo->format) != FormatComponentCount(image_state->createInfo.format)))) {
                skip |= LogError(device, "VUID-VkImageViewCreateInfo-imageViewFormatReinterpretation-04466",
                                 "vkCreateImageView (portability error): ImageView format must have"
                                 " the same number of components and bits per component as the Image's format");
            }
        }

        auto image_view_min_lod = LvlFindInChain<VkImageViewMinLodCreateInfoEXT>(pCreateInfo->pNext);
        if (image_view_min_lod) {
            if ((!enabled_features.image_view_min_lod_features.minLod) && (image_view_min_lod->minLod != 0)) {
                skip |= LogError(device, "VUID-VkImageViewMinLodCreateInfoEXT-minLod-06455",
                                 "vkCreateImageView(): VkImageViewMinLodCreateInfoEXT::minLod = %f, but the minLod feature is not "
                                 "enabled.  If the minLod feature is not enabled, minLod must be 0.0",
                                 image_view_min_lod->minLod);
            }
            auto max_level =
                static_cast<float>(pCreateInfo->subresourceRange.baseMipLevel + (pCreateInfo->subresourceRange.levelCount - 1));
            if (image_view_min_lod->minLod > max_level) {
                skip |= LogError(device, "VUID-VkImageViewMinLodCreateInfoEXT-minLod-06456",
                                 "vkCreateImageView(): minLod (%f) must be less or equal to the index of the last mipmap level "
                                 "accessible to the view (%f)",
                                 image_view_min_lod->minLod, max_level);
            }
        }

        if (image_usage & VK_IMAGE_USAGE_SAMPLED_BIT && FormatRequiresYcbcrConversionExplicitly(view_format)) {
            const auto ycbcr_conversion = LvlFindInChain<VkSamplerYcbcrConversionInfo>(pCreateInfo->pNext);
            if ((!ycbcr_conversion || ycbcr_conversion->conversion == VK_NULL_HANDLE) &&
                (image_usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
                skip |= LogError(device, "VUID-VkImageViewCreateInfo-format-06415",
                                 "vkCreateImageView(): When using VK_IMAGE_USAGE_SAMPLED_BIT, YCbCr Format %s requires a "
                                 "VkSamplerYcbcrConversion but one was not passed in the pNext chain.",
                                 string_VkFormat(view_format));
            }
        }

        if (pCreateInfo->viewType != VK_IMAGE_VIEW_TYPE_2D && pCreateInfo->viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
            VkImageUsageFlags decode_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR |
                                             VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
            if (image_usage & decode_usage) {
                skip |= LogError(device, "VUID-VkImageViewCreateInfo-image-04817",
                                 "vkCreateImageView(): View type %s is incompatible with decode usage.",
                                 string_VkImageViewType(pCreateInfo->viewType));
            }
        }

        if ((pCreateInfo->flags & VK_IMAGE_VIEW_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT) &&
            !enabled_features.descriptor_buffer_features.descriptorBufferCaptureReplay) {
            skip |= LogError(device, "VUID-VkImageViewCreateInfo-flags-08106",
                             "vkCreateImageView(): the descriptorBufferCaptureReplay device feature is disabled: Image views "
                             "cannot be created with "
                             "the VK_IMAGE_VIEW_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT.");
        }

        auto opaque_capture_descriptor_buffer = LvlFindInChain<VkOpaqueCaptureDescriptorDataCreateInfoEXT>(pCreateInfo->pNext);
        if (opaque_capture_descriptor_buffer &&
            !(pCreateInfo->flags & VK_IMAGE_VIEW_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT)) {
            skip |= LogError(device, "VUID-VkImageViewCreateInfo-pNext-08107",
                             "vkCreateImageView(): VkOpaqueCaptureDescriptorDataCreateInfoEXT is in pNext chain, but "
                             "VK_IMAGE_VIEW_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT is not set.");
        }
    }

    return skip;
}

bool CoreChecks::PreCallValidateDestroyImageView(VkDevice device, VkImageView imageView,
                                                 const VkAllocationCallbacks *pAllocator) const {
    auto image_view_state = Get<IMAGE_VIEW_STATE>(imageView);

    bool skip = false;
    if (image_view_state) {
        skip |= ValidateObjectNotInUse(image_view_state.get(), "vkDestroyImageView", "VUID-vkDestroyImageView-imageView-01026");
    }
    return skip;
}

bool CoreChecks::PreCallValidateDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) const {
    auto buffer_state = Get<BUFFER_STATE>(buffer);

    bool skip = false;
    if (buffer_state) {
        skip |= ValidateObjectNotInUse(buffer_state.get(), "vkDestroyBuffer", "VUID-vkDestroyBuffer-buffer-00922");
    }
    return skip;
}

bool CoreChecks::PreCallValidateDestroyBufferView(VkDevice device, VkBufferView bufferView,
                                                  const VkAllocationCallbacks *pAllocator) const {
    auto buffer_view_state = Get<BUFFER_VIEW_STATE>(bufferView);
    bool skip = false;
    if (buffer_view_state) {
        skip |= ValidateObjectNotInUse(buffer_view_state.get(), "vkDestroyBufferView", "VUID-vkDestroyBufferView-bufferView-00936");
    }
    return skip;
}

bool CoreChecks::PreCallValidateCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset,
                                              VkDeviceSize size, uint32_t data) const {
    bool skip = false;
    auto cb_state_ptr = GetRead<CMD_BUFFER_STATE>(commandBuffer);
    auto buffer_state = Get<BUFFER_STATE>(dstBuffer);
    if (!cb_state_ptr || !buffer_state) {
        return skip;
    }
    const CMD_BUFFER_STATE &cb_state = *cb_state_ptr;
    skip |=
        ValidateMemoryIsBoundToBuffer(commandBuffer, *buffer_state, "vkCmdFillBuffer()", "VUID-vkCmdFillBuffer-dstBuffer-00031");
    skip |= ValidateCmd(cb_state, CMD_FILLBUFFER);
    // Validate that DST buffer has correct usage flags set
    skip |=
        ValidateBufferUsageFlags(commandBuffer, *buffer_state, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                 "VUID-vkCmdFillBuffer-dstBuffer-00029", "vkCmdFillBuffer()", "VK_BUFFER_USAGE_TRANSFER_DST_BIT");

    skip |= ValidateProtectedBuffer(cb_state, *buffer_state, "vkCmdFillBuffer()", "VUID-vkCmdFillBuffer-commandBuffer-01811");
    skip |= ValidateUnprotectedBuffer(cb_state, *buffer_state, "vkCmdFillBuffer()", "VUID-vkCmdFillBuffer-commandBuffer-01812");

    if (dstOffset >= buffer_state->createInfo.size) {
        skip |= LogError(dstBuffer, "VUID-vkCmdFillBuffer-dstOffset-00024",
                         "vkCmdFillBuffer(): dstOffset (0x%" PRIxLEAST64
                         ") is not less than destination buffer (%s) size (0x%" PRIxLEAST64 ").",
                         dstOffset, report_data->FormatHandle(dstBuffer).c_str(), buffer_state->createInfo.size);
    }

    if ((size != VK_WHOLE_SIZE) && (size > (buffer_state->createInfo.size - dstOffset))) {
        skip |= LogError(dstBuffer, "VUID-vkCmdFillBuffer-size-00027",
                         "vkCmdFillBuffer(): size (0x%" PRIxLEAST64 ") is greater than dstBuffer (%s) size (0x%" PRIxLEAST64
                         ") minus dstOffset (0x%" PRIxLEAST64 ").",
                         size, report_data->FormatHandle(dstBuffer).c_str(), buffer_state->createInfo.size, dstOffset);
    }

    if (!IsExtEnabled(device_extensions.vk_khr_maintenance1)) {
        skip |= ValidateCmdQueueFlags(cb_state, "vkCmdFillBuffer()", VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
                                      "VUID-vkCmdFillBuffer-commandBuffer-00030");
    }

    return skip;
}

bool CoreChecks::PreCallValidateGetImageSubresourceLayout(VkDevice device, VkImage image, const VkImageSubresource *pSubresource,
                                                          VkSubresourceLayout *pLayout) const {
    bool skip = false;
    const VkImageAspectFlags sub_aspect = pSubresource->aspectMask;

    // The aspectMask member of pSubresource must only have a single bit set
    if (GetBitSetCount(sub_aspect) != 1) {
        skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-aspectMask-00997",
                         "vkGetImageSubresourceLayout(): VkImageSubresource.aspectMask must have exactly 1 bit set (currently %s).",
                         string_VkImageAspectFlags(sub_aspect).c_str());
    }

    auto image_entry = Get<IMAGE_STATE>(image);
    if (!image_entry) {
        return skip;
    }

    // Image must have been created with tiling equal to VK_IMAGE_TILING_LINEAR
    if (IsExtEnabled(device_extensions.vk_ext_image_drm_format_modifier)) {
        if ((image_entry->createInfo.tiling != VK_IMAGE_TILING_LINEAR) &&
            (image_entry->createInfo.tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT)) {
            skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-image-07790",
                             "vkGetImageSubresourceLayout(): Image must have tiling of VK_IMAGE_TILING_LINEAR or "
                             "VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT.");
        }
    } else {
        if (image_entry->createInfo.tiling != VK_IMAGE_TILING_LINEAR) {
            skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-image-07789",
                             "vkGetImageSubresourceLayout(): Image must have tiling of VK_IMAGE_TILING_LINEAR.");
        }
    }

    // mipLevel must be less than the mipLevels specified in VkImageCreateInfo when the image was created
    if (pSubresource->mipLevel >= image_entry->createInfo.mipLevels) {
        skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-mipLevel-01716",
                         "vkGetImageSubresourceLayout(): pSubresource.mipLevel (%d) must be less than %d.", pSubresource->mipLevel,
                         image_entry->createInfo.mipLevels);
    }

    // arrayLayer must be less than the arrayLayers specified in VkImageCreateInfo when the image was created
    if (pSubresource->arrayLayer >= image_entry->createInfo.arrayLayers) {
        skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-arrayLayer-01717",
                         "vkGetImageSubresourceLayout(): pSubresource.arrayLayer (%d) must be less than %d.",
                         pSubresource->arrayLayer, image_entry->createInfo.arrayLayers);
    }

    // subresource's aspect must be compatible with image's format.
    const VkFormat img_format = image_entry->createInfo.format;
    if (image_entry->createInfo.tiling == VK_IMAGE_TILING_LINEAR) {
        if (FormatIsMultiplane(img_format)) {
            VkImageAspectFlags allowed_flags = (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);
            const char *vuid = "VUID-vkGetImageSubresourceLayout-format-01581";  // 2-plane version
            if (FormatPlaneCount(img_format) > 2u) {
                allowed_flags |= VK_IMAGE_ASPECT_PLANE_2_BIT;
                vuid = "VUID-vkGetImageSubresourceLayout-format-01582";  // 3-plane version
            }
            if (sub_aspect != (sub_aspect & allowed_flags)) {
                skip |= LogError(image, vuid,
                                 "vkGetImageSubresourceLayout(): For multi-planar images, VkImageSubresource.aspectMask (0x%" PRIx32
                                 ") must be a single-plane specifier flag.",
                                 sub_aspect);
            }
        } else if (FormatIsColor(img_format)) {
            if (sub_aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
                skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-format-04461",
                                 "vkGetImageSubresourceLayout(): For color formats, VkImageSubresource.aspectMask must be "
                                 "VK_IMAGE_ASPECT_COLOR.");
            }
        } else if (FormatIsDepthOrStencil(img_format)) {
            if ((sub_aspect != VK_IMAGE_ASPECT_DEPTH_BIT) && (sub_aspect != VK_IMAGE_ASPECT_STENCIL_BIT)) {
            }
        }
        if (!FormatIsDepthAndStencil(img_format) && !FormatIsDepthOnly(img_format)) {
            if (sub_aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
                skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-format-04464",
                                 "vkGetImageSubresourceLayout(): Image format (%s) does not contain a depth component, "
                                 "but VkImageSubresource.aspectMask contains VK_IMAGE_ASPECT_DEPTH_BIT.",
                                 string_VkFormat(img_format));
            }
        } else {
            if ((sub_aspect & VK_IMAGE_ASPECT_DEPTH_BIT) == 0) {
                skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-format-04462",
                                 "vkGetImageSubresourceLayout(): Image format (%s) contains a depth component, "
                                 "but VkImageSubresource.aspectMask does not contain VK_IMAGE_ASPECT_DEPTH_BIT.",
                                 string_VkFormat(img_format));
            }
        }
        if (!FormatIsDepthAndStencil(img_format) && !FormatIsStencilOnly(img_format)) {
            if (sub_aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
                skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-format-04464",
                                 "vkGetImageSubresourceLayout(): Image format (%s) does not contain a stencil component, "
                                 "but VkImageSubresource.aspectMask contains VK_IMAGE_ASPECT_STENCIL_BIT.",
                                 string_VkFormat(img_format));
            }
        } else {
            if ((sub_aspect & VK_IMAGE_ASPECT_STENCIL_BIT) == 0) {
                skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-format-04463",
                                 "vkGetImageSubresourceLayout(): Image format (%s) contains a stencil component, "
                                 "but VkImageSubresource.aspectMask does not contain VK_IMAGE_ASPECT_STENCIL_BIT.",
                                 string_VkFormat(img_format));
            }
        }
    } else if (image_entry->createInfo.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        if ((sub_aspect != VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT) && (sub_aspect != VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT) &&
            (sub_aspect != VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT) && (sub_aspect != VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT)) {
            skip |= LogError(
                image, "VUID-vkGetImageSubresourceLayout-tiling-02271",
                "vkGetImageSubresourceLayout(): VkImageSubresource.aspectMask (%s) must be VK_IMAGE_ASPECT_MEMORY_PLANE_i_BIT_EXT.",
                string_VkImageAspectFlags(sub_aspect).c_str());
        } else {
            // Parameter validation should catch if this is used without VK_EXT_image_drm_format_modifier
            assert(IsExtEnabled(device_extensions.vk_ext_image_drm_format_modifier));
            VkImageDrmFormatModifierPropertiesEXT drm_format_properties = LvlInitStruct<VkImageDrmFormatModifierPropertiesEXT>();
            DispatchGetImageDrmFormatModifierPropertiesEXT(device, image, &drm_format_properties);

            auto fmt_drm_props = LvlInitStruct<VkDrmFormatModifierPropertiesListEXT>();
            auto fmt_props_2 = LvlInitStruct<VkFormatProperties2>(&fmt_drm_props);
            DispatchGetPhysicalDeviceFormatProperties2(physical_device, image_entry->createInfo.format, &fmt_props_2);
            std::vector<VkDrmFormatModifierPropertiesEXT> drm_properties{fmt_drm_props.drmFormatModifierCount};
            fmt_drm_props.pDrmFormatModifierProperties = drm_properties.data();
            DispatchGetPhysicalDeviceFormatProperties2(physical_device, image_entry->createInfo.format, &fmt_props_2);

            uint32_t max_plane_count = 0u;

            for (auto const &drm_property : drm_properties) {
                if (drm_format_properties.drmFormatModifier == drm_property.drmFormatModifier) {
                    max_plane_count = drm_property.drmFormatModifierPlaneCount;
                    break;
                }
            }

            VkImageAspectFlagBits allowed_plane_indices[] = {
                VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
                VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};

            bool is_valid = false;

            for (uint32_t i = 0u; i < max_plane_count; ++i) {
                if (sub_aspect == allowed_plane_indices[i]) {
                    is_valid = true;
                    break;
                }
            }

            if (!is_valid) {
                skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-tiling-02271",
                                 "vkGetImageSubresourceLayout(): VkImageSubresource.aspectMask (%s) must be "
                                 "VK_IMAGE_ASPECT_MEMORY_PLANE_i_BIT_EXT, with i being less than the "
                                 "VkDrmFormatModifierPropertiesEXT::drmFormatModifierPlaneCount (%" PRIu32
                                 ") associated with the image's format (%s) and "
                                 "VkImageDrmFormatModifierPropertiesEXT::drmFormatModifier (%" PRIu64 ").",
                                 string_VkImageAspectFlags(sub_aspect).c_str(), max_plane_count,
                                 string_VkFormat(image_entry->createInfo.format), drm_format_properties.drmFormatModifier);
            }
        }
    }

    if (image_entry->IsExternalAHB() && (0 == image_entry->GetBoundMemoryStates().size())) {
        skip |= LogError(image, "VUID-vkGetImageSubresourceLayout-image-01895",
                         "vkGetImageSubresourceLayout(): Attempt to query layout from an image created with "
                         "VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID handleType which has not yet been "
                         "bound to memory.");
    }

    return skip;
}

// Validates the image is allowed to be protected
bool CoreChecks::ValidateProtectedImage(const CMD_BUFFER_STATE &cb_state, const IMAGE_STATE &image_state, const char *cmd_name,
                                        const char *vuid, const char *more_message) const {
    bool skip = false;

    // if driver supports protectedNoFault the operation is valid, just has undefined values
    if ((!phys_dev_props_core11.protectedNoFault) && (cb_state.unprotected == true) && (image_state.unprotected == false)) {
        const LogObjectList objlist(cb_state.Handle(), image_state.Handle());
        skip |= LogError(objlist, vuid, "%s: command buffer %s is unprotected while image %s is a protected image.%s", cmd_name,
                         report_data->FormatHandle(cb_state.Handle()).c_str(),
                         report_data->FormatHandle(image_state.Handle()).c_str(), more_message);
    }
    return skip;
}

// Validates the image is allowed to be unprotected
bool CoreChecks::ValidateUnprotectedImage(const CMD_BUFFER_STATE &cb_state, const IMAGE_STATE &image_state, const char *cmd_name,
                                          const char *vuid, const char *more_message) const {
    bool skip = false;

    // if driver supports protectedNoFault the operation is valid, just has undefined values
    if ((!phys_dev_props_core11.protectedNoFault) && (cb_state.unprotected == false) && (image_state.unprotected == true)) {
        const LogObjectList objlist(cb_state.Handle(), image_state.Handle());
        skip |= LogError(objlist, vuid, "%s: command buffer %s is protected while image %s is an unprotected image.%s", cmd_name,
                         report_data->FormatHandle(cb_state.Handle()).c_str(),
                         report_data->FormatHandle(image_state.Handle()).c_str(), more_message);
    }
    return skip;
}

// Validates the buffer is allowed to be protected
bool CoreChecks::ValidateProtectedBuffer(const CMD_BUFFER_STATE &cb_state, const BUFFER_STATE &buffer_state, const char *cmd_name,
                                         const char *vuid, const char *more_message) const {
    bool skip = false;

    // if driver supports protectedNoFault the operation is valid, just has undefined values
    if ((!phys_dev_props_core11.protectedNoFault) && (cb_state.unprotected == true) && (buffer_state.unprotected == false)) {
        const LogObjectList objlist(cb_state.Handle(), buffer_state.Handle());
        skip |= LogError(objlist, vuid, "%s: command buffer %s is unprotected while buffer %s is a protected buffer.%s", cmd_name,
                         report_data->FormatHandle(cb_state.Handle()).c_str(),
                         report_data->FormatHandle(buffer_state.Handle()).c_str(), more_message);
    }
    return skip;
}

// Validates the buffer is allowed to be unprotected
bool CoreChecks::ValidateUnprotectedBuffer(const CMD_BUFFER_STATE &cb_state, const BUFFER_STATE &buffer_state, const char *cmd_name,
                                           const char *vuid, const char *more_message) const {
    bool skip = false;

    // if driver supports protectedNoFault the operation is valid, just has undefined values
    if ((!phys_dev_props_core11.protectedNoFault) && (cb_state.unprotected == false) && (buffer_state.unprotected == true)) {
        const LogObjectList objlist(cb_state.Handle(), buffer_state.Handle());
        skip |= LogError(objlist, vuid, "%s: command buffer %s is protected while buffer %s is an unprotected buffer.%s", cmd_name,
                         report_data->FormatHandle(cb_state.Handle()).c_str(),
                         report_data->FormatHandle(buffer_state.Handle()).c_str(), more_message);
    }
    return skip;
}
