// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <string>

#include "Common/Align.h"
#include "Common/Common.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/FramebufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/PostProcessing.h"
#include "VideoBackends/Vulkan/Renderer.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/ShaderCompiler.h"
#include "VideoBackends/Vulkan/Texture2D.h"
#include "VideoBackends/Vulkan/VulkanContext.h"


#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Statistics.h"

namespace Vulkan
{

static const char* s_vertex_shader = R"(
out vec2 v_source_uv;
out vec2 v_target_uv;
flat out float v_layer;
void main(void)
{
vec2 rawpos = vec2(gl_VertexID&1, gl_VertexID&2);
gl_Position = vec4(rawpos*2.0-1.0, 0.0, 1.0);
v_source_uv = rawpos * u_source_rect.zw + u_source_rect.xy;
v_target_uv = rawpos;
v_layer = u_src_layer;
}
)";

static const char* s_layered_vertex_shader = R"(
out vec2 i_source_uv;
out vec2 i_target_uv;
void main(void)
{
vec2 rawpos = vec2(gl_VertexID&1, gl_VertexID&2);
gl_Position = vec4(rawpos*2.0-1.0, 0.0, 1.0);
i_source_uv = rawpos * u_source_rect.zw + u_source_rect.xy;
i_target_uv = rawpos;
}
)";

static const char* s_geometry_shader = R"(

	layout(triangles) in;
layout(triangle_strip, max_vertices = %d) out;

in vec2 i_source_uv[3];
in vec2 i_target_uv[3];
out vec2 v_source_uv;
out vec2 v_target_uv;
flat out float v_layer;

void main()
{
for (int i = 0; i < %d; i++)
{
	for (int j = 0; j < 3; j++)
	{
		gl_Position = gl_in[j].gl_Position;
		v_source_uv = i_source_uv[j];
		v_target_uv = i_target_uv[j];
		v_layer = float(i);
		gl_Layer = i;
		EmitVertex();
	}

			EndPrimitive();
}
}

)";

VulkanPostProcessingShader::VulkanPostProcessingShader()
{
  m_uniform_buffer = reinterpret_cast<uintptr_t>(new StreamBuffer(VkBufferUsageFlagBits::VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, PostProcessor::UNIFORM_BUFFER_SIZE * 1024));
}

VulkanPostProcessingShader::~VulkanPostProcessingShader()
{
  for (RenderPassData& pass : m_passes)
  {
    ReleasePassNativeResources(pass);
  }
  StreamBuffer* buffer = reinterpret_cast<StreamBuffer*>(m_uniform_buffer);
  delete buffer;
  m_uniform_buffer = 0;
}

void VulkanPostProcessingShader::ReleasePassNativeResources(RenderPassData& pass)
{
  if (pass.shader)
  {
    RenderPassVulkanData* pixel_shader = reinterpret_cast<RenderPassVulkanData*>(pass.shader);
    if (pixel_shader->m_fragment_shader != VK_NULL_HANDLE)
    {
      g_command_buffer_mgr->WaitForGPUIdle();
      g_shader_cache->ClearPipelineCache();
      vkDestroyShaderModule(g_vulkan_context->GetDevice(), pixel_shader->m_fragment_shader, nullptr);
      pixel_shader->m_fragment_shader = VK_NULL_HANDLE;
    }
    delete pixel_shader;
    pass.shader = 0;
  }
}

void VulkanPostProcessingShader::ReleaseBindingSampler(uintptr_t sampler)
{

}

uintptr_t VulkanPostProcessingShader::CreateBindingSampler(const PostProcessingShaderConfiguration::RenderPass::Input& input_config)
{
  return static_cast<uintptr_t>(input_config.filter * POST_PROCESSING_ADDRESS_MODE_COUNT + input_config.address_mode + 1);
}

