// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/Vulkan/ShaderCache.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <type_traits>
#include <xxhash.h>

#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/LinearDiskCache.h"
#include "Common/MsgHandler.h"

#include "Core/ConfigManager.h"
#include "Core/Host.h"

#include "VideoBackends/Vulkan/FramebufferManager.h"
#include "VideoBackends/Vulkan/ShaderCompiler.h"
#include "VideoBackends/Vulkan/StreamBuffer.h"
#include "VideoBackends/Vulkan/Util.h"
#include "VideoBackends/Vulkan/VertexFormat.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/UberShaderPixel.h"
#include "VideoCommon/UberShaderVertex.h"
#include "VideoCommon/VertexLoaderManager.h"

namespace Vulkan
{
std::unique_ptr<ShaderCache> g_shader_cache;

ShaderCache::ShaderCache()
{
}

ShaderCache::~ShaderCache()
{
  DestroyPipelineCache();
  DestroyShaderCaches();
  DestroySharedShaders();
}

bool ShaderCache::Initialize()
{
  LoadShaderCaches();
  if (!CreatePipelineCache(true))
    return false;

  if (!CompileSharedShaders())
    return false;

  return true;
}

void ShaderCache::Shutdown()
{
}

static VkPipelineRasterizationStateCreateInfo
GetVulkanRasterizationState(const RasterizationState& state)
{
  static constexpr std::array<VkCullModeFlags, 4> cull_modes = {
    { VK_CULL_MODE_NONE, VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT,
    VK_CULL_MODE_FRONT_AND_BACK } };

  bool depth_clamp = g_ActiveConfig.backend_info.bSupportsDepthClamp;

  return {
    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,  // VkStructureType sType
    nullptr,               // const void*                               pNext
    0,                     // VkPipelineRasterizationStateCreateFlags   flags
    depth_clamp,           // VkBool32                                  depthClampEnable
    VK_FALSE,              // VkBool32                                  rasterizerDiscardEnable
    VK_POLYGON_MODE_FILL,  // VkPolygonMode                             polygonMode
    cull_modes[state.cullmode],  // VkCullModeFlags                           cullMode
    VK_FRONT_FACE_CLOCKWISE,     // VkFrontFace                               frontFace
    VK_FALSE,                    // VkBool32                                  depthBiasEnable
    0.0f,  // float                                     depthBiasConstantFactor
    0.0f,  // float                                     depthBiasClamp
    0.0f,  // float                                     depthBiasSlopeFactor
    1.0f   // float                                     lineWidth
  };
}

static VkPipelineMultisampleStateCreateInfo
GetVulkanMultisampleState(const MultisamplingState& state)
{
  u32 samples = std::max(state.samples.Value(), 1u);
  return {
    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,  // VkStructureType sType
    nullptr,  // const void*                              pNext
    0,        // VkPipelineMultisampleStateCreateFlags    flags
    static_cast<VkSampleCountFlagBits>(
      samples),  // VkSampleCountFlagBits                    rasterizationSamples
    state.per_sample_shading,    // VkBool32                                 sampleShadingEnable
    1.0f,                        // float                                    minSampleShading
    nullptr,                     // const VkSampleMask*                      pSampleMask;
    VK_FALSE,                    // VkBool32                                 alphaToCoverageEnable
    VK_FALSE                     // VkBool32                                 alphaToOneEnable
  };
}

static VkPipelineDepthStencilStateCreateInfo GetVulkanDepthStencilState(const DepthState& state)
{
  // Less/greater are swapped due to inverted depth.
  static constexpr std::array<VkCompareOp, 8> funcs = {
    { VK_COMPARE_OP_NEVER, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_EQUAL,
    VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_NOT_EQUAL,
    VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_ALWAYS } };

  return {
    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,  // VkStructureType sType
    nullptr,             // const void*                               pNext
    0,                   // VkPipelineDepthStencilStateCreateFlags    flags
    state.testenable,    // VkBool32                                  depthTestEnable
    state.updateenable,  // VkBool32                                  depthWriteEnable
    funcs[state.func],   // VkCompareOp                               depthCompareOp
    VK_FALSE,            // VkBool32                                  depthBoundsTestEnable
    VK_FALSE,            // VkBool32                                  stencilTestEnable
    {},                  // VkStencilOpState                          front
    {},                  // VkStencilOpState                          back
    0.0f,                // float                                     minDepthBounds
    1.0f                 // float                                     maxDepthBounds
  };
}

static VkPipelineColorBlendAttachmentState GetVulkanAttachmentBlendState(const BlendingState& state)
{
  VkPipelineColorBlendAttachmentState vk_state = {};
  vk_state.blendEnable = static_cast<VkBool32>(state.blendenable);
  vk_state.colorBlendOp = state.subtract ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
  vk_state.alphaBlendOp = state.subtractAlpha ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;

  if (state.usedualsrc && g_vulkan_context->SupportsDualSourceBlend())
  {
    static constexpr std::array<VkBlendFactor, 8> src_factors = {
      { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA } };
    static constexpr std::array<VkBlendFactor, 8> dst_factors = {
      { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA } };

    vk_state.srcColorBlendFactor = src_factors[state.srcfactor];
    vk_state.srcAlphaBlendFactor = src_factors[state.srcfactoralpha];
    vk_state.dstColorBlendFactor = dst_factors[state.dstfactor];
    vk_state.dstAlphaBlendFactor = dst_factors[state.dstfactoralpha];
  }
  else
  {
    static constexpr std::array<VkBlendFactor, 8> src_factors = {
      { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA } };

    static constexpr std::array<VkBlendFactor, 8> dst_factors = {
      { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA } };

    vk_state.srcColorBlendFactor = src_factors[state.srcfactor];
    vk_state.srcAlphaBlendFactor = src_factors[state.srcfactoralpha];
    vk_state.dstColorBlendFactor = dst_factors[state.dstfactor];
    vk_state.dstAlphaBlendFactor = dst_factors[state.dstfactoralpha];
  }

  if (state.colorupdate)
  {
    vk_state.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
  }
  else
  {
    vk_state.colorWriteMask = 0;
  }

  if (state.alphaupdate)
    vk_state.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;

  return vk_state;
}

static VkPipelineColorBlendStateCreateInfo
GetVulkanColorBlendState(const BlendingState& state,
  const VkPipelineColorBlendAttachmentState* attachments,
  uint32_t num_attachments)
{
  static constexpr std::array<VkLogicOp, 16> vk_logic_ops = {
    { VK_LOGIC_OP_CLEAR, VK_LOGIC_OP_AND, VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_COPY,
    VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_XOR, VK_LOGIC_OP_OR,
    VK_LOGIC_OP_NOR, VK_LOGIC_OP_EQUIVALENT, VK_LOGIC_OP_INVERT, VK_LOGIC_OP_OR_REVERSE,
    VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_OR_INVERTED, VK_LOGIC_OP_NAND, VK_LOGIC_OP_SET } };

  VkBool32 vk_logic_op_enable = static_cast<VkBool32>(state.logicopenable);
  if (vk_logic_op_enable && !g_vulkan_context->SupportsLogicOps())
  {
    // At the time of writing, Adreno and Mali drivers didn't support logic ops.
    // The "emulation" through blending path has been removed, so just disable it completely.
    // These drivers don't support dual-source blend either, so issues are to be expected.
    vk_logic_op_enable = VK_FALSE;
  }

  VkLogicOp vk_logic_op = vk_logic_op_enable ? vk_logic_ops[state.logicmode] : VK_LOGIC_OP_CLEAR;

  VkPipelineColorBlendStateCreateInfo vk_state = {
    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,  // VkStructureType sType
    nullptr,                  // const void*                                   pNext
    0,                        // VkPipelineColorBlendStateCreateFlags          flags
    vk_logic_op_enable,       // VkBool32                                      logicOpEnable
    vk_logic_op,              // VkLogicOp                                     logicOp
    num_attachments,          // uint32_t                                      attachmentCount
    attachments,              // const VkPipelineColorBlendAttachmentState*    pAttachments
    { 1.0f, 1.0f, 1.0f, 1.0f }  // float                                         blendConstants[4]
  };

  return vk_state;
}

VkPipeline ShaderCache::CreatePipeline(const PipelineInfo& info)
{
  // Declare descriptors for empty vertex buffers/attributes
  static const VkPipelineVertexInputStateCreateInfo empty_vertex_input_state = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,  // const void*                                pNext
      0,        // VkPipelineVertexInputStateCreateFlags       flags
      0,        // uint32_t                                    vertexBindingDescriptionCount
      nullptr,  // const VkVertexInputBindingDescription*      pVertexBindingDescriptions
      0,        // uint32_t                                    vertexAttributeDescriptionCount
      nullptr   // const VkVertexInputAttributeDescription*    pVertexAttributeDescriptions
  };

