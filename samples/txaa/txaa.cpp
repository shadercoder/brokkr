/*
* Brokkr framework
*
* Copyright(c) 2017 by Ferran Sole
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files(the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions :
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include "render.h"
#include "window.h"
#include "mesh.h"
#include "maths.h"
#include "timer.h"
#include "../utility.h"
#include "transform-manager.h"
#include "packed-freelist.h"

using namespace bkk;
using namespace maths;
using namespace sample_utils;

static const char* gGeometryPassVertexShaderSource = R"(
  #version 440 core

  layout(location = 0) in vec3 aPosition;
  layout(location = 1) in vec3 aNormal;

  layout (set = 0, binding = 0) uniform SCENE
  {
  mat4 view;
  mat4 projection;
  mat4 projectionInverse;
  mat4 prevViewProjection;
  vec4 imageSize;
  }scene;

  layout(set = 1, binding = 0) uniform MODEL
  {
    mat4 transform;
  }model;

  layout(location = 0) out vec3 normalViewSpace;

  void main(void)
  {
    mat4 modelView = scene.view * model.transform;
    gl_Position =  scene.projection * modelView * vec4(aPosition,1.0);
    normalViewSpace = normalize((transpose( inverse( modelView) ) * vec4(aNormal,0.0)).xyz);
  }
)";

static const char* gGeometryPassFragmentShaderSource = R"(
  #version 440 core

  layout(set = 2, binding = 0) uniform MATERIAL
  {
    vec3 albedo;
    float metallic;
    vec3 F0;
    float roughness;
  }material;

  layout(location = 0) out vec4 RT0;
  layout(location = 1) out vec4 RT1;
  layout(location = 2) out vec4 RT2;

  layout(location = 0) in vec3 normalViewSpace;

  void main(void)
  {
    RT0 = vec4(material.albedo,  material.roughness );
    RT1 = vec4(normalize(normalViewSpace),gl_FragCoord.z);
    RT2 = vec4(material.F0, material.metallic);
  }
)";

static const char* gLightPassVertexShaderSource = R"(
  #version 440 core

  layout(location = 0) in vec3 aPosition;

  layout(set = 0, binding = 0) uniform SCENE
  {
    mat4 view;
    mat4 projection;
    mat4 projectionInverse;
    mat4 prevViewProjection;
    vec4 imageSize;
  }scene;

  layout (set = 2, binding = 0) uniform LIGHT
  {
   vec4 position;
   vec3 color;
   float radius;
  }light;

  layout(location = 0) out vec3 lightPositionVS;

  void main(void)
  {
    mat4 viewProjection =  scene.projection * scene.view;
    vec4 vertexPosition =  vec4( aPosition*light.radius+light.position.xyz, 1.0 );
    gl_Position = viewProjection * vertexPosition;
    lightPositionVS = (scene.view * light.position).xyz;
  }
)";

static const char* gLightPassFragmentShaderSource = R"(
  #version 440 core

  layout(set = 0, binding = 0) uniform SCENE
  {
    mat4 view;
    mat4 projection;
    mat4 projectionInverse;
    mat4 prevViewProjection;
    vec4 imageSize;
  }scene;

  layout (set = 2, binding = 0) uniform LIGHT
  {
   vec4 position;
   vec3 color;
   float radius;
  }light;

  layout(set = 1, binding = 0) uniform sampler2D RT0;
  layout(set = 1, binding = 1) uniform sampler2D RT1;
  layout(set = 1, binding = 2) uniform sampler2D RT2;

  layout(location = 0) in vec3 lightPositionVS;
  
  layout(location = 0) out vec4 result;
  
  const float PI = 3.14159265359;
  vec3 ViewSpacePositionFromDepth(vec2 uv, float depth)
  {
    vec3 clipSpacePosition = vec3(uv* 2.0 - 1.0, depth);
    vec4 viewSpacePosition = scene.projectionInverse * vec4(clipSpacePosition,1.0);
    return(viewSpacePosition.xyz / viewSpacePosition.w);
  }

  vec3 fresnelSchlick(float cosTheta, vec3 F0)
  {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
  }

  float DistributionGGX(vec3 N, vec3 H, float roughness)
  {
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;
    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return nom / denom;
  }

  float GeometrySchlickGGX(float NdotV, float roughness)
  {
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;
    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return nom / denom;
  }

  float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
  {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
  }

  void main(void)
  {
    vec2 uv = gl_FragCoord.xy * scene.imageSize.zw;
    vec4 RT0Value = texture(RT0, uv);
    vec3 albedo = RT0Value.xyz;
    float roughness = RT0Value.w;
    vec4 RT1Value = texture(RT1, uv);
    vec3 N = normalize(RT1Value.xyz); 
    float depth = RT1Value.w;
    vec4 RT2Value = texture(RT2, uv);
    vec3 positionVS = ViewSpacePositionFromDepth( uv,depth );
    vec3 L = normalize( lightPositionVS-positionVS );
    vec3 F0 = RT2Value.xyz;
    float metallic = RT2Value.w;
    vec3 V = -normalize(positionVS);
    vec3 H = normalize(V + L);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;
    vec3 nominator = NDF * G * F;
    float denominator = 4 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;
    vec3 specular = nominator / denominator;
    float lightDistance    = length(lightPositionVS - positionVS);
    float attenuation = 1.0 - clamp( lightDistance / light.radius, 0.0, 1.0);
    attenuation *= attenuation;
    float NdotL =  max( 0.0, dot( N, L ) );
    vec3 color = (kD * albedo / PI + specular) * (light.color*attenuation) * NdotL;
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    result = vec4(color,1.0);
  }
)";

static const char* gTxaaResolveFragmentShaderSource = R"(
  #version 440 core

  layout(location = 0) in vec2 uv;

  layout(set = 0, binding = 0) uniform SCENE
  {
    mat4 view;
    mat4 projection;
    mat4 projectionInverse;
    mat4 prevViewProjection;
    vec4 imageSize;
  }scene;

  layout (set = 0, binding = 1) uniform sampler2D uRenderedImage;
  layout (set = 0, binding = 2) uniform sampler2D  uHistoryBuffer;
  layout (set = 0, binding = 3) uniform sampler2D  uDepthAndNormals;
  layout(location = 0) out vec4 color;

  vec2 reproject(vec2 uv, float depth)
  {
    vec3 clipSpacePosition = vec3(uv* 2.0 - 1.0, depth);
    vec4 viewSpacePosition = scene.projectionInverse * vec4(clipSpacePosition,1.0);
    viewSpacePosition /= viewSpacePosition.w;
    vec4 worldSpacePos = inverse(scene.view) * viewSpacePosition;
    vec4 a = scene.prevViewProjection * vec4(worldSpacePos.xyz, 1.0);
    return vec2( ( a.x/a.w + 1.0 ) * 0.5, (a.y/a.w + 1.0) * 0.5 );
  }

  void main(void)
  {
    vec3 currentFragment = texture(uRenderedImage, uv).xyz; 
    float depth = texture(uDepthAndNormals, uv).w;
    vec2 reprojectedUv = reproject(uv, depth);
    if( depth == 0.0 || reprojectedUv.x < 0.0 || reprojectedUv.x > 1.0 || reprojectedUv.y < 0.0 || reprojectedUv.y > 1.0 )
    {
      color = vec4(currentFragment, 1.0);
      return;
    }

    vec3 nearColor0 = texture(uRenderedImage, reprojectedUv + vec2(scene.imageSize.z, 0.0)).xyz;
    vec3 nearColor1 = texture(uRenderedImage, reprojectedUv + vec2(0.0,scene.imageSize.w)).xyz;
    vec3 nearColor2 = texture(uRenderedImage, reprojectedUv + vec2(-scene.imageSize.z, 0.0)).xyz;
    vec3 nearColor3 = texture(uRenderedImage, reprojectedUv + vec2(0.0, -scene.imageSize.w)).xyz;
    vec3 minColor = min(currentFragment, min(nearColor0, min(nearColor1, min(nearColor2, nearColor3))));
    vec3 maxColor = max(currentFragment, max(nearColor0, max(nearColor1, max(nearColor2, nearColor3))));
    vec3 historyFragment = texture(uHistoryBuffer, reprojectedUv).xyz; 
    historyFragment = clamp(historyFragment, minColor, maxColor);
    color = vec4(mix(historyFragment,currentFragment, 1.0 / 8.0), 1.0);
  }
)";

static const char* gPresentationVertexShaderSource = R"(
  #version 440 core

  layout(location = 0) in vec3 aPosition;
  layout(location = 1) in vec2 aTexCoord;
  
  layout(location = 0) out vec2 uv;

  void main(void)
  {
    gl_Position = vec4(aPosition,1.0);
    uv = aTexCoord;
  }
)";

static const char* gPresentationFragmentShaderSource = R"(
  #version 440 core

  layout(location = 0) in vec2 uv;
  layout (set = 0, binding = 0) uniform sampler2D uTexture;
  
  layout(location = 0) out vec4 color;

  void main(void)
  {
    color = texture(uTexture, uv);
  }
)";

struct TXAA_sample_t : public application_t
{
  struct light_t
  {
    struct uniforms_t
    {
      maths::vec4 position_;
      maths::vec3 color_;
      float radius_;
    };

    uniforms_t uniforms_;
    render::gpu_buffer_t ubo_;
    render::descriptor_set_t descriptorSet_;
  };

  struct material_t
  {
    struct uniforms_t
    {
      vec3 albedo_;
      float metallic_;
      vec3 F0_;
      float roughness_;
    };

    uniforms_t uniforms_;
    render::gpu_buffer_t ubo_;
    render::descriptor_set_t descriptorSet_;
  };

  struct object_t
  {
    bkk::handle_t mesh_;
    bkk::handle_t material_;
    bkk::handle_t transform_;
    render::gpu_buffer_t ubo_;
    render::descriptor_set_t descriptorSet_;
  };

  struct scene_uniforms_t
  {
    mat4 viewMatrix_;
    mat4 projectionMatrix_;
    mat4 projectionInverseMatrix_;
    mat4 prevViewProjection_;
    vec4 imageSize_;
  };

  TXAA_sample_t()
    :application_t("Temporal Anti-Aliasing", 1200u, 800u, 3u),
    camera_(vec3(0.0f, 2.5f, 8.5f), vec2(0.0f, 0.0f), 1.0f, 0.01f),
    bTemporalAA_(true),
    currentFrame_(0)
  {
    render::context_t& context = getRenderContext();
    uvec2 size = getWindowSize();

    //Create allocator for uniform buffers and meshes
    render::gpuAllocatorCreate(context, 100 * 1024 * 1024, 0xFFFF, render::gpu_memory_type_e::HOST_VISIBLE_COHERENT, &allocator_);

    //Create descriptor pool
    render::descriptorPoolCreate(context, 1000u, 1000u, 1000u, 0u, 0u, &descriptorPool_);

    //Create vertex format (position + normal)
    uint32_t vertexSize = 2 * sizeof(maths::vec3);
    render::vertex_attribute_t attributes[2] = { { render::vertex_attribute_t::format::VEC3, 0, vertexSize },{ render::vertex_attribute_t::format::VEC3, sizeof(maths::vec3), vertexSize } };
    render::vertexFormatCreate(attributes, 2u, &vertexFormat_);

    //Load full-screen quad and sphere meshes
    fullScreenQuad_ = sample_utils::fullScreenQuad(context);
    mesh::createFromFile(context, "../resources/sphere.obj", mesh::EXPORT_POSITION_ONLY, nullptr, 0u, &sphereMesh_);
    
    //Create render targets 
    render::texture2DCreate(context, size.x, size.y, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &gBufferRT0_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &gBufferRT0_);
    render::texture2DCreate(context, size.x, size.y, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &gBufferRT1_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &gBufferRT1_);
    render::texture2DCreate(context, size.x, size.y, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &gBufferRT2_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &gBufferRT2_);
    render::texture2DCreate(context, size.x, size.y, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &finalImage_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &finalImage_);
    render::depthStencilBufferCreate(context, size.x, size.y, &depthStencilBuffer_);
    
    render::texture2DCreate(context, size.x, size.y, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &historyBuffer_[0]);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &historyBuffer_[0]);
    render::texture2DCreate(context, size.x, size.y, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &historyBuffer_[1]);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &historyBuffer_[1]);
    
    //Create globals uniform buffer    
    sceneUniforms_.projectionMatrix_ = computePerspectiveProjectionMatrix(1.2f, (f32)size.x / (f32)size.y, 0.1f, 100.0f);
    computeInverse(sceneUniforms_.projectionMatrix_, sceneUniforms_.projectionInverseMatrix_);
    sceneUniforms_.viewMatrix_ = camera_.view_;
    sceneUniforms_.imageSize_ = vec4((f32)size.x, (f32)size.y, 1.0f / (f32)size.x, 1.0f / (f32)size.y);
    render::gpuBufferCreate(context, render::gpu_buffer_t::usage::UNIFORM_BUFFER, (void*)&sceneUniforms_, sizeof(scene_uniforms_t), &allocator_, &globalsUbo_);

    //Create global descriptor set (Scene uniforms)   
    render::descriptor_binding_t binding = { render::descriptor_t::type::UNIFORM_BUFFER, 0, render::descriptor_t::stage::VERTEX | render::descriptor_t::stage::FRAGMENT };
    render::descriptorSetLayoutCreate(context, &binding, 1u, &globalsDescriptorSetLayout_);
    render::descriptor_t descriptor = render::getDescriptor(globalsUbo_);
    render::descriptorSetCreate(context, descriptorPool_, globalsDescriptorSetLayout_, &descriptor, &globalsDescriptorSet_);

    //Presentation descriptor set layout and pipeline layout
    binding = { bkk::render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 0, bkk::render::descriptor_t::stage::FRAGMENT };
    bkk::render::descriptorSetLayoutCreate(context, &binding, 1u, &presentationDescriptorSetLayout_);
    bkk::render::pipelineLayoutCreate(context, &presentationDescriptorSetLayout_, 1u, &presentationPipelineLayout_);

    //Presentation descriptor sets
    descriptor = bkk::render::getDescriptor(historyBuffer_[1]);
    bkk::render::descriptorSetCreate(context, descriptorPool_, presentationDescriptorSetLayout_, &descriptor, &presentationDescriptorSet_);

    //Create presentation pipeline
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::VERTEX_SHADER, gPresentationVertexShaderSource, &presentationVertexShader_);
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::FRAGMENT_SHADER, gPresentationFragmentShaderSource, &presentationFragmentShader_);
    render::graphics_pipeline_t::description_t pipelineDesc = {};
    pipelineDesc.viewPort_ = { 0.0f, 0.0f, (float)context.swapChain_.imageWidth_, (float)context.swapChain_.imageHeight_, 0.0f, 1.0f };
    pipelineDesc.scissorRect_ = { { 0,0 },{ context.swapChain_.imageWidth_,context.swapChain_.imageHeight_ } };
    pipelineDesc.blendState_.resize(1);
    pipelineDesc.blendState_[0].colorWriteMask = 0xF;
    pipelineDesc.blendState_[0].blendEnable = VK_FALSE;
    pipelineDesc.cullMode_ = VK_CULL_MODE_BACK_BIT;
    pipelineDesc.depthTestEnabled_ = false;
    pipelineDesc.depthWriteEnabled_ = false;
    pipelineDesc.vertexShader_ = presentationVertexShader_;
    pipelineDesc.fragmentShader_ = presentationFragmentShader_;
    bkk::render::graphicsPipelineCreate(context, context.swapChain_.renderPass_, 0u, fullScreenQuad_.vertexFormat_, presentationPipelineLayout_, pipelineDesc, &presentationPipeline_);
    
    initializeOffscreenPass(context, size);
    buildPresentationCommandBuffers();
  }

  bkk::handle_t addQuadMesh()
  {
    struct Vertex
    {
      float position[3];
      float normal[3];
    };

    static const Vertex vertices[] = { { { -1.0f, 0.0f, +1.0f },{ 0.0f, 1.0f, 0.0f } },
    { { +1.0f, 0.0f, +1.0f },{ 0.0f, 1.0f, 0.0f } },
    { { -1.0f, 0.0f, -1.0f },{ 0.0f, 1.0f, 0.0f } },
    { { +1.0f, 0.0f, -1.0f },{ 0.0f, 1.0f, 0.0f } }
    };

    static const uint32_t indices[] = { 0,1,2,1,3,2 };

    static render::vertex_attribute_t attributes[2];
    attributes[0] = { render::vertex_attribute_t::format::VEC3, 0, sizeof(Vertex) };
    attributes[1] = { render::vertex_attribute_t::format::VEC3, offsetof(Vertex, normal), sizeof(Vertex) };

    mesh::mesh_t mesh;
    mesh::create(getRenderContext(), indices, sizeof(indices), (const void*)vertices, sizeof(vertices), attributes, 2, &allocator_, &mesh);
    return mesh_.add(mesh);
  }

  bkk::handle_t addMesh(const char* url)
  {
    mesh::mesh_t mesh;
    mesh::createFromFile(getRenderContext(), url, mesh::EXPORT_NORMALS, &allocator_, 0u, &mesh);
    return mesh_.add(mesh);
  }

  bkk::handle_t addMaterial(const vec3& albedo, float metallic, const vec3& F0, float roughness)
  {
    render::context_t& context = getRenderContext();

    //Create uniform buffer and descriptor set
    material_t material = {};
    material.uniforms_.albedo_ = albedo;
    material.uniforms_.metallic_ = metallic;
    material.uniforms_.F0_ = F0;
    material.uniforms_.roughness_ = roughness;
    render::gpuBufferCreate(context, render::gpu_buffer_t::usage::UNIFORM_BUFFER,
                            &material.uniforms_, sizeof(material_t::uniforms_t),
                            &allocator_, &material.ubo_);

    render::descriptor_t descriptor = render::getDescriptor(material.ubo_);
    render::descriptorSetCreate(context, descriptorPool_, materialDescriptorSetLayout_, &descriptor, &material.descriptorSet_);
    return material_.add(material);
  }

  bkk::handle_t addObject(bkk::handle_t meshId, bkk::handle_t materialId, const maths::mat4& transform)
  {
    render::context_t& context = getRenderContext();

    bkk::handle_t transformId = transformManager_.createTransform(transform);

    //Create uniform buffer and descriptor set
    render::gpu_buffer_t ubo;
    render::gpuBufferCreate(context, render::gpu_buffer_t::usage::UNIFORM_BUFFER,
      nullptr, sizeof(mat4),
      &allocator_, &ubo);

    object_t object = { meshId, materialId, transformId, ubo };
    render::descriptor_t descriptor = render::getDescriptor(object.ubo_);
    render::descriptorSetCreate(context, descriptorPool_, objectDescriptorSetLayout_, &descriptor, &object.descriptorSet_);
    return object_.add(object);
  }

  bkk::handle_t addLight(const maths::vec3& position, float radius, const maths::vec3& color)
  {
    render::context_t& context = getRenderContext();

    light_t light;

    light.uniforms_.position_ = maths::vec4(position, 1.0);
    light.uniforms_.color_ = color;
    light.uniforms_.radius_ = radius;

    //Create uniform buffer and descriptor set
    render::gpuBufferCreate(context, render::gpu_buffer_t::usage::UNIFORM_BUFFER,
                            &light.uniforms_, sizeof(light_t::uniforms_t),
                            &allocator_, &light.ubo_);

    render::descriptor_t descriptor = render::getDescriptor(light.ubo_);
    render::descriptorSetCreate(context, descriptorPool_, lightDescriptorSetLayout_, &descriptor, &light.descriptorSet_);
    return light_.add(light);
  }

  void onResize(uint32_t width, uint32_t height)
  {
    buildPresentationCommandBuffers();
  }

  void render()
  {
    render::context_t& context = getRenderContext();
    transformManager_.update();

    uvec2 windowSize = getWindowSize();
    sceneUniforms_.projectionMatrix_ = computePerspectiveProjectionMatrix(1.2f, (f32)windowSize.x / (f32)windowSize.y, 0.1f, 100.0f);
    computeInverse(sceneUniforms_.projectionMatrix_, sceneUniforms_.projectionInverseMatrix_);

    //Update global matrices
    sceneUniforms_.prevViewProjection_ = sceneUniforms_.viewMatrix_ * sceneUniforms_.projectionMatrix_;
    sceneUniforms_.viewMatrix_ = camera_.view_;
    
    if (bTemporalAA_)
    {
      static const vec2 sampleLocations[8] = { vec2(-7.0f,1.0f) / 8.0f, vec2(-5.0f,-5.0f)/ 8.0f, vec2(-1.0f,-3.0f) / 8.0f, vec2(3.0f, -7.0f) / 8.0f,
                                               vec2(5.0f,-1.0f) / 8.0f, vec2(7.0f, 7.0f) / 8.0f, vec2(1.0f,3.0f)   / 8.0f, vec2(-3.0f, 5.0f) / 8.0f };

      vec2 texelSize(sceneUniforms_.imageSize_.z, sceneUniforms_.imageSize_.w);
      vec2 subsampleOffset = sampleLocations[currentFrame_ % 8] * texelSize;

      sceneUniforms_.projectionMatrix_[8] = subsampleOffset.x;
      sceneUniforms_.projectionMatrix_[9] = subsampleOffset.y;
    }
    
    render::gpuBufferUpdate(context, (void*)&sceneUniforms_, 0u, sizeof(scene_uniforms_t), &globalsUbo_);

    //Update modelview matrices
    std::vector<object_t>& object(object_.getData());
    for (u32 i(0); i < object.size(); ++i)
    {
      render::gpuBufferUpdate(context, transformManager_.getWorldMatrix(object[i].transform_), 0, sizeof(mat4), &object[i].ubo_);
    }

    //Update lights position
    std::vector<light_t>& light(light_.getData());
    for (u32 i(0); i < light.size(); ++i)
    {
      render::gpuBufferUpdate(context, &light[i].uniforms_.position_, 0, sizeof(vec4), &light[i].ubo_);
    }

    buildAndSubmitCommandBuffer();
    render::presentFrame(&context, &copyComplete_, 1u);

    currentFrame_++;
  }

  virtual void onKeyEvent(window::key_e key, bool pressed)
  {
    if (pressed)
    {
      switch (key)
      {
      case window::key_e::KEY_UP:
      case 'w':
      {
        camera_.Move(0.0f, -0.5f);
        break;
      }
      case window::key_e::KEY_DOWN:
      case 's':
      {
        camera_.Move(0.0f, 0.5f);
        break;
      }
      case window::key_e::KEY_LEFT:
      case 'a':
      {
        camera_.Move(-0.5f, 0.0f);
        break;
      }
      case window::key_e::KEY_RIGHT:
      case 'd':
      {
        camera_.Move(0.5f, 0.0f);
        break;
      }
      case window::key_e::KEY_P:
      {
        bTemporalAA_ = !bTemporalAA_;
        break;
      }
      default:
        break;
      }
    }
  }

  void onMouseMove(const vec2& mousePos, const vec2& mouseDeltaPos, bool buttonPressed)
  {
    if (buttonPressed)
    {
      camera_.Rotate(mouseDeltaPos.x, mouseDeltaPos.y);
    }
  }

  void onQuit()
  {

    render::context_t& context = getRenderContext();

    //Destroy meshes
    packed_freelist_iterator_t<mesh::mesh_t> meshIter = mesh_.begin();
    while (meshIter != mesh_.end())
    {
      mesh::destroy(context, &meshIter.get(), &allocator_);
      ++meshIter;
    }

    //Destroy material resources
    packed_freelist_iterator_t<material_t> materialIter = material_.begin();
    while (materialIter != material_.end())
    {
      render::gpuBufferDestroy(context, &allocator_, &materialIter.get().ubo_);
      render::descriptorSetDestroy(context, &materialIter.get().descriptorSet_);
      ++materialIter;
    }

    //Destroy object resources
    packed_freelist_iterator_t<object_t> objectIter = object_.begin();
    while (objectIter != object_.end())
    {
      render::gpuBufferDestroy(context, &allocator_, &objectIter.get().ubo_);
      render::descriptorSetDestroy(context, &objectIter.get().descriptorSet_);
      ++objectIter;
    }

    //Destroy lights resources
    packed_freelist_iterator_t<light_t> lightIter = light_.begin();
    while (lightIter != light_.end())
    {
      render::gpuBufferDestroy(context, &allocator_, &lightIter.get().ubo_);
      render::descriptorSetDestroy(context, &lightIter.get().descriptorSet_);
      ++lightIter;
    }


    render::shaderDestroy(context, &gBuffervertexShader_);
    render::shaderDestroy(context, &gBufferfragmentShader_);
    render::shaderDestroy(context, &lightVertexShader_);
    render::shaderDestroy(context, &lightFragmentShader_);
    render::shaderDestroy(context, &txaaResolveFragmentShader_);
    render::shaderDestroy(context, &presentationVertexShader_);
    render::shaderDestroy(context, &presentationFragmentShader_);

    render::graphicsPipelineDestroy(context, &gBufferPipeline_);
    render::graphicsPipelineDestroy(context, &lightPipeline_);
    render::graphicsPipelineDestroy(context, &presentationPipeline_);

    render::pipelineLayoutDestroy(context, &presentationPipelineLayout_);
    render::pipelineLayoutDestroy(context, &gBufferPipelineLayout_);
    render::pipelineLayoutDestroy(context, &lightPipelineLayout_);

    render::descriptorSetDestroy(context, &globalsDescriptorSet_);
    render::descriptorSetDestroy(context, &lightPassTexturesDescriptorSet_);
    render::descriptorSetDestroy(context, &presentationDescriptorSet_);
    render::descriptorSetDestroy(context, &copyDescriptorSet_[0]);
    render::descriptorSetDestroy(context, &copyDescriptorSet_[1]);
    render::descriptorSetDestroy(context, &txaaResolveDescriptorSet_);


    render::descriptorSetLayoutDestroy(context, &globalsDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(context, &materialDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(context, &objectDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(context, &lightDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(context, &lightPassTexturesDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(context, &presentationDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(context, &txaaResolveDescriptorSetLayout_);

    render::textureDestroy(context, &gBufferRT0_);
    render::textureDestroy(context, &gBufferRT1_);
    render::textureDestroy(context, &gBufferRT2_);
    render::textureDestroy(context, &finalImage_);
    render::depthStencilBufferDestroy(context, &depthStencilBuffer_);

    mesh::destroy(context, &fullScreenQuad_);
    mesh::destroy(context, &sphereMesh_);

    render::frameBufferDestroy(context, &frameBuffer_);
    render::commandBufferDestroy(context, &commandBuffer_);
    render::renderPassDestroy(context, &renderPass_);
    render::vertexFormatDestroy(&vertexFormat_);
    render::gpuBufferDestroy(context, &allocator_, &globalsUbo_);
    render::gpuAllocatorDestroy(context, &allocator_);
    render::descriptorPoolDestroy(context, &descriptorPool_);
    vkDestroySemaphore(context.device_, renderComplete_, nullptr);
    vkDestroySemaphore(context.device_, txaaResolveComplete_, nullptr);
    vkDestroySemaphore(context.device_, copyComplete_, nullptr);

    render::textureDestroy(context, &historyBuffer_[0]);
    render::textureDestroy(context, &historyBuffer_[1]);
    render::frameBufferDestroy(context, &copyFrameBuffer_);
    render::frameBufferDestroy(context, &txaaResolveFrameBuffer_);
    render::renderPassDestroy(context, &copyRenderPass_);
    render::renderPassDestroy(context, &txaaResolveRenderPass_);
        
    render::pipelineLayoutDestroy(context, &txaaResolvePipelineLayout_);
    render::graphicsPipelineDestroy(context, &txaaResolvePipeline_);
    render::graphicsPipelineDestroy(context, &copyPipeline_);
    render::commandBufferDestroy(context, &txaaResolveCommandBuffer_);
    render::commandBufferDestroy(context, &copyCommandBuffer_);    
  }

private:
  ///Helper methods
  void initializeOffscreenPass(render::context_t& context, const uvec2& size)
  {
    //Semaphore to indicate rendering has completed
    VkSemaphoreCreateInfo semaphoreCreateInfo = {};
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(context.device_, &semaphoreCreateInfo, nullptr, &renderComplete_);
    vkCreateSemaphore(context.device_, &semaphoreCreateInfo, nullptr, &txaaResolveComplete_);
    vkCreateSemaphore(context.device_, &semaphoreCreateInfo, nullptr, &copyComplete_);

    //Create offscreen render pass (GBuffer + light subpasses)
    renderPass_ = {};
    render::render_pass_t::attachment_t attachments[5];
    attachments[0].format_ = gBufferRT0_.format_;
    attachments[0].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].samples_ = VK_SAMPLE_COUNT_1_BIT;

    attachments[1].format_ = gBufferRT1_.format_;
    attachments[1].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].samples_ = VK_SAMPLE_COUNT_1_BIT;

    attachments[2].format_ = gBufferRT2_.format_;
    attachments[2].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[2].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[2].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[2].samples_ = VK_SAMPLE_COUNT_1_BIT;

    attachments[3].format_ = finalImage_.format_;
    attachments[3].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[3].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[3].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[3].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[3].samples_ = VK_SAMPLE_COUNT_1_BIT;

    attachments[4].format_ = depthStencilBuffer_.format_;
    attachments[4].initialLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[4].finallLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[4].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[4].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[4].samples_ = VK_SAMPLE_COUNT_1_BIT;

    render::render_pass_t::subpass_t subpasses[2];
    subpasses[0].colorAttachmentIndex_.push_back(0);
    subpasses[0].colorAttachmentIndex_.push_back(1);
    subpasses[0].colorAttachmentIndex_.push_back(2);
    subpasses[0].depthStencilAttachmentIndex_ = 4;

    subpasses[1].inputAttachmentIndex_.push_back(0);
    subpasses[1].inputAttachmentIndex_.push_back(1);
    subpasses[1].inputAttachmentIndex_.push_back(2);
    subpasses[1].colorAttachmentIndex_.push_back(3);

    //Dependency chain for layout transitions
    render::render_pass_t::subpass_dependency_t dependencies[4];
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    dependencies[1].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].dstSubpass = 1;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    dependencies[2].srcSubpass = 0;
    dependencies[2].dstSubpass = 1;
    dependencies[2].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[2].dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    dependencies[3].srcSubpass = 1;
    dependencies[3].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[3].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[3].dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[3].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[3].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    render::renderPassCreate(context, attachments, 5u, subpasses, 2u, dependencies, 4u, &renderPass_);

    //Create frame buffer
    VkImageView fbAttachment[5] = { gBufferRT0_.imageView_, gBufferRT1_.imageView_, gBufferRT2_.imageView_, finalImage_.imageView_, depthStencilBuffer_.imageView_ };
    render::frameBufferCreate(context, size.x, size.y, renderPass_, fbAttachment, &frameBuffer_);

    //Create descriptorSets layouts
    render::descriptor_binding_t binding = { render::descriptor_t::type::UNIFORM_BUFFER, 0u, render::descriptor_t::stage::VERTEX };
    render::descriptorSetLayoutCreate(context, &binding, 1u, &objectDescriptorSetLayout_);

    binding = { render::descriptor_t::type::UNIFORM_BUFFER, 0u, render::descriptor_t::stage::FRAGMENT };
    render::descriptorSetLayoutCreate(context, &binding, 1u, &materialDescriptorSetLayout_);

    //Create gBuffer pipeline layout
    render::descriptor_set_layout_t descriptorSetLayouts[3] = { globalsDescriptorSetLayout_, objectDescriptorSetLayout_, materialDescriptorSetLayout_ };
    render::pipelineLayoutCreate(context, descriptorSetLayouts, 3u, &gBufferPipelineLayout_);

    //Create geometry pass pipeline
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::VERTEX_SHADER, gGeometryPassVertexShaderSource, &gBuffervertexShader_);
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::FRAGMENT_SHADER, gGeometryPassFragmentShaderSource, &gBufferfragmentShader_);
    bkk::render::graphics_pipeline_t::description_t pipelineDesc = {};
    pipelineDesc.viewPort_ = { 0.0f, 0.0f, (float)context.swapChain_.imageWidth_, (float)context.swapChain_.imageHeight_, 0.0f, 1.0f };
    pipelineDesc.scissorRect_ = { { 0,0 },{ context.swapChain_.imageWidth_,context.swapChain_.imageHeight_ } };
    pipelineDesc.blendState_.resize(3);
    pipelineDesc.blendState_[0].colorWriteMask = 0xF;
    pipelineDesc.blendState_[0].blendEnable = VK_FALSE;
    pipelineDesc.blendState_[1].colorWriteMask = 0xF;
    pipelineDesc.blendState_[1].blendEnable = VK_FALSE;
    pipelineDesc.blendState_[2].colorWriteMask = 0xF;
    pipelineDesc.blendState_[2].blendEnable = VK_FALSE;
    pipelineDesc.cullMode_ = VK_CULL_MODE_BACK_BIT;
    pipelineDesc.depthTestEnabled_ = true;
    pipelineDesc.depthWriteEnabled_ = true;
    pipelineDesc.depthTestFunction_ = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipelineDesc.vertexShader_ = gBuffervertexShader_;
    pipelineDesc.fragmentShader_ = gBufferfragmentShader_;
    render::graphicsPipelineCreate(context, renderPass_.handle_, 0u, vertexFormat_, gBufferPipelineLayout_, pipelineDesc, &gBufferPipeline_);

    //Create light pass descriptorSet layouts
    render::descriptor_binding_t bindings[3];
    bindings[0] = { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 0, render::descriptor_t::stage::FRAGMENT };
    bindings[1] = { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 1, render::descriptor_t::stage::FRAGMENT };
    bindings[2] = { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 2, render::descriptor_t::stage::FRAGMENT };
    render::descriptorSetLayoutCreate(context, bindings, 3u, &lightPassTexturesDescriptorSetLayout_);

    binding = { render::descriptor_t::type::UNIFORM_BUFFER, 0, render::descriptor_t::stage::VERTEX | render::descriptor_t::stage::FRAGMENT };
    render::descriptorSetLayoutCreate(context, &binding, 1u, &lightDescriptorSetLayout_);

    //Create descriptor sets for light pass (GBuffer textures)
    render::descriptor_t descriptors[3];
    descriptors[0] = render::getDescriptor(gBufferRT0_);
    descriptors[1] = render::getDescriptor(gBufferRT1_);
    descriptors[2] = render::getDescriptor(gBufferRT2_);
    render::descriptorSetCreate(context, descriptorPool_, lightPassTexturesDescriptorSetLayout_, descriptors, &lightPassTexturesDescriptorSet_);

    //Create light pass pipeline layout
    render::descriptor_set_layout_t lightPassDescriptorSetLayouts[3] = { globalsDescriptorSetLayout_, lightPassTexturesDescriptorSetLayout_, lightDescriptorSetLayout_ };
    render::pipelineLayoutCreate(context, lightPassDescriptorSetLayouts, 3u, &lightPipelineLayout_);

    //Create light pass pipeline
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::VERTEX_SHADER, gLightPassVertexShaderSource, &lightVertexShader_);
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::FRAGMENT_SHADER, gLightPassFragmentShaderSource, &lightFragmentShader_);
    bkk::render::graphics_pipeline_t::description_t lightPipelineDesc = {};
    lightPipelineDesc.viewPort_ = { 0.0f, 0.0f, (float)context.swapChain_.imageWidth_, (float)context.swapChain_.imageHeight_, 0.0f, 1.0f };
    lightPipelineDesc.scissorRect_ = { { 0,0 },{ context.swapChain_.imageWidth_,context.swapChain_.imageHeight_ } };
    lightPipelineDesc.blendState_.resize(1);
    lightPipelineDesc.blendState_[0].colorWriteMask = 0xF;
    lightPipelineDesc.blendState_[0].blendEnable = VK_TRUE;
    lightPipelineDesc.blendState_[0].colorBlendOp = VK_BLEND_OP_ADD;
    lightPipelineDesc.blendState_[0].alphaBlendOp = VK_BLEND_OP_ADD;
    lightPipelineDesc.blendState_[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    lightPipelineDesc.blendState_[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    lightPipelineDesc.blendState_[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    lightPipelineDesc.blendState_[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    lightPipelineDesc.cullMode_ = VK_CULL_MODE_FRONT_BIT;
    lightPipelineDesc.depthTestEnabled_ = false;
    lightPipelineDesc.depthWriteEnabled_ = false;
    lightPipelineDesc.vertexShader_ = lightVertexShader_;
    lightPipelineDesc.fragmentShader_ = lightFragmentShader_;
    render::graphicsPipelineCreate(context, renderPass_.handle_, 1u, sphereMesh_.vertexFormat_, lightPipelineLayout_, lightPipelineDesc, &lightPipeline_);
    
    //txaaResolve render pass 
    {
      txaaResolveRenderPass_ = {};
      render::render_pass_t::attachment_t attachment;
      attachment.format_ = historyBuffer_[0].format_;
      attachment.initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachment.finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachment.storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.samples_ = VK_SAMPLE_COUNT_1_BIT;

      render::renderPassCreate(context, &attachment, 1u, nullptr, 0u, nullptr, 0u, &txaaResolveRenderPass_);

      VkImageView fbAttachment = historyBuffer_[0].imageView_;
      render::frameBufferCreate(context, size.x, size.y, txaaResolveRenderPass_, &fbAttachment, &txaaResolveFrameBuffer_);


      render::descriptor_binding_t bindings[4] = { { render::descriptor_t::type::UNIFORM_BUFFER, 0,  render::descriptor_t::stage::FRAGMENT },
                                                   { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 1,  render::descriptor_t::stage::FRAGMENT },
                                                   { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 2, render::descriptor_t::stage::FRAGMENT },
                                                   { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 3, render::descriptor_t::stage::FRAGMENT } };

      render::descriptorSetLayoutCreate(context, bindings, 4, &txaaResolveDescriptorSetLayout_);
      render::pipelineLayoutCreate(context, &txaaResolveDescriptorSetLayout_, 1u, &txaaResolvePipelineLayout_);
      render::shaderCreateFromGLSLSource(context, render::shader_t::FRAGMENT_SHADER, gTxaaResolveFragmentShaderSource, &txaaResolveFragmentShader_);
      render::graphics_pipeline_t::description_t pipelineDesc = {};
      pipelineDesc.viewPort_ = { 0.0f, 0.0f, (float)context.swapChain_.imageWidth_, (float)context.swapChain_.imageHeight_, 0.0f, 1.0f };
      pipelineDesc.scissorRect_ = { { 0,0 },{ context.swapChain_.imageWidth_,context.swapChain_.imageHeight_ } };
      pipelineDesc.blendState_.resize(1);
      pipelineDesc.blendState_[0].colorWriteMask = 0xF;
      pipelineDesc.blendState_[0].blendEnable = VK_FALSE;
      pipelineDesc.cullMode_ = VK_CULL_MODE_BACK_BIT;
      pipelineDesc.depthTestEnabled_ = false;
      pipelineDesc.depthWriteEnabled_ = false;
      pipelineDesc.vertexShader_ = presentationVertexShader_;
      pipelineDesc.fragmentShader_ = txaaResolveFragmentShader_;
      render::graphicsPipelineCreate(context, txaaResolveRenderPass_.handle_, 0u, fullScreenQuad_.vertexFormat_, txaaResolvePipelineLayout_, pipelineDesc, &txaaResolvePipeline_);

      render::descriptor_t descriptors[4] = { render::getDescriptor(globalsUbo_), render::getDescriptor(finalImage_), render::getDescriptor(historyBuffer_[1]), render::getDescriptor(gBufferRT1_) };
      render::descriptorSetCreate(context, descriptorPool_, txaaResolveDescriptorSetLayout_, descriptors, &txaaResolveDescriptorSet_);
    }

    //Copy render pass
    {
      copyRenderPass_ = {};
      render::render_pass_t::attachment_t attachment;
      attachment.format_ = historyBuffer_[0].format_;
      attachment.initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachment.finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachment.storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.samples_ = VK_SAMPLE_COUNT_1_BIT;

      render::renderPassCreate(context, &attachment, 1u, nullptr, 0u, nullptr, 0u, &copyRenderPass_);

      VkImageView fbAttachment = historyBuffer_[1].imageView_;
      render::frameBufferCreate(context, size.x, size.y, copyRenderPass_, &fbAttachment, &copyFrameBuffer_);

      render::graphics_pipeline_t::description_t pipelineDesc = {};
      pipelineDesc.viewPort_ = { 0.0f, 0.0f, (float)context.swapChain_.imageWidth_, (float)context.swapChain_.imageHeight_, 0.0f, 1.0f };
      pipelineDesc.scissorRect_ = { { 0,0 },{ context.swapChain_.imageWidth_,context.swapChain_.imageHeight_ } };
      pipelineDesc.blendState_.resize(1);
      pipelineDesc.blendState_[0].colorWriteMask = 0xF;
      pipelineDesc.blendState_[0].blendEnable = VK_FALSE;
      pipelineDesc.cullMode_ = VK_CULL_MODE_BACK_BIT;
      pipelineDesc.depthTestEnabled_ = false;
      pipelineDesc.depthWriteEnabled_ = false;
      pipelineDesc.vertexShader_ = presentationVertexShader_;
      pipelineDesc.fragmentShader_ = presentationFragmentShader_;
      render::graphicsPipelineCreate(context, copyRenderPass_.handle_, 0u, fullScreenQuad_.vertexFormat_, presentationPipelineLayout_, pipelineDesc, &copyPipeline_);

      render::descriptor_t descriptor = render::getDescriptor(historyBuffer_[0]);
      render::descriptorSetCreate(context, descriptorPool_, presentationDescriptorSetLayout_, &descriptor, &copyDescriptorSet_[0]);

      descriptor = render::getDescriptor(finalImage_);
      render::descriptorSetCreate(context, descriptorPool_, presentationDescriptorSetLayout_, &descriptor, &copyDescriptorSet_[1]);
    }

  }

  void buildAndSubmitCommandBuffer()
  {
    render::context_t& context = getRenderContext();

    if (commandBuffer_.handle_ == VK_NULL_HANDLE)
    {
      render::commandBufferCreate(context, VK_COMMAND_BUFFER_LEVEL_PRIMARY, nullptr, nullptr, 0u, &renderComplete_, 1u, render::command_buffer_t::GRAPHICS, &commandBuffer_);
    }

    VkClearValue clearValues[5];
    clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clearValues[2].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clearValues[3].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clearValues[4].depthStencil = { 1.0f,0 };

    render::commandBufferBegin(context, &frameBuffer_, clearValues, 5u, commandBuffer_);
    {
      //GBuffer pass
      bkk::render::graphicsPipelineBind(commandBuffer_.handle_, gBufferPipeline_);
      render::descriptor_set_t descriptorSets[3];
      descriptorSets[0] = globalsDescriptorSet_;
      packed_freelist_iterator_t<object_t> objectIter = object_.begin();
      while (objectIter != object_.end())
      {
        descriptorSets[1] = objectIter.get().descriptorSet_;
        descriptorSets[2] = material_.get(objectIter.get().material_)->descriptorSet_;
        bkk::render::descriptorSetBindForGraphics(commandBuffer_.handle_, gBufferPipelineLayout_, 0, descriptorSets, 3u);
        mesh::mesh_t* mesh = mesh_.get(objectIter.get().mesh_);
        mesh::draw(commandBuffer_.handle_, *mesh);
        ++objectIter;
      }

      bkk::render::commandBufferNextSubpass(commandBuffer_);

      //Light pass      
      bkk::render::graphicsPipelineBind(commandBuffer_.handle_, lightPipeline_);
      descriptorSets[1] = lightPassTexturesDescriptorSet_;
      packed_freelist_iterator_t<light_t> lightIter = light_.begin();
      while (lightIter != light_.end())
      {
        descriptorSets[2] = lightIter.get().descriptorSet_;
        bkk::render::descriptorSetBindForGraphics(commandBuffer_.handle_, lightPipelineLayout_, 0u, descriptorSets, 3u);
        mesh::draw(commandBuffer_.handle_, sphereMesh_);
        ++lightIter;
      }
    }

    render::commandBufferEnd(commandBuffer_);
    render::commandBufferSubmit(context, commandBuffer_);

    //TXAA resolve pass
    if (txaaResolveCommandBuffer_.handle_ == VK_NULL_HANDLE)
    {
      VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      render::commandBufferCreate(context, VK_COMMAND_BUFFER_LEVEL_PRIMARY, &renderComplete_, &waitStage, 1u, &txaaResolveComplete_, 1u, render::command_buffer_t::GRAPHICS, &txaaResolveCommandBuffer_);
    }

    render::commandBufferBegin(context, &txaaResolveFrameBuffer_, clearValues, 1u, txaaResolveCommandBuffer_);
    {
      if (bTemporalAA_)
      {
        bkk::render::graphicsPipelineBind(txaaResolveCommandBuffer_.handle_, txaaResolvePipeline_);
        bkk::render::descriptorSetBindForGraphics(txaaResolveCommandBuffer_.handle_, txaaResolvePipelineLayout_, 0u, &txaaResolveDescriptorSet_, 1u);
        mesh::draw(txaaResolveCommandBuffer_.handle_, fullScreenQuad_);
      }
    }
    render::commandBufferEnd(txaaResolveCommandBuffer_);
    render::commandBufferSubmit(context, txaaResolveCommandBuffer_);
    
    if (copyCommandBuffer_.handle_ == VK_NULL_HANDLE)
    {
      VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      render::commandBufferCreate(context, VK_COMMAND_BUFFER_LEVEL_PRIMARY, &txaaResolveComplete_, &waitStage, 1u, &copyComplete_, 1u, render::command_buffer_t::GRAPHICS, &copyCommandBuffer_);
    }

    //Copy resolved image to presentation image
    render::commandBufferBegin(context, &copyFrameBuffer_, clearValues, 1u, copyCommandBuffer_);
    {
        bkk::render::graphicsPipelineBind(copyCommandBuffer_.handle_, copyPipeline_);
        bkk::render::descriptorSetBindForGraphics(copyCommandBuffer_.handle_, presentationPipelineLayout_, 0u, &copyDescriptorSet_[bTemporalAA_ ? 0 : 1], 1u);
        mesh::draw(copyCommandBuffer_.handle_, fullScreenQuad_);
      
    }
    render::commandBufferEnd(copyCommandBuffer_);
    render::commandBufferSubmit(context, copyCommandBuffer_);

  }

  void buildPresentationCommandBuffers()
  {
    render::context_t& context = getRenderContext();

    const VkCommandBuffer* commandBuffers;
    uint32_t count = bkk::render::getPresentationCommandBuffers(context, &commandBuffers);
    for (uint32_t i(0); i < count; ++i)
    {
      bkk::render::beginPresentationCommandBuffer(context, i, nullptr);
      bkk::render::graphicsPipelineBind(commandBuffers[i], presentationPipeline_);
      bkk::render::descriptorSetBindForGraphics(commandBuffers[i], presentationPipelineLayout_, 0u, &presentationDescriptorSet_, 1u);
      bkk::mesh::draw(commandBuffers[i], fullScreenQuad_);
      bkk::render::endPresentationCommandBuffer(context, i);
    }
  }

private:
  ///Member variables
  bkk::transform_manager_t transformManager_;
  render::gpu_memory_allocator_t allocator_;

  packed_freelist_t<object_t> object_;
  packed_freelist_t<material_t> material_;
  packed_freelist_t<mesh::mesh_t> mesh_;
  packed_freelist_t<light_t> light_;

  render::descriptor_pool_t descriptorPool_;
  render::descriptor_set_layout_t globalsDescriptorSetLayout_;
  render::descriptor_set_layout_t materialDescriptorSetLayout_;
  render::descriptor_set_layout_t objectDescriptorSetLayout_;
  render::descriptor_set_layout_t lightDescriptorSetLayout_;
  render::descriptor_set_layout_t lightPassTexturesDescriptorSetLayout_;
  render::descriptor_set_layout_t presentationDescriptorSetLayout_;

  render::descriptor_set_t presentationDescriptorSet_;
  render::descriptor_set_t globalsDescriptorSet_;
  render::descriptor_set_t lightPassTexturesDescriptorSet_;

  render::vertex_format_t vertexFormat_;

  render::pipeline_layout_t gBufferPipelineLayout_;
  render::graphics_pipeline_t gBufferPipeline_;
  render::pipeline_layout_t lightPipelineLayout_;
  render::graphics_pipeline_t lightPipeline_;

  render::pipeline_layout_t presentationPipelineLayout_;
  render::graphics_pipeline_t presentationPipeline_;

  VkSemaphore renderComplete_;
  render::command_buffer_t commandBuffer_;
  render::render_pass_t renderPass_;

  scene_uniforms_t sceneUniforms_;
  render::gpu_buffer_t globalsUbo_;

  render::frame_buffer_t frameBuffer_;
  render::texture_t gBufferRT0_;  //Albedo + roughness
  render::texture_t gBufferRT1_;  //Normal + Depth
  render::texture_t gBufferRT2_;  //F0 + metallic
  render::texture_t finalImage_;
  render::depth_stencil_buffer_t depthStencilBuffer_;

  render::shader_t gBuffervertexShader_;
  render::shader_t gBufferfragmentShader_;
  render::shader_t lightVertexShader_;
  render::shader_t lightFragmentShader_;
  render::shader_t presentationVertexShader_;
  render::shader_t presentationFragmentShader_;
  render::shader_t txaaResolveFragmentShader_;

  render::texture_t historyBuffer_[2];
  render::frame_buffer_t copyFrameBuffer_;
  render::frame_buffer_t txaaResolveFrameBuffer_;
  render::render_pass_t copyRenderPass_;
  render::render_pass_t txaaResolveRenderPass_;

  VkSemaphore txaaResolveComplete_;
  VkSemaphore copyComplete_;

  render::descriptor_set_layout_t txaaResolveDescriptorSetLayout_;
  render::pipeline_layout_t txaaResolvePipelineLayout_;
  render::graphics_pipeline_t txaaResolvePipeline_;
  render::graphics_pipeline_t copyPipeline_;
  render::command_buffer_t txaaResolveCommandBuffer_;
  render::command_buffer_t copyCommandBuffer_;
  render::descriptor_set_t copyDescriptorSet_[2];
  render::descriptor_set_t txaaResolveDescriptorSet_;

  mesh::mesh_t sphereMesh_;
  mesh::mesh_t fullScreenQuad_;

  free_camera_t camera_;
  bool bTemporalAA_;
  uint32_t currentFrame_;
};


int main()
{
  TXAA_sample_t scene;

  //Materials  
  bkk::handle_t wall = scene.addMaterial(vec3(0.5f, 0.5f, 0.5f), 0.0f, vec3(0.004f, 0.004f, 0.004f), 0.7f);
  bkk::handle_t redWall = scene.addMaterial(vec3(0.5f, 0.0f, 0.0f), 0.0f, vec3(0.04f, 0.04f, 0.04f), 0.7f);
  bkk::handle_t greenWall = scene.addMaterial(vec3(0.0f, 0.5f, 0.0f), 0.0f, vec3(0.004f, 0.004f, 0.004f), 0.7f);
  bkk::handle_t gold = scene.addMaterial(vec3(0.0f, 0.0f, 0.0f), 1.0f, vec3(1.000f, 0.766f, 0.336f), 0.3f);

  //Meshes
  bkk::handle_t teapot = scene.addMesh("../resources/teapot.obj");
  bkk::handle_t quad = scene.addQuadMesh();

 
  //Objects
  scene.addObject(quad, wall, maths::computeTransform(maths::vec3(0.0f, 0.0f, 0.0f), maths::vec3(5.0f, 5.0f, 5.0f), maths::QUAT_UNIT));
  scene.addObject(quad, redWall, maths::computeTransform(maths::vec3(-5.0f, 4.0f, 0.0f), maths::vec3(4.0f, 5.0f, 5.0f), maths::quaternionFromAxisAngle(vec3(0, 0, 1), maths::degreeToRadian(90.0f))));
  scene.addObject(quad, greenWall, maths::computeTransform(maths::vec3(5.0f, 4.0f, 0.0f), maths::vec3(4.0f, 5.0f, 5.0f), maths::quaternionFromAxisAngle(vec3(0, 0, 1), maths::degreeToRadian(-90.0f))));
  scene.addObject(quad, wall, maths::computeTransform(maths::vec3(0.0f, 4.0f, -5.0f), maths::vec3(5.0f, 5.0f, 4.0f), maths::quaternionFromAxisAngle(vec3(1, 0, 0), maths::degreeToRadian(-90.0f))));
  scene.addObject(quad, wall, maths::computeTransform(maths::vec3(0.0f, 8.0f, 0.0f), maths::vec3(5.0f, 5.0f, 5.0f), maths::quaternionFromAxisAngle(vec3(1, 0, 0), maths::degreeToRadian(180.0f))));
  scene.addObject(teapot, gold, maths::computeTransform(maths::vec3(0.0f, -0.4f, 0.5f), maths::vec3(1.0f, 1.0f, 1.0f), maths::QUAT_UNIT));
  
  //Lights
  scene.addLight(vec3(0.0f, 5.0f, 5.0f), 25.0f, vec3(1.0f, 1.0f, 1.0f));
  scene.loop();
  return 0;
}