bool VulkanPostProcessingShader::RecompileShaders()
{
  static const char* definitions =
    "#define API_VULKAN 1\n"
    "#define GLSL 1\n"
    "#define COLOR_BUFFER_INPUT_INDEX %i\n"
    "#define DEPTH_BUFFER_INPUT_INDEX %i\n"
    "#define PREV_OUTPUT_INPUT_INDEX %i\n";

  std::string common_source = PostProcessor::GetCommonFragmentShaderSource(API_VULKAN, m_config, 0);
  for (size_t i = 0; i < m_passes.size(); i++)
  {
    RenderPassData& pass = m_passes[i];
    const PostProcessingShaderConfiguration::RenderPass& pass_config = m_config->GetPass(i);

    int color_buffer_index = 0;
    int depth_buffer_index = 0;
    int prev_output_index = 0;

    pass_config.GetInputLocations(color_buffer_index, depth_buffer_index, prev_output_index);

    std::string hlsl_source = PostProcessor::GetPassFragmentShaderSource(API_VULKAN, m_config, &pass_config);
    ReleasePassNativeResources(pass);
    RenderPassVulkanData* pixel_shader = new RenderPassVulkanData();
    pass.shader = reinterpret_cast<uintptr_t>(pixel_shader);
    std::string fullcode = StringFromFormat(definitions, color_buffer_index, depth_buffer_index, prev_output_index) + common_source + hlsl_source;
    pixel_shader->m_fragment_shader = Util::CompileAndCreateFragmentShader(fullcode);
    if (pixel_shader->m_fragment_shader == VK_NULL_HANDLE)
    {
      ReleasePassNativeResources(pass);
      ERROR_LOG(VIDEO, "Failed to compile post-processing shader %s (pass %s)", m_config->GetShaderName().c_str(), pass_config.entry_point.c_str());
      m_ready = false;
      return false;
    }
  }
  return true;
}

void VulkanPostProcessingShader::MapAndUpdateConfigurationBuffer()
{

}