  // Vertex inputs
  const VkPipelineVertexInputStateCreateInfo& vertex_input_state =
      info.vertex_format ? info.vertex_format->GetVertexInputStateInfo() : empty_vertex_input_state;

  // Input assembly
  static constexpr std::array<VkPrimitiveTopology, 4> vk_primitive_topologies = {
    { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP } };
  VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      nullptr,                  // const void*                                pNext
      0,                        // VkPipelineInputAssemblyStateCreateFlags    flags
      vk_primitive_topologies[static_cast<u32>(info.rasterization_state.primitive.Value())],  // VkPrimitiveTopology
      VK_FALSE                  // VkBool32                                   primitiveRestartEnable
  };

  // Shaders to stages
  VkPipelineShaderStageCreateInfo shader_stages[3];
  uint32_t num_shader_stages = 0;
  if (info.vs != VK_NULL_HANDLE)
  {
    shader_stages[num_shader_stages++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                          nullptr,
                                          0,
                                          VK_SHADER_STAGE_VERTEX_BIT,
                                          info.vs,
                                          "main"};
  }
  if (info.gs != VK_NULL_HANDLE)
  {
    shader_stages[num_shader_stages++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                          nullptr,
                                          0,
                                          VK_SHADER_STAGE_GEOMETRY_BIT,
                                          info.gs,
                                          "main"};
  }
  if (info.ps != VK_NULL_HANDLE)
  {
    shader_stages[num_shader_stages++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                          nullptr,
                                          0,
                                          VK_SHADER_STAGE_FRAGMENT_BIT,
                                          info.ps,
                                          "main"};
  }

  // Fill in Vulkan descriptor structs from our state structures.
  VkPipelineRasterizationStateCreateInfo rasterization_state =
      GetVulkanRasterizationState(info.rasterization_state);
  VkPipelineMultisampleStateCreateInfo multisample_state =
      GetVulkanMultisampleState(info.multisampling_state);
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state =
      GetVulkanDepthStencilState(info.depth_state);
  VkPipelineColorBlendAttachmentState blend_attachment_state =
      GetVulkanAttachmentBlendState(info.blend_state);
  VkPipelineColorBlendStateCreateInfo blend_state =
      GetVulkanColorBlendState(info.blend_state, &blend_attachment_state, 1);

  // This viewport isn't used, but needs to be specified anyway.
  static const VkViewport viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
  static const VkRect2D scissor = {{0, 0}, {1, 1}};
  static const VkPipelineViewportStateCreateInfo viewport_state = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      nullptr,
      0,          // VkPipelineViewportStateCreateFlags    flags;
      1,          // uint32_t                              viewportCount
      &viewport,  // const VkViewport*                     pViewports
      1,          // uint32_t                              scissorCount
      &scissor    // const VkRect2D*                       pScissors
  };

  // Set viewport and scissor dynamic state so we can change it elsewhere.
  static const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                  VK_DYNAMIC_STATE_SCISSOR};
  static const VkPipelineDynamicStateCreateInfo dynamic_state = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr,
      0,                                            // VkPipelineDynamicStateCreateFlags    flags
      static_cast<u32>(ArraySize(dynamic_states)),  // uint32_t dynamicStateCount
      dynamic_states  // const VkDynamicState*                pDynamicStates
  };

  // Combine to full pipeline info structure.
  VkGraphicsPipelineCreateInfo pipeline_info = {
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      nullptr,                // VkStructureType sType
      0,                      // VkPipelineCreateFlags                            flags
      num_shader_stages,      // uint32_t                                         stageCount
      shader_stages,          // const VkPipelineShaderStageCreateInfo*           pStages
      &vertex_input_state,    // const VkPipelineVertexInputStateCreateInfo*      pVertexInputState
      &input_assembly_state,  // const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState
      nullptr,                // const VkPipelineTessellationStateCreateInfo*     pTessellationState
      &viewport_state,        // const VkPipelineViewportStateCreateInfo*         pViewportState
      &rasterization_state,  // const VkPipelineRasterizationStateCreateInfo*    pRasterizationState
      &multisample_state,    // const VkPipelineMultisampleStateCreateInfo*      pMultisampleState
      &depth_stencil_state,  // const VkPipelineDepthStencilStateCreateInfo*     pDepthStencilState
      &blend_state,          // const VkPipelineColorBlendStateCreateInfo*       pColorBlendState
      &dynamic_state,        // const VkPipelineDynamicStateCreateInfo*          pDynamicState
      info.pipeline_layout,  // VkPipelineLayout                                 layout
      info.render_pass,      // VkRenderPass                                     renderPass
      0,                     // uint32_t                                         subpass
      VK_NULL_HANDLE,        // VkPipeline                                       basePipelineHandle
      -1                     // int32_t                                          basePipelineIndex
  };

  VkPipeline pipeline;
  VkResult res = vkCreateGraphicsPipelines(g_vulkan_context->GetDevice(), m_pipeline_cache, 1,
                                           &pipeline_info, nullptr, &pipeline);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateGraphicsPipelines failed: ");
    return VK_NULL_HANDLE;
  }

  return pipeline;
}

