/*
 * Copyright (c) 2021, Kirill GPRB.
 * All Rights Reserved.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "gl_private.hpp"

static void GLAPIENTRY debugCallback(GLenum, GLenum, GLuint, GLenum, GLsizei, const char *message, const void *arg)
{
    const uvre::DeviceInfo *info = reinterpret_cast<const uvre::DeviceInfo *>(arg);
    info->onMessage(message);
}

uvre::GLRenderDevice::GLRenderDevice(const uvre::DeviceInfo &info)
    : info(info), vbos(nullptr), null_pipeline(), bound_pipeline(&null_pipeline), shaders(), pipelines(), buffers(), samplers(), textures(), rendertargets(), commandlists()
{
    glCreateBuffers(1, &idbo);
    glNamedBufferData(idbo, static_cast<GLsizeiptr>(sizeof(uvre::DrawCmd)), nullptr, GL_DYNAMIC_DRAW);

    vbos = new uvre::VBOBinding;
    vbos->index = 0;
    vbos->is_free = true;
    vbos->next = nullptr;

    null_pipeline.vaobj = 0;
    null_pipeline.ppobj = 0;
    null_pipeline.blending.enabled = false;
    null_pipeline.depth_testing.enabled = false;
    null_pipeline.face_culling.enabled = false;
    null_pipeline.index_type = GL_UNSIGNED_SHORT;
    null_pipeline.primitive_mode = GL_LINE_STRIP;
    null_pipeline.fill_mode = GL_LINE;

    if(this->info.onMessage) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debugCallback, &this->info);
    }
}

uvre::GLRenderDevice::~GLRenderDevice()
{
    for(uvre::ICommandList *commands : commandlists) {
        delete commands;
    }

    for(uvre::RenderTarget *target : rendertargets) {
        glDeleteFramebuffers(1, &target->fbobj);
        delete target;
    }

    for(uvre::Sampler *sampler : samplers) {
        glDeleteSamplers(1, &sampler->ssobj);
        delete sampler;
    }

    for(uvre::Texture *texture : textures) {
        glDeleteTextures(1, &texture->texobj);
        delete texture;
    }

    for(uvre::Buffer *buffer : buffers) {
        glDeleteBuffers(1, &buffer->bufobj);
        delete buffer;
    }

    for(uvre::Pipeline *pipeline : pipelines) {
        glDeleteVertexArrays(1, &pipeline->vaobj);
        glDeleteProgramPipelines(1, &pipeline->ppobj);
        delete pipeline;
    }

    for(uvre::Shader *shader : shaders) {
        glDeleteProgram(shader->prog);
        delete shader;
    }

    shaders.clear();
    pipelines.clear();
    buffers.clear();
    textures.clear();
    samplers.clear();
    rendertargets.clear();
    commandlists.clear();

    // Make sure that the GL context doesn't use it anymore
    glDisable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(nullptr, nullptr);

    glDeleteBuffers(1, &idbo);
}

uvre::Shader *uvre::GLRenderDevice::createShader(const uvre::ShaderInfo &info)
{
    std::stringstream ss;
    ss << "#version 460 core\n";
    ss << "#define UVRE_SOURCE 1\n";

    uint32_t stage = 0;
    uint32_t stage_bit = 0;
    switch(info.stage) {
        case uvre::ShaderStage::VERTEX:
            stage = GL_VERTEX_SHADER;
            stage_bit = GL_VERTEX_SHADER_BIT;
            ss << "#define VERTEX_SHADER 1\n";
            break;
        case uvre::ShaderStage::FRAGMENT:
            stage = GL_FRAGMENT_SHADER;
            stage_bit = GL_FRAGMENT_SHADER_BIT;
            ss << "#define FRAGMENT_SHADER 1\n";
            break;
    }

    int32_t status, info_log_length;
    std::string info_log;
    uint32_t shobj = glCreateShader(stage);
    const char *source_cstr;
    std::string source;

    switch(info.format) {
        case uvre::ShaderFormat::BINARY_SPIRV:
            glShaderBinary(1, &shobj, GL_SHADER_BINARY_FORMAT_SPIR_V, info.code, static_cast<GLsizei>(info.code_size));
            glSpecializeShader(shobj, "main", 0, nullptr, nullptr);
            break;
        case uvre::ShaderFormat::SOURCE_GLSL:
            source = ss.str() + reinterpret_cast<const char *>(info.code);
            source_cstr = source.c_str();
            glShaderSource(shobj, 1, &source_cstr, nullptr);
            glCompileShader(shobj);
            break;
        default:
            glDeleteShader(shobj);
            return nullptr;
    }

    if(this->info.onMessage) {
        glGetShaderiv(shobj, GL_INFO_LOG_LENGTH, &info_log_length);
        if(info_log_length > 1) {
            info_log.resize(info_log_length);
            glGetShaderInfoLog(shobj, static_cast<GLsizei>(info_log.size()), nullptr, &info_log[0]);
            this->info.onMessage(info_log.c_str());
        }
    }

    glGetShaderiv(shobj, GL_COMPILE_STATUS, &status);
    if(!status) {
        glDeleteShader(shobj);
        return nullptr;
    }

    uint32_t prog = glCreateProgram();
    glProgramParameteri(prog, GL_PROGRAM_SEPARABLE, GL_TRUE);
    glAttachShader(prog, shobj);
    glLinkProgram(prog);
    glDeleteShader(shobj);

    if(this->info.onMessage) {
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &info_log_length);
        if(info_log_length > 1) {
            info_log.resize(info_log_length);
            glGetProgramInfoLog(prog, static_cast<GLsizei>(info_log.size()), nullptr, &info_log[0]);
            this->info.onMessage(info_log.c_str());
        }
    }

    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if(!status) {
        glDeleteProgram(prog);
        return nullptr;
    }

    uvre::Shader *shader = new uvre::Shader;
    shader->prog = prog;
    shader->stage = info.stage;
    shader->stage_bit = stage_bit;

    shaders.push_back(shader);
    return shader;
}

void uvre::GLRenderDevice::destroyShader(uvre::Shader *shader)
{
    for(std::vector<uvre::Shader *>::const_iterator it = shaders.cbegin(); it != shaders.cend(); it++) {
        if(*it == shader) {
            shaders.erase(it);
            glDeleteProgram(shader->prog);
            delete shader;
            return;
        }
    }
}

static inline uint32_t getBlendEquation(uvre::BlendEquation equation)
{
    switch(equation) {
        case uvre::BlendEquation::ADD:
            return GL_FUNC_ADD;
        case uvre::BlendEquation::SUBTRACT:
            return GL_FUNC_SUBTRACT;
        case uvre::BlendEquation::REVERSE_SUBTRACT:
            return GL_FUNC_REVERSE_SUBTRACT;
        case uvre::BlendEquation::MIN:
            return GL_MIN;
        case uvre::BlendEquation::MAX:
            return GL_MAX;
        default:
            return 0;
    }
}

static inline uint32_t getBlendFunc(uvre::BlendFunc func)
{
    switch(func) {
        case uvre::BlendFunc::ZERO:
            return GL_ZERO;
        case uvre::BlendFunc::ONE:
            return GL_ONE;
        case uvre::BlendFunc::SRC_COLOR:
            return GL_SRC_COLOR;
        case uvre::BlendFunc::ONE_MINUS_SRC_COLOR:
            return GL_ONE_MINUS_SRC_COLOR;
        case uvre::BlendFunc::SRC_ALPHA:
            return GL_SRC_ALPHA;
        case uvre::BlendFunc::ONE_MINUS_SRC_ALPHA:
            return GL_ONE_MINUS_SRC_ALPHA;
        case uvre::BlendFunc::DST_COLOR:
            return GL_DST_COLOR;
        case uvre::BlendFunc::ONE_MINUS_DST_COLOR:
            return GL_ONE_MINUS_DST_COLOR;
        case uvre::BlendFunc::DST_ALPHA:
            return GL_DST_ALPHA;
        case uvre::BlendFunc::ONE_MINUS_DST_ALPHA:
            return GL_ONE_MINUS_DST_ALPHA;
        default:
            return 0;
    }
}

static inline uint32_t getDepthFunc(uvre::DepthFunc func)
{
    switch(func) {
        case uvre::DepthFunc::NEVER:
            return GL_NEVER;
        case uvre::DepthFunc::ALWAYS:
            return GL_ALWAYS;
        case uvre::DepthFunc::EQUAL:
            return GL_EQUAL;
        case uvre::DepthFunc::NOT_EQUAL:
            return GL_NOTEQUAL;
        case uvre::DepthFunc::LESS:
            return GL_LESS;
        case uvre::DepthFunc::LESS_OR_EQUAL:
            return GL_LEQUAL;
        case uvre::DepthFunc::GREATER:
            return GL_GREATER;
        case uvre::DepthFunc::GREATER_OR_EQUAL:
            return GL_GEQUAL;
        default:
            return 0;
    }
}

static uint32_t getAttribType(uvre::VertexAttribType type)
{
    switch(type) {
        case uvre::VertexAttribType::FLOAT32:
            return GL_FLOAT;
        case uvre::VertexAttribType::FLOAT64:
            return GL_DOUBLE;
        case uvre::VertexAttribType::SIGNED_INT8:
            return GL_BYTE;
        case uvre::VertexAttribType::SIGNED_INT16:
            return GL_SHORT;
        case uvre::VertexAttribType::SIGNED_INT32:
            return GL_INT;
        case uvre::VertexAttribType::UNSIGNED_INT8:
            return GL_UNSIGNED_BYTE;
        case uvre::VertexAttribType::UNSIGNED_INT16:
            return GL_UNSIGNED_SHORT;
        case uvre::VertexAttribType::UNSIGNED_INT32:
            return GL_UNSIGNED_INT;
        default:
            return 0;
    }
}

static inline uint32_t getIndexType(uvre::IndexType type)
{
    switch(type) {
        case uvre::IndexType::INDEX16:
            return GL_UNSIGNED_SHORT;
        case uvre::IndexType::INDEX32:
            return GL_UNSIGNED_INT;
        default:
            return 0;
    }
}

static inline uint32_t getPrimitiveType(uvre::PrimitiveMode type)
{
    switch(type) {
        case uvre::PrimitiveMode::POINTS:
            return GL_POINTS;
        case uvre::PrimitiveMode::LINES:
            return GL_LINES;
        case uvre::PrimitiveMode::LINE_STRIP:
            return GL_LINE_STRIP;
        case uvre::PrimitiveMode::LINE_LOOP:
            return GL_LINE_LOOP;
        case uvre::PrimitiveMode::TRIANGLES:
            return GL_TRIANGLES;
        case uvre::PrimitiveMode::TRIANGLE_STRIP:
            return GL_TRIANGLE_STRIP;
        case uvre::PrimitiveMode::TRIANGLE_FAN:
            return GL_TRIANGLE_FAN;
        default:
            return GL_LINE_STRIP;
    }
}

static inline uint32_t getCullFace(bool back, bool front)
{
    if(back && front)
        return GL_FRONT_AND_BACK;
    if(back)
        return GL_BACK;
    if(front)
        return GL_FRONT;
    return GL_BACK;
}

static inline uint32_t getFillMode(uvre::FillMode mode)
{
    switch(mode) {
        case uvre::FillMode::FILLED:
            return GL_FILL;
        case uvre::FillMode::POINTS:
            return GL_POINT;
        case uvre::FillMode::WIREFRAME:
            return GL_LINE;
        default:
            return GL_LINE;
    }
}

uvre::Pipeline *uvre::GLRenderDevice::createPipeline(const uvre::PipelineInfo &info)
{
    uvre::Pipeline *pipeline = new uvre::Pipeline;

    glCreateProgramPipelines(1, &pipeline->ppobj);
    glCreateVertexArrays(1, &pipeline->vaobj);

    pipeline->blending.enabled = info.blending.enabled;
    pipeline->blending.equation = getBlendEquation(info.blending.equation);
    pipeline->blending.sfactor = getBlendFunc(info.blending.sfactor);
    pipeline->blending.dfactor = getBlendFunc(info.blending.dfactor);
    pipeline->depth_testing.enabled = info.depth_testing.enabled;
    pipeline->depth_testing.func = getDepthFunc(info.depth_testing.func);
    pipeline->face_culling.enabled = info.face_culling.enabled;
    pipeline->face_culling.front_face = (info.face_culling.flags & uvre::CULL_CLOCKWISE) ? GL_CW : GL_CCW;
    pipeline->face_culling.cull_face = getCullFace(info.face_culling.flags & uvre::CULL_BACK, info.face_culling.flags & uvre::CULL_FRONT);
    pipeline->index_type = getIndexType(info.index_type);
    pipeline->primitive_mode = getPrimitiveType(info.primitive_mode);
    pipeline->fill_mode = getFillMode(info.fill_mode);
    pipeline->vertex_stride = info.vertex_stride;
    pipeline->attributes = std::vector<uvre::VertexAttrib>(info.vertex_attribs, info.vertex_attribs + info.num_vertex_attribs);

    for(const uvre::VertexAttrib &attrib : pipeline->attributes) {
        glEnableVertexArrayAttrib(pipeline->vaobj, attrib.id);
        glVertexArrayAttribFormat(pipeline->vaobj, attrib.id, static_cast<GLint>(attrib.count), getAttribType(attrib.type), attrib.normalized ? GL_TRUE : GL_FALSE, static_cast<GLuint>(attrib.offset));
    }

    for(size_t i = 0; i < info.num_shaders; i++) {
        if(info.shaders[i]) {
            // Use this shader stage
            glUseProgramStages(pipeline->ppobj, info.shaders[i]->stage_bit, info.shaders[i]->prog);
        }
    }

    // Add all the existing vertex buffers to this pipeline
    for(uvre::Buffer *buffer : buffers) {
        if(buffer->vbo) {
            // offset is zero and that is hardcoded
            glVertexArrayVertexBuffer(pipeline->vaobj, buffer->vbo->index, buffer->bufobj, 0, static_cast<GLsizei>(pipeline->vertex_stride));
        }
    }

    pipelines.push_back(pipeline);
    return pipeline;
}

void uvre::GLRenderDevice::destroyPipeline(uvre::Pipeline *pipeline)
{
    for(std::vector<uvre::Pipeline *>::const_iterator it = pipelines.cbegin(); it != pipelines.cend(); it++) {
        if(*it == pipeline) {
            pipelines.erase(it);
            glDeleteVertexArrays(1, &pipeline->vaobj);
            glDeleteProgramPipelines(1, &pipeline->ppobj);
            delete pipeline;
            return;
        }
    }
}

static uvre::VBOBinding *getFreeVBOBinding(uvre::VBOBinding **begin)
{
    for(uvre::VBOBinding *binding = *begin; binding; binding = binding->next) {
        if(binding->is_free) {
            // Bullseye
            return binding;
        }

        // Create a new binding
        // TODO: check for max bindings?
        if(!binding->next) {
            uvre::VBOBinding *next = new uvre::VBOBinding;
            next->index = binding->index + 1;
            next->is_free = true;
            next->next = *begin;
            *begin = next;
            return next;
        }
    }

    // Oh no cringe!
    return nullptr;
}

uvre::Buffer *uvre::GLRenderDevice::createBuffer(const uvre::BufferInfo &info)
{
    uvre::Buffer *buffer = new uvre::Buffer;

    glCreateBuffers(1, &buffer->bufobj);

    buffer->size = info.size;
    buffer->vbo = nullptr;

    if(info.type == uvre::BufferType::VERTEX_BUFFER) {
        buffer->vbo = getFreeVBOBinding(&vbos);
        buffer->vbo->is_free = false;

        // Add this buffer to all the existing pipelines
        // with the respective vertex stride
        for(uvre::Pipeline *pipeline : pipelines) {
            // offset is zero and that is hardcoded
            glVertexArrayVertexBuffer(pipeline->vaobj, buffer->vbo->index, buffer->bufobj, 0, static_cast<GLsizei>(pipeline->vertex_stride));
        }
    }

    if(buffer->size) {
        // Pre-allocate the buffer
        glNamedBufferData(buffer->bufobj, static_cast<GLsizeiptr>(buffer->size), info.data, GL_DYNAMIC_DRAW);
    }

    buffers.push_back(buffer);
    return buffer;
}

void uvre::GLRenderDevice::destroyBuffer(uvre::Buffer *buffer)
{
    for(std::vector<uvre::Buffer *>::const_iterator it = buffers.cbegin(); it != buffers.cend(); it++) {
        if(*it == buffer) {
            buffers.erase(it);
            glDeleteBuffers(1, &buffer->bufobj);
            if(buffer->vbo)
                buffer->vbo->is_free = true;
            delete buffer;
            return;
        }
    }
}

void uvre::GLRenderDevice::resizeBuffer(uvre::Buffer *buffer, size_t size, const void *data)
{
    buffer->size = size;
    glNamedBufferData(buffer->bufobj, static_cast<GLsizeiptr>(buffer->size), data, GL_DYNAMIC_DRAW);
}

bool uvre::GLRenderDevice::writeBuffer(uvre::Buffer *buffer, size_t offset, size_t size, const void *data)
{
    if(offset + size >= buffer->size)
        return false;
    glNamedBufferSubData(buffer->bufobj, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
    return true;
}

uvre::Sampler *uvre::GLRenderDevice::createSampler(const uvre::SamplerInfo &info)
{
    uint32_t ssobj;
    glCreateSamplers(1, &ssobj);

    glSamplerParameteri(ssobj, GL_TEXTURE_WRAP_S, (info.flags & SAMPLER_CLAMP_S) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glSamplerParameteri(ssobj, GL_TEXTURE_WRAP_T, (info.flags & SAMPLER_CLAMP_T) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glSamplerParameteri(ssobj, GL_TEXTURE_WRAP_R, (info.flags & SAMPLER_CLAMP_R) ? GL_CLAMP_TO_EDGE : GL_REPEAT);

    if(info.flags & SAMPLER_FILTER) {
        if(info.flags & SAMPLER_FILTER_ANISO)
            glSamplerParameterf(ssobj, GL_TEXTURE_MAX_ANISOTROPY, info.aniso_level);
        glSamplerParameterf(ssobj, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameterf(ssobj, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    else {
        glSamplerParameterf(ssobj, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameterf(ssobj, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    glSamplerParameterf(ssobj, GL_TEXTURE_MIN_LOD, info.min_lod);
    glSamplerParameterf(ssobj, GL_TEXTURE_MAX_LOD, info.max_lod);
    glSamplerParameterf(ssobj, GL_TEXTURE_LOD_BIAS, info.lod_bias);

    uvre::Sampler *sampler = new uvre::Sampler;
    sampler->ssobj = ssobj;

    samplers.push_back(sampler);
    return sampler;
}

void uvre::GLRenderDevice::destroySampler(uvre::Sampler *sampler)
{
    for(std::vector<uvre::Sampler *>::const_iterator it = samplers.cbegin(); it != samplers.cend(); it++) {
        if(*it == sampler) {
            samplers.erase(it);
            glDeleteSamplers(1, &sampler->ssobj);
            delete sampler;
            return;
        }
    }
}

static inline uint32_t getInternalFormat(uvre::PixelFormat format)
{
    switch(format) {
        case uvre::PixelFormat::R8_UNORM:
            return GL_R8;
        case uvre::PixelFormat::R8_SINT:
            return GL_R8I;
        case uvre::PixelFormat::R8_UINT:
            return GL_R8UI;
        case uvre::PixelFormat::R8G8_UNORM:
            return GL_RG8;
        case uvre::PixelFormat::R8G8_SINT:
            return GL_RG8I;
        case uvre::PixelFormat::R8G8_UINT:
            return GL_RG8UI;
        case uvre::PixelFormat::R8G8B8_UNORM:
            return GL_RGB8;
        case uvre::PixelFormat::R8G8B8_SINT:
            return GL_RGB8I;
        case uvre::PixelFormat::R8G8B8_UINT:
            return GL_RGB8UI;
        case uvre::PixelFormat::R8G8B8A8_UNORM:
            return GL_RGBA8;
        case uvre::PixelFormat::R8G8B8A8_SINT:
            return GL_RGBA8I;
        case uvre::PixelFormat::R8G8B8A8_UINT:
            return GL_RGBA8UI;
        case uvre::PixelFormat::R16_UNORM:
            return GL_R16;
        case uvre::PixelFormat::R16_SINT:
            return GL_R16I;
        case uvre::PixelFormat::R16_UINT:
            return GL_R16UI;
        case uvre::PixelFormat::R16_FLOAT:
            return GL_R16F;
        case uvre::PixelFormat::R16G16_UNORM:
            return GL_RG16;
        case uvre::PixelFormat::R16G16_SINT:
            return GL_RG16I;
        case uvre::PixelFormat::R16G16_UINT:
            return GL_RG16UI;
        case uvre::PixelFormat::R16G16_FLOAT:
            return GL_RG16F;
        case uvre::PixelFormat::R16G16B16_UNORM:
            return GL_RGB16;
        case uvre::PixelFormat::R16G16B16_SINT:
            return GL_RGB16I;
        case uvre::PixelFormat::R16G16B16_UINT:
            return GL_RGB16UI;
        case uvre::PixelFormat::R16G16B16_FLOAT:
            return GL_RGB16F;
        case uvre::PixelFormat::R16G16B16A16_UNORM:
            return GL_RGBA16;
        case uvre::PixelFormat::R16G16B16A16_SINT:
            return GL_RGBA16I;
        case uvre::PixelFormat::R16G16B16A16_UINT:
            return GL_RGBA16UI;
        case uvre::PixelFormat::R16G16B16A16_FLOAT:
            return GL_RGBA16F;
        case uvre::PixelFormat::R32_SINT:
            return GL_R32I;
        case uvre::PixelFormat::R32_UINT:
            return GL_R32UI;
        case uvre::PixelFormat::R32_FLOAT:
            return GL_R32F;
        case uvre::PixelFormat::R32G32_SINT:
            return GL_RG32I;
        case uvre::PixelFormat::R32G32_UINT:
            return GL_RG32UI;
        case uvre::PixelFormat::R32G32_FLOAT:
            return GL_RG32F;
        case uvre::PixelFormat::R32G32B32_SINT:
            return GL_RGB32I;
        case uvre::PixelFormat::R32G32B32_UINT:
            return GL_RGB32UI;
        case uvre::PixelFormat::R32G32B32_FLOAT:
            return GL_RGB32F;
        case uvre::PixelFormat::R32G32B32A32_SINT:
            return GL_RGBA32I;
        case uvre::PixelFormat::R32G32B32A32_UINT:
            return GL_RGBA32UI;
        case uvre::PixelFormat::R32G32B32A32_FLOAT:
            return GL_RGBA32F;
        case uvre::PixelFormat::D16_UNORM:
            return GL_DEPTH_COMPONENT16;
        case uvre::PixelFormat::D32_FLOAT:
            return GL_DEPTH_COMPONENT32F;
        case uvre::PixelFormat::S8_UINT:
            return GL_STENCIL_INDEX8;
        default:
            return 0;
    }
}

uvre::Texture *uvre::GLRenderDevice::createTexture(const uvre::TextureInfo &info)
{
    uvre::Texture *texture = new uvre::Texture;

    texture->format = getInternalFormat(info.format);
    texture->width = static_cast<int>(info.width);
    texture->height = static_cast<int>(info.height);
    texture->depth = static_cast<int>(info.depth);

    switch(info.type) {
        case uvre::TextureType::TEXTURE_2D:
            glCreateTextures(GL_TEXTURE_2D, 1, &texture->texobj);
            glTextureStorage2D(texture->texobj, std::max<uint32_t>(1, info.mip_levels), texture->format, texture->width, texture->height);
            break;
        case uvre::TextureType::TEXTURE_CUBE:
            glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &texture->texobj);
            glTextureStorage2D(texture->texobj, std::max<uint32_t>(1, info.mip_levels), texture->format, texture->width, texture->height);
            break;
        case uvre::TextureType::TEXTURE_ARRAY:
            glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &texture->texobj);
            glTextureStorage3D(texture->texobj, std::max<uint32_t>(1, info.mip_levels), texture->format, texture->width, texture->height, texture->depth);
            break;
        default:
            delete texture;
            return nullptr;
    }

    textures.push_back(texture);
    return texture;
}

void uvre::GLRenderDevice::destroyTexture(uvre::Texture *texture)
{
    for(std::vector<uvre::Texture *>::const_iterator it = textures.cbegin(); it != textures.cend(); it++) {
        if(*it == texture) {
            textures.erase(it);
            glDeleteTextures(1, &texture->texobj);
            delete texture;
            return;
        }
    }
}

static bool getExternalFormat(uvre::PixelFormat format, uint32_t &fmt, uint32_t &type)
{
    switch(format) {
        case uvre::PixelFormat::R8_UNORM:
        case uvre::PixelFormat::R8_SINT:
        case uvre::PixelFormat::R8_UINT:
        case uvre::PixelFormat::R16_UNORM:
        case uvre::PixelFormat::R16_SINT:
        case uvre::PixelFormat::R16_UINT:
        case uvre::PixelFormat::R16_FLOAT:
        case uvre::PixelFormat::R32_SINT:
        case uvre::PixelFormat::R32_UINT:
        case uvre::PixelFormat::R32_FLOAT:
            fmt = GL_RED;
            break;
        case uvre::PixelFormat::R8G8_UNORM:
        case uvre::PixelFormat::R8G8_SINT:
        case uvre::PixelFormat::R8G8_UINT:
        case uvre::PixelFormat::R16G16_UNORM:
        case uvre::PixelFormat::R16G16_SINT:
        case uvre::PixelFormat::R16G16_UINT:
        case uvre::PixelFormat::R16G16_FLOAT:
        case uvre::PixelFormat::R32G32_SINT:
        case uvre::PixelFormat::R32G32_UINT:
        case uvre::PixelFormat::R32G32_FLOAT:
            fmt = GL_RG;
            break;
        case uvre::PixelFormat::R8G8B8_UNORM:
        case uvre::PixelFormat::R8G8B8_SINT:
        case uvre::PixelFormat::R8G8B8_UINT:
        case uvre::PixelFormat::R16G16B16_UNORM:
        case uvre::PixelFormat::R16G16B16_SINT:
        case uvre::PixelFormat::R16G16B16_UINT:
        case uvre::PixelFormat::R16G16B16_FLOAT:
        case uvre::PixelFormat::R32G32B32_SINT:
        case uvre::PixelFormat::R32G32B32_UINT:
        case uvre::PixelFormat::R32G32B32_FLOAT:
            fmt = GL_RGB;
            break;
        case uvre::PixelFormat::R8G8B8A8_UNORM:
        case uvre::PixelFormat::R8G8B8A8_SINT:
        case uvre::PixelFormat::R8G8B8A8_UINT:
        case uvre::PixelFormat::R16G16B16A16_UNORM:
        case uvre::PixelFormat::R16G16B16A16_SINT:
        case uvre::PixelFormat::R16G16B16A16_UINT:
        case uvre::PixelFormat::R16G16B16A16_FLOAT:
        case uvre::PixelFormat::R32G32B32A32_SINT:
        case uvre::PixelFormat::R32G32B32A32_UINT:
        case uvre::PixelFormat::R32G32B32A32_FLOAT:
            fmt = GL_RGBA;
            break;
        default:
            return false;
    }

    switch(format) {
        case uvre::PixelFormat::R8_SINT:
        case uvre::PixelFormat::R8G8_SINT:
        case uvre::PixelFormat::R8G8B8_SINT:
        case uvre::PixelFormat::R8G8B8A8_SINT:
            type = GL_BYTE;
            break;
        case uvre::PixelFormat::R8_UNORM:
        case uvre::PixelFormat::R8_UINT:
        case uvre::PixelFormat::R8G8_UNORM:
        case uvre::PixelFormat::R8G8_UINT:
        case uvre::PixelFormat::R8G8B8_UNORM:
        case uvre::PixelFormat::R8G8B8_UINT:
        case uvre::PixelFormat::R8G8B8A8_UNORM:
        case uvre::PixelFormat::R8G8B8A8_UINT:
            type = GL_UNSIGNED_BYTE;
            break;
        case uvre::PixelFormat::R16_SINT:
        case uvre::PixelFormat::R16G16_SINT:
        case uvre::PixelFormat::R16G16B16_SINT:
        case uvre::PixelFormat::R16G16B16A16_SINT:
            type = GL_SHORT;
            break;
        case uvre::PixelFormat::R16_UNORM:
        case uvre::PixelFormat::R16_UINT:
        case uvre::PixelFormat::R16G16_UNORM:
        case uvre::PixelFormat::R16G16_UINT:
        case uvre::PixelFormat::R16G16B16_UNORM:
        case uvre::PixelFormat::R16G16B16_UINT:
        case uvre::PixelFormat::R16G16B16A16_UNORM:
        case uvre::PixelFormat::R16G16B16A16_UINT:
            type = GL_UNSIGNED_SHORT;
            break;
        case uvre::PixelFormat::R32_SINT:
        case uvre::PixelFormat::R32G32_SINT:
        case uvre::PixelFormat::R32G32B32_SINT:
        case uvre::PixelFormat::R32G32B32A32_SINT:
            type = GL_INT;
            break;
        case uvre::PixelFormat::R32_UINT:
        case uvre::PixelFormat::R32G32_UINT:
        case uvre::PixelFormat::R32G32B32_UINT:
        case uvre::PixelFormat::R32G32B32A32_UINT:
            type = GL_UNSIGNED_INT;
            break;
        case uvre::PixelFormat::R32_FLOAT:
        case uvre::PixelFormat::R32G32_FLOAT:
        case uvre::PixelFormat::R32G32B32_FLOAT:
        case uvre::PixelFormat::R32G32B32A32_FLOAT:
            type = GL_FLOAT;
            break;
        default:
            return false;
    }

    return true;
}

bool uvre::GLRenderDevice::writeTexture2D(uvre::Texture *texture, int x, int y, int w, int h, uvre::PixelFormat format, const void *data)
{
    uint32_t fmt, type;
    if(getExternalFormat(format, fmt, type)) {
        glTextureSubImage2D(texture->texobj, 0, x, y, w, h, fmt, type, data);
        return true;
    }

    return false;
}

bool uvre::GLRenderDevice::writeTextureCube(uvre::Texture *texture, int face, int x, int y, int w, int h, uvre::PixelFormat format, const void *data)
{
    uint32_t fmt, type;
    if(getExternalFormat(format, fmt, type)) {
        glTextureSubImage3D(texture->texobj, 0, x, y, face, w, h, 1, fmt, type, data);
        return true;
    }

    return false;
}

bool uvre::GLRenderDevice::writeTextureArray(uvre::Texture *texture, int x, int y, int z, int w, int h, int d, uvre::PixelFormat format, const void *data)
{
    uint32_t fmt, type;
    if(getExternalFormat(format, fmt, type)) {
        glTextureSubImage3D(texture->texobj, 0, x, y, z, w, h, d, fmt, type, data);
        return true;
    }

    return false;
}

uvre::RenderTarget *uvre::GLRenderDevice::createRenderTarget(const uvre::RenderTargetInfo &info)
{
    uint32_t fbobj;
    glCreateFramebuffers(1, &fbobj);
    if(info.depth_attachment)
        glNamedFramebufferTexture(fbobj, GL_DEPTH_ATTACHMENT, info.depth_attachment->texobj, 0);
    if(info.stencil_attachment)
        glNamedFramebufferTexture(fbobj, GL_STENCIL_ATTACHMENT, info.stencil_attachment->texobj, 0);
    for(size_t i = 0; i < info.num_color_attachments; i++)
        glNamedFramebufferTexture(fbobj, GL_COLOR_ATTACHMENT0 + info.color_attachments[i].id, info.color_attachments[i].color->texobj, 0);

    if(glCheckNamedFramebufferStatus(fbobj, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fbobj);
        return nullptr;
    }

    uvre::RenderTarget *target = new uvre::RenderTarget;
    target->fbobj = fbobj;

    rendertargets.push_back(target);
    return target;
}

void uvre::GLRenderDevice::destroyRenderTarget(uvre::RenderTarget *target)
{
    for(std::vector<uvre::RenderTarget *>::const_iterator it = rendertargets.cbegin(); it != rendertargets.cend(); it++) {
        if(*it == target) {
            rendertargets.erase(it);
            glDeleteFramebuffers(1, &target->fbobj);
            delete target;
            return;
        }
    }
}

uvre::ICommandList *uvre::GLRenderDevice::createCommandList()
{
    uvre::GLCommandList *commands = new uvre::GLCommandList(this);
    commandlists.push_back(commands);
    return commands;
}

void uvre::GLRenderDevice::destroyCommandList(uvre::ICommandList *commands)
{
    for(std::vector<uvre::GLCommandList *>::const_iterator it = commandlists.cbegin(); it != commandlists.cend(); it++) {
        if(*it == commands) {
            commandlists.erase(it);
            delete commands;
            return;
        }
    }
}

void uvre::GLRenderDevice::startRecording(uvre::ICommandList *)
{
    // Nothing in OpenGL
}

void uvre::GLRenderDevice::submit(uvre::ICommandList *)
{
    // Nothing in OpenGL
}

void uvre::GLRenderDevice::prepare()
{
    // Third-party overlay applications
    // can cause mayhem if this is not called.
    glUseProgram(0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, idbo);
}

void uvre::GLRenderDevice::present()
{
    info.gl.swapBuffers(info.gl.user_data);
}

void uvre::GLRenderDevice::vsync(bool enable)
{
    info.gl.setSwapInterval(info.gl.user_data, enable ? 1 : 0);
}

void uvre::GLRenderDevice::mode(int, int)
{
    // Nothing in OpenGL
}