void VulkanPostProcessingShader::Draw(PostProcessor* p,
  const TargetRectangle& dst_rect, const TargetSize& dst_size, uintptr_t dst_tex,
  const TargetRectangle& src_rect, const TargetSize& src_size, uintptr_t src_tex,
  uintptr_t src_depth_tex, int src_layer, float gamma)
{
  VulkanPostProcessor* parent = static_cast<VulkanPostProcessor*>(p);
  Texture2D* dst_texture = reinterpret_cast<Texture2D*>(dst_tex);
  Texture2D* src_texture = reinterpret_cast<Texture2D*>(src_tex);
  Texture2D* src_depth_texture = reinterpret_cast<Texture2D*>(src_depth_tex);
  _dbg_assert_(VIDEO, m_ready && m_internal_size == src_size);

  // Determine whether we can skip the final copy by writing directly to the output texture, if the last pass is not scaled.
  bool skip_final_copy = !IsLastPassScaled() && (dst_texture != src_texture || !m_last_pass_uses_color_buffer) && !m_prev_frame_enabled;

  // Draw each pass.
  PostProcessor::InputTextureSizeArray input_sizes;
  TargetRectangle output_rect = {};
  TargetSize output_size;

  u32 shader_buffer_size;
  void* shader_buffer_data = m_config->UpdateConfigurationBuffer(&shader_buffer_size, true);
  if (!shader_buffer_data)
  {
    shader_buffer_data = m_config->GetConfigurationBuffer(&shader_buffer_size);
  }

  for (size_t pass_index = 0; pass_index < m_passes.size(); pass_index++)
  {
    const RenderPassData& pass = m_passes[pass_index];
    bool is_last_pass = (pass_index == m_last_pass_index);
    if (!pass.enabled)
      continue;
    // Select geometry shader based on layers
    VkShaderModule geometry_shader = VK_NULL_HANDLE;
    if (src_layer < 0 && m_internal_layers > 1)
      geometry_shader = parent->GetGeometryShader();

    UtilityShaderDraw draw(g_command_buffer_mgr->GetCurrentCommandBuffer(),
      g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_STANDARD), TextureCache::GetInstance()->GetRenderPass(),
      parent->GetVertexShader(src_layer < 0 && m_internal_layers > 1),
      geometry_shader,
      reinterpret_cast<RenderPassVulkanData*>(pass.shader)->m_fragment_shader);  

    void* psuniforms = draw.AllocatePSUniforms(shader_buffer_size);
    memcpy(
      psuniforms,
      shader_buffer_data,
      shader_buffer_size);
    draw.CommitPSUniforms(shader_buffer_size);


    // Bind inputs to pipeline
    for (size_t i = 0; i < pass.inputs.size(); i++)
    {
      const InputBinding& input = pass.inputs[i];

      Texture2D* input_texture = nullptr;

      switch (input.type)
      {
      case POST_PROCESSING_INPUT_TYPE_COLOR_BUFFER:
        input_texture = src_texture;
        input_sizes[i] = src_size;
        break;

      case POST_PROCESSING_INPUT_TYPE_DEPTH_BUFFER:
        input_texture = src_depth_texture;
        input_sizes[i] = src_size;
        break;
      case POST_PROCESSING_INPUT_TYPE_PASS_FRAME_OUTPUT:
        if (m_prev_frame_enabled)
        {
          input_texture = reinterpret_cast<Texture2D*>(GetPrevColorFrame(input.frame_index)->GetInternalObject());
          input_sizes[i] = m_prev_frame_size;
        }
        break;
      case POST_PROCESSING_INPUT_TYPE_PASS_DEPTH_FRAME_OUTPUT:
        if (m_prev_depth_enabled)
        {
          input_texture = reinterpret_cast<Texture2D*>(GetPrevDepthFrame(input.frame_index)->GetInternalObject());
          input_sizes[i] = m_prev_depth_frame_size;
        }
        break;
      default:
        HostTexture* i_texture = input.texture ? input.texture.get() : input.prev_texture;
        if (i_texture != nullptr)
        {
          input_texture = reinterpret_cast<Texture2D*>(i_texture->GetInternalObject());
          input_sizes[i] = input.size;
        }
        else
        {
          input_texture = src_texture;
          input_sizes[i] = src_size;
        }
        break;
      }
      if (input_texture)
      {
        input_texture->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        draw.SetPSSampler(i, src_texture->GetView(), g_object_cache->GetLinearSampler());
      }
      draw.SetPSSampler(i, src_texture->GetView(), parent->GetSamplerHandle(input.texture_sampler - 1));
    }

    Texture2D* dst = dst_texture;
    // If this is the last pass and we can skip the final copy, write directly to output texture.
    if (is_last_pass && skip_final_copy)
    {
      // The target rect may differ from the source.
      output_rect = dst_rect;
      output_size = dst_size;
    }
    else
    {
      output_rect = PostProcessor::ScaleTargetRectangle(API_VULKAN, src_rect, pass.output_scale);
      output_size = pass.output_size;
      dst = reinterpret_cast<Texture2D*>(pass.output_texture->GetInternalObject());
    }
    dst->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    parent->MapAndUpdateUniformBuffer(input_sizes, output_rect, output_size, src_rect, src_size, src_layer, gamma);
    void* vsuniforms = draw.AllocateVSUniforms(POST_PROCESSING_CONTANTS_BUFFER_SIZE);
    memcpy(
      vsuniforms,
      parent->GetConstatsData(),
      POST_PROCESSING_CONTANTS_BUFFER_SIZE);
    draw.CommitVSUniforms(POST_PROCESSING_CONTANTS_BUFFER_SIZE);
    VkRect2D region = {
      { dst_rect.left, dst_rect.top },
      { static_cast<u32>(dst_rect.GetWidth()), static_cast<u32>(dst_rect.GetHeight()) } };
    draw.BeginRenderPass(dst->GetFrameBuffer(), region);
    draw.SetPSSampler(0, src_texture->GetView(), g_object_cache->GetLinearSampler());
    draw.DrawQuad(dst_rect.left, dst_rect.top, dst_rect.GetWidth(), dst_rect.GetHeight(),
      src_rect.left, src_rect.top, 0, src_rect.GetWidth(), src_rect.GetHeight(),
      static_cast<int>(src_texture->GetWidth()),
      static_cast<int>(src_texture->GetHeight()));
    draw.EndRenderPass();
  }

  // Copy the last pass output to the target if not done already
  IncrementFrame();
  if (m_prev_depth_enabled && src_depth_tex)
  {
    TargetRectangle dst;
    dst.left = 0;
    dst.right = m_prev_depth_frame_size.width;
    dst.top = 0;
    dst.bottom = m_prev_depth_frame_size.height;
    parent->CopyTexture(dst, GetPrevDepthFrame(0)->GetInternalObject(), output_rect, src_depth_tex, src_size, src_layer, true, true);
  }
  if (!skip_final_copy)
  {
    RenderPassData& final_pass = m_passes[m_last_pass_index];
    if (m_prev_frame_enabled)
    {
      TargetRectangle dst;
      dst.left = 0;
      dst.right = m_prev_frame_size.width;
      dst.top = 0;
      dst.bottom = m_prev_frame_size.height;
      parent->CopyTexture(dst, GetPrevColorFrame(0)->GetInternalObject(), output_rect, final_pass.output_texture->GetInternalObject(), final_pass.output_size, src_layer, false, true);
    }
    parent->CopyTexture(dst_rect, dst_tex, output_rect, final_pass.output_texture->GetInternalObject(), final_pass.output_size, src_layer);
  }
}