VkPipeline ShaderCache::GetPipeline(const PipelineInfo& info)
{
  return GetPipelineWithCacheResult(info).first;
}

std::pair<VkPipeline, bool> ShaderCache::GetPipelineWithCacheResult(const PipelineInfo& info)
{
  auto iter = m_pipeline_objects.find(info);
  if (iter != m_pipeline_objects.end())
    return iter->second;

  VkPipeline pipeline = CreatePipeline(info);
  m_pipeline_objects.emplace(info, std::make_pair(pipeline, true));
  return{ pipeline, false };
}

VkPipeline ShaderCache::CreateComputePipeline(const ComputePipelineInfo& info)
{
  VkComputePipelineCreateInfo pipeline_info =
  {
    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    nullptr,
    0,
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
    nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, info.cs,
    "main", nullptr},
    info.pipeline_layout,
    VK_NULL_HANDLE,
    -1};

  VkPipeline pipeline;
  VkResult res = vkCreateComputePipelines(g_vulkan_context->GetDevice(), VK_NULL_HANDLE, 1,
                                          &pipeline_info, nullptr, &pipeline);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateComputePipelines failed: ");
    return VK_NULL_HANDLE;
  }

  return pipeline;
}

VkPipeline ShaderCache::GetComputePipeline(const ComputePipelineInfo& info)
{
  auto iter = m_compute_pipeline_objects.find(info);
  if (iter != m_compute_pipeline_objects.end())
    return iter->second;

  VkPipeline pipeline = CreateComputePipeline(info);
  m_compute_pipeline_objects.emplace(info, pipeline);
  return pipeline;
}

void ShaderCache::ClearPipelineCache()
{
  // TODO: Stop any async compiling happening.
  for (const auto& it : m_pipeline_objects)
  {
    if (it.second.first != VK_NULL_HANDLE)
      vkDestroyPipeline(g_vulkan_context->GetDevice(), it.second.first, nullptr);
  }
  m_pipeline_objects.clear();

  for (const auto& it : m_compute_pipeline_objects)
  {
    if (it.second != VK_NULL_HANDLE)
      vkDestroyPipeline(g_vulkan_context->GetDevice(), it.second, nullptr);
  }
  m_compute_pipeline_objects.clear();
}

class PipelineCacheReadCallback : public LinearDiskCacheReader<u32, u8>
{
public:
  PipelineCacheReadCallback(std::vector<u8>* data) : m_data(data) {}
  void Read(const u32& key, const u8* value, u32 value_size) override
  {
    m_data->resize(value_size);
    if (value_size > 0)
      std::memcpy(m_data->data(), value, value_size);
  }

private:
  std::vector<u8>* m_data;
};

class PipelineCacheReadIgnoreCallback : public LinearDiskCacheReader<u32, u8>
{
public:
  void Read(const u32& key, const u8* value, u32 value_size) override {}
};