VulkanPostProcessor::~VulkanPostProcessor()
{
  for (size_t i = 0; i < m_samplers.size(); i++)
  {
    if (m_samplers[i] != VK_NULL_HANDLE)
    {
      vkDestroySampler(g_vulkan_context->GetDevice(), m_samplers[i], nullptr);
      m_samplers[i] = VK_NULL_HANDLE;
    }
  }
  g_command_buffer_mgr->WaitForGPUIdle();
  g_shader_cache->ClearPipelineCache();
  if (m_vertex_shader != VK_NULL_HANDLE)
  {
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), m_vertex_shader, nullptr);
    m_vertex_shader = VK_NULL_HANDLE;
  }
  if (m_layered_vertex_shader != VK_NULL_HANDLE)
  {
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), m_layered_vertex_shader, nullptr);
    m_layered_vertex_shader = VK_NULL_HANDLE;
  }
  if (m_layered_geometry_shader != VK_NULL_HANDLE)
  {
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), m_layered_geometry_shader, nullptr);
    m_layered_geometry_shader = VK_NULL_HANDLE;
  }
}

bool VulkanPostProcessor::Initialize()
{
  // Lookup tables for samplers
  static const VkFilter Vulkan_filters[] = { VK_FILTER_NEAREST, VK_FILTER_LINEAR };
  static const VkSamplerAddressMode Vulkan_address_modes[] = { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER };
  // Create sampler objects to match posible configuration values
  VkSamplerCreateInfo create_info = {
    VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,    // VkStructureType         sType
    nullptr,                                  // const void*             pNext
    0,                                        // VkSamplerCreateFlags    flags
    VK_FILTER_NEAREST,                        // VkFilter                magFilter
    VK_FILTER_NEAREST,                        // VkFilter                minFilter
    VK_SAMPLER_MIPMAP_MODE_NEAREST,           // VkSamplerMipmapMode     mipmapMode
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,  // VkSamplerAddressMode    addressModeU
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,  // VkSamplerAddressMode    addressModeV
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,    // VkSamplerAddressMode    addressModeW
    0.0f,                                     // float                   mipLodBias
    VK_FALSE,                                 // VkBool32                anisotropyEnable
    1.0f,                                     // float                   maxAnisotropy
    VK_FALSE,                                 // VkBool32                compareEnable
    VK_COMPARE_OP_ALWAYS,                     // VkCompareOp             compareOp
    std::numeric_limits<float>::min(),        // float                   minLod
    std::numeric_limits<float>::max(),        // float                   maxLod
    VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,  // VkBorderColor           borderColor
    VK_FALSE                                  // VkBool32                unnormalizedCoordinates
  };
  for (size_t i = 0; i < 2; i++)
  {
    for (size_t j = 0; j < 3; j++)
    {
      create_info.magFilter = Vulkan_filters[i];
      create_info.minFilter = Vulkan_filters[i];
      create_info.addressModeU = Vulkan_address_modes[j];
      create_info.addressModeV = Vulkan_address_modes[j];
      VkSampler samp;
      VkResult res =
        vkCreateSampler(g_vulkan_context->GetDevice(), &create_info, nullptr, &samp);
      if (res != VK_SUCCESS)
      {
        LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");
        return false;
      }
      m_samplers.push_back(samp);
    }
  }
  // Create VS/GS
  if (!CreateCommonShaders())
    return false;
  // Load the currently-configured shader (this may fail, and that's okay)
  ReloadShaders();
  return true;
}

bool VulkanPostProcessor::CreateCommonShaders()
{
  bool result = true;
  std::string shader;
  PostProcessor::GetUniformBufferShaderSource(API_VULKAN, nullptr, shader, false);
  shader += s_vertex_shader;
  m_vertex_shader = Util::CompileAndCreateVertexShader(shader);
  result = result &&  m_vertex_shader != VK_NULL_HANDLE;
  shader.clear();
  PostProcessor::GetUniformBufferShaderSource(API_VULKAN, nullptr, shader, false);
  shader += s_layered_vertex_shader;
  m_layered_vertex_shader = Util::CompileAndCreateVertexShader(shader);
  result = result &&  m_layered_vertex_shader != VK_NULL_HANDLE;
  shader.clear();
  PostProcessor::GetUniformBufferShaderSource(API_VULKAN, nullptr, shader, false);
  shader += StringFromFormat(s_geometry_shader, 6, 2);
  m_layered_geometry_shader = Util::CompileAndCreateGeometryShader(shader);
  result = result &&  m_layered_geometry_shader != VK_NULL_HANDLE;
  return result;
}

std::unique_ptr<PostProcessingShader> VulkanPostProcessor::CreateShader(PostProcessingShaderConfiguration* config)
{
  std::unique_ptr<PostProcessingShader> shader;
  shader.reset(new VulkanPostProcessingShader());
  if (!shader->Initialize(config, g_framebuffer_manager->GetEFBLayers()))
    shader.reset();

  return shader;
}

void VulkanPostProcessor::PostProcessEFBToTexture(uintptr_t dst_texture)
{
  // Apply normal post-process process, but to the EFB buffers.
  // Uses the current viewport as the "visible" region to post-process.
  TargetRectangle target_rect = { 0, 0, g_renderer->GetTargetWidth(), g_renderer->GetTargetHeight() };
  TargetSize target_size(g_renderer->GetTargetWidth(), g_renderer->GetTargetHeight());

  // Source and target textures, if MSAA is enabled, this needs to be resolved
  Texture2D* color_texture = FramebufferManager::GetInstance()->GetResolvedEFBColorTexture();
  Texture2D* depth_texture = nullptr;
  if (m_requires_depth_buffer)
  {
    depth_texture = FramebufferManager::GetInstance()->GetEFBDepthTexture();
  }
  // Invoke post process process
  PostProcess(nullptr, nullptr, nullptr,
    target_rect, target_size, reinterpret_cast<uintptr_t>(color_texture),
    target_rect, target_size, reinterpret_cast<uintptr_t>(depth_texture), dst_texture);

  g_renderer->RestoreAPIState();
}

void VulkanPostProcessor::PostProcessEFB(const TargetRectangle& src_rect, const TargetSize& src_size)
{
  // Apply normal post-process process, but to the EFB buffers.
  // Uses the current viewport as the "visible" region to post-process.
  // In Vulkan, the viewport rectangle must fit within the render target.
  TargetRectangle target_rect;
  TargetSize target_size(src_size.width, src_size.height);
  target_rect.left = src_rect.left >= 0 ? src_rect.left : 0;
  target_rect.top = src_rect.top >= 0 ? src_rect.top : 0;
  target_rect.right = src_rect.right <= src_size.width ? src_rect.right : src_size.width;
  target_rect.bottom = src_rect.bottom <= src_size.height ? src_rect.bottom : src_size.height;

  // Source and target textures, if MSAA is enabled, this needs to be resolved
  Texture2D* color_texture = FramebufferManager::GetInstance()->GetResolvedEFBColorTexture();
  Texture2D* depth_texture = nullptr;
  if (m_requires_depth_buffer)
  {
    depth_texture = FramebufferManager::GetInstance()->GetEFBDepthTexture();
  }

  // Invoke post process process
  PostProcess(nullptr, nullptr, nullptr,
    target_rect, target_size, reinterpret_cast<uintptr_t>(color_texture),
    target_rect, target_size, reinterpret_cast<uintptr_t>(depth_texture));

  // Copy back to EFB buffer when multisampling is enabled
  if (g_ActiveConfig.iMultisamples > 1)
    CopyTexture(target_rect, reinterpret_cast<uintptr_t>(FramebufferManager::GetInstance()->GetEFBColorTexture()), target_rect, reinterpret_cast<uintptr_t>(color_texture), target_size, -1, false, true);

  g_renderer->RestoreAPIState();
}