bool ShaderCache::CreatePipelineCache(bool load_from_disk)
{
  // We have to keep the pipeline cache file name around since when we save it
  // we delete the old one, by which time the game's unique ID is already cleared.
  m_pipeline_cache_filename = GetDiskShaderCacheFileName(API_VULKAN, "pipeline", true, true);

  std::vector<u8> disk_data;
  if (load_from_disk)
  {
    LinearDiskCache<u32, u8> disk_cache;
    PipelineCacheReadCallback read_callback(&disk_data);
    if (disk_cache.OpenAndRead(m_pipeline_cache_filename, read_callback) != 1)
      disk_data.clear();
  }

  if (!disk_data.empty() && !ValidatePipelineCache(disk_data.data(), disk_data.size()))
  {
    // Don't use this data. In fact, we should delete it to prevent it from being used next time.
    File::Delete(m_pipeline_cache_filename);
    disk_data.clear();
  }

  VkPipelineCacheCreateInfo info = {
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,  // VkStructureType            sType
      nullptr,                                       // const void*                pNext
      0,                                             // VkPipelineCacheCreateFlags flags
      disk_data.size(),                              // size_t                     initialDataSize
      !disk_data.empty() ? disk_data.data() : nullptr,  // const void*                pInitialData
  };

  VkResult res =
    vkCreatePipelineCache(g_vulkan_context->GetDevice(), &info, nullptr, &m_pipeline_cache);
  if (res == VK_SUCCESS)
    return true;

  // Failed to create pipeline cache, try with it empty.
  LOG_VULKAN_ERROR(res, "vkCreatePipelineCache failed, trying empty cache: ");
  info.initialDataSize = 0;
  info.pInitialData = nullptr;
  res = vkCreatePipelineCache(g_vulkan_context->GetDevice(), &info, nullptr, &m_pipeline_cache);
  if (res == VK_SUCCESS)
    return true;

  LOG_VULKAN_ERROR(res, "vkCreatePipelineCache failed: ");
  return false;
}

// Based on Vulkan 1.0 specification,
// Table 9.1. Layout for pipeline cache header version VK_PIPELINE_CACHE_HEADER_VERSION_ONE
// NOTE: This data is assumed to be in little-endian format.
#pragma pack(push, 4)
struct VK_PIPELINE_CACHE_HEADER
{
  u32 header_length;
  u32 header_version;
  u32 vendor_id;
  u32 device_id;
  u8 uuid[VK_UUID_SIZE];
};
#pragma pack(pop)
// TODO: Remove the #if here when GCC 5 is a minimum build requirement.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 5
static_assert(std::has_trivial_copy_constructor<VK_PIPELINE_CACHE_HEADER>::value,
              "VK_PIPELINE_CACHE_HEADER must be trivially copyable");
#else
static_assert(std::is_trivially_copyable<VK_PIPELINE_CACHE_HEADER>::value,
              "VK_PIPELINE_CACHE_HEADER must be trivially copyable");
#endif

bool ShaderCache::ValidatePipelineCache(const u8* data, size_t data_length)
{
  if (data_length < sizeof(VK_PIPELINE_CACHE_HEADER))
  {
    ERROR_LOG(VIDEO, "Pipeline cache failed validation: Invalid header");
    return false;
  }

  VK_PIPELINE_CACHE_HEADER header;
  std::memcpy(&header, data, sizeof(header));
  if (header.header_length < sizeof(VK_PIPELINE_CACHE_HEADER))
  {
    ERROR_LOG(VIDEO, "Pipeline cache failed validation: Invalid header length");
    return false;
  }

  if (header.header_version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
  {
    ERROR_LOG(VIDEO, "Pipeline cache failed validation: Invalid header version");
    return false;
  }

  if (header.vendor_id != g_vulkan_context->GetDeviceProperties().vendorID)
  {
    ERROR_LOG(VIDEO,
              "Pipeline cache failed validation: Incorrect vendor ID (file: 0x%X, device: 0x%X)",
              header.vendor_id, g_vulkan_context->GetDeviceProperties().vendorID);
    return false;
  }

  if (header.device_id != g_vulkan_context->GetDeviceProperties().deviceID)
  {
    ERROR_LOG(VIDEO,
              "Pipeline cache failed validation: Incorrect device ID (file: 0x%X, device: 0x%X)",
              header.device_id, g_vulkan_context->GetDeviceProperties().deviceID);
    return false;
  }

  if (std::memcmp(header.uuid, g_vulkan_context->GetDeviceProperties().pipelineCacheUUID,
                  VK_UUID_SIZE) != 0)
  {
    ERROR_LOG(VIDEO, "Pipeline cache failed validation: Incorrect UUID");
    return false;
  }

  return true;
}

void ShaderCache::DestroyPipelineCache()
{
  ClearPipelineCache();
  vkDestroyPipelineCache(g_vulkan_context->GetDevice(), m_pipeline_cache, nullptr);
  m_pipeline_cache = VK_NULL_HANDLE;
}

void ShaderCache::SavePipelineCache()
{
  size_t data_size;
  VkResult res =
      vkGetPipelineCacheData(g_vulkan_context->GetDevice(), m_pipeline_cache, &data_size, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData failed: ");
    return;
  }

  std::vector<u8> data(data_size);
  res = vkGetPipelineCacheData(g_vulkan_context->GetDevice(), m_pipeline_cache, &data_size,
                               data.data());
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData failed: ");
    return;
  }

  // Delete the old cache and re-create.
  File::Delete(m_pipeline_cache_filename);

  // We write a single key of 1, with the entire pipeline cache data.
  // Not ideal, but our disk cache class does not support just writing a single blob
  // of data without specifying a key.
  LinearDiskCache<u32, u8> disk_cache;
  PipelineCacheReadIgnoreCallback callback;
  disk_cache.OpenAndRead(m_pipeline_cache_filename, callback);
  disk_cache.Append(1, data.data(), static_cast<u32>(data.size()));
  disk_cache.Close();
}