void VulkanPostProcessor::MapAndUpdateUniformBuffer(
  const InputTextureSizeArray& input_sizes,
  const TargetRectangle& dst_rect, const TargetSize& dst_size,
  const TargetRectangle& src_rect, const TargetSize& src_size,
  int src_layer, float gamma)
{
  // Skip writing to buffer if there were no changes
  UpdateConstantUniformBuffer(input_sizes, dst_rect, dst_size, src_rect, src_size, src_layer, gamma);
}

void VulkanPostProcessor::CopyTexture(const TargetRectangle& dst_rect, uintptr_t dst_tex,
  const TargetRectangle& src_rect, uintptr_t src_tex,
  const TargetSize& src_size, int src_layer, bool is_depth_texture,
  bool force_shader_copy)
{
  Texture2D* dst_texture = reinterpret_cast<Texture2D*>(dst_tex);
  Texture2D* src_texture = reinterpret_cast<Texture2D*>(src_tex);
  // If the dimensions are the same, we can copy instead of using a shader.
  bool scaling = (dst_rect.GetWidth() != src_rect.GetWidth() || dst_rect.GetHeight() != src_rect.GetHeight());
  if (!scaling && !force_shader_copy)
  {
    VkImageCopy image_copy = {
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
      src_texture->GetLayers() },        // VkImageSubresourceLayers    srcSubresource
      { src_rect.left, src_rect.top, 0 },  // VkOffset3D                  srcOffset
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,  // VkImageSubresourceLayers    dstSubresource
      dst_texture->GetLayers() },
      { dst_rect.left, dst_rect.top, 0 },  // VkOffset3D                  dstOffset
      { static_cast<uint32_t>(src_rect.GetWidth()), static_cast<uint32_t>(src_rect.GetHeight()),
      1 }  // VkExtent3D                  extent
    };

    // Must be called outside of a render pass.
    StateTracker::GetInstance()->EndRenderPass();

    src_texture->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    dst_texture->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    vkCmdCopyImage(g_command_buffer_mgr->GetCurrentCommandBuffer(), src_texture->GetImage(),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_texture->GetImage(),
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_copy);
  }
  else
  {
    // Can't do this within a game render pass.
    StateTracker::GetInstance()->EndRenderPass();
    StateTracker::GetInstance()->SetPendingRebind();

    // Render pass expects dst_texture to be in SHADER_READ_ONLY state.
    src_texture->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    dst_texture->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    UtilityShaderDraw draw(g_command_buffer_mgr->GetCurrentCommandBuffer(),
      g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_STANDARD), TextureCache::GetInstance()->GetRenderPass(),
      g_shader_cache->GetPassthroughVertexShader(),
      g_shader_cache->GetPassthroughGeometryShader(), TextureCache::GetInstance()->GetCopyShader());

    VkRect2D region = {
      { dst_rect.left, dst_rect.top },
      { static_cast<u32>(dst_rect.GetWidth()), static_cast<u32>(dst_rect.GetHeight()) } };
    draw.BeginRenderPass(dst_texture->GetFrameBuffer(), region);
    draw.SetPSSampler(0, src_texture->GetView(), g_object_cache->GetLinearSampler());
    draw.DrawQuad(dst_rect.left, dst_rect.top, dst_rect.GetWidth(), dst_rect.GetHeight(),
      src_rect.left, src_rect.top, 0, src_rect.GetWidth(), src_rect.GetHeight(),
      static_cast<int>(src_texture->GetWidth()),
      static_cast<int>(src_texture->GetHeight()));
    draw.EndRenderPass();
  }
}

}  // namespace Vulkan