/// Cache inserter that is called back when reading from the file
template <typename Uid, typename UidHasher>
struct ShaderUsageCacheReader : public LinearDiskCacheReader<Uid, u32>
{
  ShaderUsageCacheReader(ObjectUsageProfiler<Uid, pKey_t, ShaderCache::vkShaderItem, UidHasher>* shader_map) : m_shader_map(shader_map) {}
  void Read(const Uid& key, const u32* value, u32 value_size) override
  {
    // We don't insert null modules into the shader map since creation could succeed later on.
    // e.g. we're generating bad code, but fix this in a later version, and for some reason
    // the cache is not invalidated.
    VkShaderModule module = Util::CreateShaderModule(value, value_size);
    if (module == VK_NULL_HANDLE)
      return;
    Uid item = key;
    item.ClearHASH();
    item.CalculateUIDHash();
    ShaderCache::vkShaderItem& it = m_shader_map->GetOrAdd(key);
    it.initialized.test_and_set();
    it.compiled = true;
    it.module = module;
  }

  ObjectUsageProfiler<Uid, pKey_t, ShaderCache::vkShaderItem, UidHasher>* m_shader_map;
};

template <typename Uid, typename UidHasher>
struct ShaderCacheReader : public LinearDiskCacheReader<Uid, u32>
{
  ShaderCacheReader(std::unordered_map<Uid, ShaderCache::vkShaderItem, UidHasher>& shader_map) : m_shader_map(shader_map) {}
  void Read(const Uid& key, const u32* value, u32 value_size) override
  {
    // We don't insert null modules into the shader map since creation could succeed later on.
    // e.g. we're generating bad code, but fix this in a later version, and for some reason
    // the cache is not invalidated.
    VkShaderModule module = Util::CreateShaderModule(value, value_size);
    if (module == VK_NULL_HANDLE)
      return;
    Uid item = key;
    item.ClearHASH();
    item.CalculateUIDHash();
    ShaderCache::vkShaderItem& it = m_shader_map[key];
    it.initialized.test_and_set();
    it.compiled = true;
    it.module = module;
  }

  std::unordered_map<Uid, ShaderCache::vkShaderItem, UidHasher>& m_shader_map;
};


void ShaderCache::LoadShaderCaches(bool forcecompile)
{
  pKey_t gameid = (pKey_t)GetMurmurHash3(reinterpret_cast<const u8*>(SConfig::GetInstance().GetGameID().data()), (u32)SConfig::GetInstance().GetGameID().size(), 0);
  m_vs_cache.shader_map.reset(VShaderCache::cache_type::Create(
    gameid,
    VERTEXSHADERGEN_UID_VERSION,
    "Ishiiruka.vs",
    StringFromFormat("%s.vs", SConfig::GetInstance().GetGameID().c_str())
  ));
  m_ps_cache.shader_map.reset(PShaderCache::cache_type::Create(
    gameid,
    PIXELSHADERGEN_UID_VERSION,
    "Ishiiruka.ps",
    StringFromFormat("%s.ps", SConfig::GetInstance().GetGameID().c_str())
  ));

  ShaderUsageCacheReader<VertexShaderUid, VertexShaderUid::ShaderUidHasher> vs_reader(m_vs_cache.shader_map.get());
  m_vs_cache.disk_cache.OpenAndRead(GetDiskShaderCacheFileName(API_VULKAN, "vs", true, true), vs_reader);

  ShaderUsageCacheReader<PixelShaderUid, PixelShaderUid::ShaderUidHasher> ps_reader(m_ps_cache.shader_map.get());
  m_ps_cache.disk_cache.OpenAndRead(GetDiskShaderCacheFileName(API_VULKAN, "ps", true, true), ps_reader);

  if (g_vulkan_context->SupportsGeometryShaders())
  {
    ShaderCacheReader<GeometryShaderUid, GeometryShaderUid::ShaderUidHasher> gs_reader(m_gs_cache.shader_map);
    m_gs_cache.disk_cache.OpenAndRead(GetDiskShaderCacheFileName(API_VULKAN, "gs", true, true), gs_reader);
  }

  ShaderCacheReader<UberShader::VertexUberShaderUid, UberShader::VertexUberShaderUid::ShaderUidHasher> uber_vs_reader(m_vus_cache.shader_map);
  m_vus_cache.disk_cache.OpenAndRead(
      GetDiskShaderCacheFileName(API_TYPE::API_VULKAN, "UVS", false, true), uber_vs_reader);
  ShaderCacheReader<UberShader::PixelUberShaderUid, UberShader::PixelUberShaderUid::ShaderUidHasher> uber_ps_reader(m_pus_cache.shader_map);
  m_pus_cache.disk_cache.OpenAndRead(
      GetDiskShaderCacheFileName(API_TYPE::API_VULKAN, "UPS", false, true), uber_ps_reader);
  if (g_ActiveConfig.CanPrecompileUberShaders())
  {
    CompileUberShaders();
  }
  if ((g_ActiveConfig.bCompileShaderOnStartup || forcecompile) && !g_ActiveConfig.bDisableSpecializedShaders)
  {
    CompileShaders();
  }

  SETSTAT(stats.numVertexShadersCreated, static_cast<int>(m_vs_cache.shader_map->size()));
  SETSTAT(stats.numVertexShadersAlive, static_cast<int>(m_vs_cache.shader_map->size()));
  SETSTAT(stats.numPixelShadersCreated, static_cast<int>(m_ps_cache.shader_map->size()));
  SETSTAT(stats.numPixelShadersAlive, static_cast<int>(m_ps_cache.shader_map->size()));
}

void ShaderCache::CompileUberShaders()
{
  int shader_count = 0;
  UberShader::EnumerateVertexUberShaderUids([&](const UberShader::VertexUberShaderUid& uid, size_t total) {
    vkShaderItem& it = m_vus_cache.shader_map[uid];
    if (!it.initialized.test_and_set())
    {
      CompileVertexUberShaderForUid(uid, it);
    }
    shader_count++;
    Host_UpdateProgressDialog(GetStringT("Compiling Vertex Uber shaders...").c_str(),
      static_cast<int>(shader_count), static_cast<int>(total));
  });  
  Host_UpdateProgressDialog("", -1, -1);
  shader_count = 0;
  UberShader::EnumeratePixelUberShaderUids([&](const UberShader::PixelUberShaderUid& uid, size_t total) {
    vkShaderItem& it = m_pus_cache.shader_map[uid];
    if (!it.initialized.test_and_set())
    {
      CompilePixelUberShaderForUid(uid, it);
    }
    shader_count++;
    Host_UpdateProgressDialog(GetStringT("Compiling Pixel Uber shaders...").c_str(),
      static_cast<int>(shader_count), static_cast<int>(total));    
  });
  Host_UpdateProgressDialog("", -1, -1);
}

void ShaderCache::CompileShaders()
{
  pKey_t gameid = (pKey_t)GetMurmurHash3(reinterpret_cast<const u8*>(SConfig::GetInstance().GetGameID().data()), (u32)SConfig::GetInstance().GetGameID().size(), 0);
  int shader_count = 0;
  m_vs_cache.shader_map->ForEachMostUsedByCategory(gameid,
    [&](const VertexShaderUid& uid, size_t total)
  {
    VertexShaderUid item = uid;
    item.ClearHASH();
    item.CalculateUIDHash();
    vkShaderItem& it = m_vs_cache.shader_map->GetOrAdd(item);
    if (!it.initialized.test_and_set())
    {
      CompileVertexShaderForUid(item, it);
      Host_UpdateProgressDialog(GetStringT("Compiling Vertex shaders...").c_str(),
        static_cast<int>(shader_count), static_cast<int>(total));
    }
  },
    [](vkShaderItem& entry)
  {
    return !entry.compiled;
  }
  , true);
  shader_count = 0;
  m_ps_cache.shader_map->ForEachMostUsedByCategory(gameid,
    [&](const PixelShaderUid& uid, size_t total)
  {
    PixelShaderUid item = uid;
    item.ClearHASH();
    item.CalculateUIDHash();
    vkShaderItem& it = m_ps_cache.shader_map->GetOrAdd(item);
    if (!it.initialized.test_and_set())
    {
      CompilePixelShaderForUid(item, it);
      Host_UpdateProgressDialog(GetStringT("Compiling Pixel shaders...").c_str(),
        static_cast<int>(shader_count), static_cast<int>(total));
    }
  },
    [](vkShaderItem& entry)
  {
    return !entry.compiled;
  }
  , true);

  if (g_vulkan_context->SupportsGeometryShaders())
  {
    shader_count = 0;
    EnumerateGeometryShaderUids([&](const GeometryShaderUid& uid, size_t total)
    {
      GeometryShaderUid item = uid;
      item.ClearHASH();
      item.CalculateUIDHash();
      vkShaderItem& it = m_gs_cache.shader_map[item];
      shader_count++;
      if (!it.initialized.test_and_set())
      {
        CompileGeometryShaderForUid(item, it);
        Host_UpdateProgressDialog(GetStringT("Compiling Geometry shaders...").c_str(),
          static_cast<int>(shader_count), static_cast<int>(total));
      }
    });
  }
  Host_UpdateProgressDialog("", -1, -1);
}

void ShaderCache::Reload()
{
  SavePipelineCache();
  ClearPipelineCache();
  DestroyShaderCaches();
  LoadShaderCaches(true);
  CreatePipelineCache(true);
}

template <typename T>
static void DestroyShaderUsageCache(T& cache)
{
  cache.disk_cache.Sync();
  cache.disk_cache.Close();
  cache.shader_map->ForEach([](ShaderCache::vkShaderItem& entry)
  {
    if (entry.module != VK_NULL_HANDLE)
      vkDestroyShaderModule(g_vulkan_context->GetDevice(), entry.module, nullptr);
  });
  cache.shader_map.reset();
}

template <typename T>
static void DestroyShaderCache(T& cache)
{
  cache.disk_cache.Sync();
  cache.disk_cache.Close();
  for (const auto& it : cache.shader_map)
  {
    if (it.second.module != VK_NULL_HANDLE)
      vkDestroyShaderModule(g_vulkan_context->GetDevice(), it.second.module, nullptr);
  }
  cache.shader_map.clear();
}



void ShaderCache::DestroyShaderCaches()
{
  m_vs_cache.shader_map->Persist([](VertexShaderUid &uid) {
    uid.ClearHASH();
    uid.CalculateUIDHash();
  });
  DestroyShaderUsageCache(m_vs_cache);
  m_ps_cache.shader_map->Persist([](PixelShaderUid &uid) {
    uid.ClearHASH();
    uid.CalculateUIDHash();
  });
  DestroyShaderUsageCache(m_ps_cache);

  if (g_vulkan_context->SupportsGeometryShaders())
    DestroyShaderCache(m_gs_cache);

  DestroyShaderCache(m_vus_cache);
  DestroyShaderCache(m_pus_cache);

  SETSTAT(stats.numPixelShadersCreated, 0);
  SETSTAT(stats.numPixelShadersAlive, 0);
  SETSTAT(stats.numVertexShadersCreated, 0);
  SETSTAT(stats.numVertexShadersAlive, 0);
}

void ShaderCache::CompileVertexShaderForUid(const VertexShaderUid& uid, ShaderCache::vkShaderItem& it)
{
  // Not in the cache, so compile the shader.
  ShaderCompiler::SPIRVCodeVector spv;
  VkShaderModule module = VK_NULL_HANDLE;
  ShaderCode source_code;
  GenerateVertexShaderCode(source_code, uid.GetUidData(), ShaderHostConfig::GetCurrent());
  if (ShaderCompiler::CompileVertexShader(&spv, source_code.data(),
    source_code.size()))
  {
    module = Util::CreateShaderModule(spv.data(), spv.size());

    // Append to shader cache if it created successfully.
    if (module != VK_NULL_HANDLE)
    {
      m_vs_cache.disk_cache.Append(uid, spv.data(), static_cast<u32>(spv.size()));
      INCSTAT(stats.numVertexShadersCreated);
      INCSTAT(stats.numVertexShadersAlive);
    }
  }
  it.compiled = true;
  // We still insert null entries to prevent further compilation attempts.
  it.module = module;
}

void ShaderCache::CompileVertexUberShaderForUid(const UberShader::VertexUberShaderUid& uid, ShaderCache::vkShaderItem& it)
{
  // Not in the cache, so compile the shader.
  ShaderCompiler::SPIRVCodeVector spv;
  VkShaderModule module = VK_NULL_HANDLE;
  ShaderCode source_code;
  UberShader::GenVertexShader(source_code, API_VULKAN, ShaderHostConfig::GetCurrent(), uid.GetUidData());
  if (ShaderCompiler::CompileVertexShader(&spv, source_code.data(),
    source_code.size()))
  {
    module = Util::CreateShaderModule(spv.data(), spv.size());

    // Append to shader cache if it created successfully.
    if (module != VK_NULL_HANDLE)
    {
      m_vus_cache.disk_cache.Append(uid, spv.data(), static_cast<u32>(spv.size()));
    }
  }
  it.compiled = true;
  // We still insert null entries to prevent further compilation attempts.
  it.module = module;
}

void ShaderCache::CompileGeometryShaderForUid(const GeometryShaderUid& uid, ShaderCache::vkShaderItem& it)
{
  // Not in the cache, so compile the shader.
  ShaderCompiler::SPIRVCodeVector spv;
  VkShaderModule module = VK_NULL_HANDLE;
  ShaderCode source_code;
  GenerateGeometryShaderCode(source_code, uid.GetUidData(), ShaderHostConfig::GetCurrent());
  if (ShaderCompiler::CompileGeometryShader(&spv, source_code.data(),
    source_code.size()))
  {
    module = Util::CreateShaderModule(spv.data(), spv.size());

    // Append to shader cache if it created successfully.
    if (module != VK_NULL_HANDLE)
      m_gs_cache.disk_cache.Append(uid, spv.data(), static_cast<u32>(spv.size()));
  }
  it.compiled = true;
  // We still insert null entries to prevent further compilation attempts.
  it.module = module;
}

void ShaderCache::CompilePixelShaderForUid(const PixelShaderUid& uid, ShaderCache::vkShaderItem& it)
{
  // Not in the cache, so compile the shader.
  ShaderCompiler::SPIRVCodeVector spv;
  VkShaderModule module = VK_NULL_HANDLE;
  ShaderCode source_code;
  GeneratePixelShaderCode(source_code, uid.GetUidData(), ShaderHostConfig::GetCurrent());
  if (ShaderCompiler::CompileFragmentShader(&spv, source_code.data(),
    source_code.size()))
  {
    module = Util::CreateShaderModule(spv.data(), spv.size());

    // Append to shader cache if it created successfully.
    if (module != VK_NULL_HANDLE)
    {
      m_ps_cache.disk_cache.Append(uid, spv.data(), static_cast<u32>(spv.size()));
      INCSTAT(stats.numPixelShadersCreated);
      INCSTAT(stats.numPixelShadersAlive);
    }
  }
  it.compiled = true;
  // We still insert null entries to prevent further compilation attempts.
  it.module = module;
}

void ShaderCache::CompilePixelUberShaderForUid(const UberShader::PixelUberShaderUid& uid, ShaderCache::vkShaderItem& it)
{
  // Not in the cache, so compile the shader.
  ShaderCompiler::SPIRVCodeVector spv;
  VkShaderModule module = VK_NULL_HANDLE;
  ShaderCode source_code;
  UberShader::GenPixelShader(source_code, API_VULKAN, ShaderHostConfig::GetCurrent(), uid.GetUidData());
  if (ShaderCompiler::CompileFragmentShader(&spv, source_code.data(),
    source_code.size()))
  {
    module = Util::CreateShaderModule(spv.data(), spv.size());

    // Append to shader cache if it created successfully.
    if (module != VK_NULL_HANDLE)
    {
      m_pus_cache.disk_cache.Append(uid, spv.data(), static_cast<u32>(spv.size()));
      INCSTAT(stats.numPixelShadersCreated);
      INCSTAT(stats.numPixelShadersAlive);
    }
  }
  it.compiled = true;
  // We still insert null entries to prevent further compilation attempts.
  it.module = module;
}

VkShaderModule ShaderCache::GetVertexShaderForUid(const VertexShaderUid& uid)
{
  vkShaderItem& it = m_vs_cache.shader_map->GetOrAdd(uid);
  if (it.initialized.test_and_set())
    return it.module;

  CompileVertexShaderForUid(uid, it);
  return it.module;
}

VkShaderModule ShaderCache::GetGeometryShaderForUid(const GeometryShaderUid& uid)
{
  ASSERT(g_vulkan_context->SupportsGeometryShaders());
  vkShaderItem& it = m_gs_cache.shader_map[uid];
  if (it.initialized.test_and_set())
    return it.module;

  CompileGeometryShaderForUid(uid, it);
  return it.module;
}

VkShaderModule ShaderCache::GetPixelShaderForUid(const PixelShaderUid& uid)
{
  vkShaderItem& it = m_ps_cache.shader_map->GetOrAdd(uid);
  if (it.initialized.test_and_set())
    return it.module;

  CompilePixelShaderForUid(uid, it);
  return it.module;
}

VkShaderModule ShaderCache::GetVertexUberShaderForUid(const UberShader::VertexUberShaderUid& uid)
{
  vkShaderItem& it = m_vus_cache.shader_map[uid];
  if (it.initialized.test_and_set())
    return it.module;

  CompileVertexUberShaderForUid(uid, it);
  return it.module;
}

VkShaderModule ShaderCache::GetPixelUberShaderForUid(const UberShader::PixelUberShaderUid& uid)
{
  vkShaderItem& it = m_pus_cache.shader_map[uid];
  if (it.initialized.test_and_set())
    return it.module;

  CompilePixelUberShaderForUid(uid, it);
  return it.module;
}

void ShaderCache::RecompileSharedShaders()
{
  DestroySharedShaders();
  if (!CompileSharedShaders())
    PanicAlert("Failed to recompile shared shaders.");
}

std::string ShaderCache::GetUtilityShaderHeader() const
{
  std::stringstream ss;
  if (g_ActiveConfig.iMultisamples > 1)
  {
    ss << "#define MSAA_ENABLED 1" << std::endl;
    ss << "#define MSAA_SAMPLES " << g_ActiveConfig.iMultisamples << std::endl;
    if (g_ActiveConfig.bSSAA)
      ss << "#define SSAA_ENABLED 1" << std::endl;
  }

  u32 efb_layers = (g_ActiveConfig.iStereoMode != STEREO_OFF) ? 2 : 1;
  ss << "#define EFB_LAYERS " << efb_layers << std::endl;

  return ss.str();
}

bool ShaderCache::CompileSharedShaders()
{
  static const char PASSTHROUGH_VERTEX_SHADER_SOURCE[] = R"(
    layout(location = 0) in vec4 ipos;
    layout(location = 5) in vec4 icol0;
    layout(location = 8) in vec3 itex0;

    layout(location = 0) out vec3 uv0;
    layout(location = 1) out vec4 col0;

    void main()
    {
      gl_Position = ipos;
      uv0 = itex0;
      col0 = icol0;
    }
  )";

  static const char PASSTHROUGH_GEOMETRY_SHADER_SOURCE[] = R"(
    layout(triangles) in;
    layout(triangle_strip, max_vertices = EFB_LAYERS * 3) out;

    layout(location = 0) in vec3 in_uv0[];
    layout(location = 1) in vec4 in_col0[];

    layout(location = 0) out vec3 out_uv0;
    layout(location = 1) out vec4 out_col0;

    void main()
    {
      for (int j = 0; j < EFB_LAYERS; j++)
      {
        for (int i = 0; i < 3; i++)
        {
          gl_Layer = j;
          gl_Position = gl_in[i].gl_Position;
          out_uv0 = vec3(in_uv0[i].xy, float(j));
          out_col0 = in_col0[i];
          EmitVertex();
        }
        EndPrimitive();
      }
    }
  )";

  static const char SCREEN_QUAD_VERTEX_SHADER_SOURCE[] = R"(
    layout(location = 0) out vec3 uv0;

    void main()
    {
        /*
         * id   &1    &2   clamp(*2-1)
         * 0    0,0   0,0  -1,-1      TL
         * 1    1,0   1,0  1,-1       TR
         * 2    0,2   0,1  -1,1       BL
         * 3    1,2   1,1  1,1        BR
         */
        vec2 rawpos = vec2(float(gl_VertexID & 1), clamp(float(gl_VertexID & 2), 0.0f, 1.0f));
        gl_Position = vec4(rawpos * 2.0f - 1.0f, 0.0f, 1.0f);
        uv0 = vec3(rawpos, 0.0f);
    }
  )";

  static const char SCREEN_QUAD_GEOMETRY_SHADER_SOURCE[] = R"(
    layout(triangles) in;
    layout(triangle_strip, max_vertices = EFB_LAYERS * 3) out;

    layout(location = 0) in vec3 in_uv0[];

    layout(location = 0) out vec3 out_uv0;

    void main()
    {
      for (int j = 0; j < EFB_LAYERS; j++)
      {
        for (int i = 0; i < 3; i++)
        {
          gl_Layer = j;
          gl_Position = gl_in[i].gl_Position;
          out_uv0 = vec3(in_uv0[i].xy, float(j));
          EmitVertex();
        }
        EndPrimitive();
      }
    }
  )";

  std::string header = GetUtilityShaderHeader();

  m_screen_quad_vertex_shader =
      Util::CompileAndCreateVertexShader(header + SCREEN_QUAD_VERTEX_SHADER_SOURCE);
  m_passthrough_vertex_shader =
      Util::CompileAndCreateVertexShader(header + PASSTHROUGH_VERTEX_SHADER_SOURCE);
  if (m_screen_quad_vertex_shader == VK_NULL_HANDLE ||
      m_passthrough_vertex_shader == VK_NULL_HANDLE)
  {
    return false;
  }

  if (g_ActiveConfig.iStereoMode != STEREO_OFF && g_vulkan_context->SupportsGeometryShaders())
  {
    m_screen_quad_geometry_shader =
        Util::CompileAndCreateGeometryShader(header + SCREEN_QUAD_GEOMETRY_SHADER_SOURCE);
    m_passthrough_geometry_shader =
        Util::CompileAndCreateGeometryShader(header + PASSTHROUGH_GEOMETRY_SHADER_SOURCE);
    if (m_screen_quad_geometry_shader == VK_NULL_HANDLE ||
        m_passthrough_geometry_shader == VK_NULL_HANDLE)
    {
      return false;
    }
  }

  return true;
}

void ShaderCache::DestroySharedShaders()
{
  auto DestroyShader = [this](VkShaderModule& shader) {
    if (shader != VK_NULL_HANDLE)
    {
      vkDestroyShaderModule(g_vulkan_context->GetDevice(), shader, nullptr);
      shader = VK_NULL_HANDLE;
    }
  };

  DestroyShader(m_screen_quad_vertex_shader);
  DestroyShader(m_passthrough_vertex_shader);
  DestroyShader(m_screen_quad_geometry_shader);
  DestroyShader(m_passthrough_geometry_shader);
}
}
