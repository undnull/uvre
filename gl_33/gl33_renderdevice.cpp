/*
 * Copyright (c) 2021, Kirill GPRB.
 * All Rights Reserved.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include <cstring>
#include <functional>
#include "gl33_private.hpp"

static uvre::VertexArray_S dummy_vao = { 0, 0, 0, nullptr };

static void GLAPIENTRY debugCallback(GLenum, GLenum, GLuint, GLenum severity, GLsizei, const char *message, const void *arg)
{
    const uvre::RenderDeviceImpl *device = reinterpret_cast<const uvre::RenderDeviceImpl *>(arg);
        if(device && device->create_info.onDebugMessage) {

        uvre::DebugMessageInfo msg = {};
        msg.text = message;

        switch(severity) {
            case GL_DEBUG_SEVERITY_HIGH:
                msg.level = uvre::DebugMessageLevel::ERROR;
                break;
            case GL_DEBUG_SEVERITY_MEDIUM:
                msg.level = uvre::DebugMessageLevel::WARN;
                break;
            case GL_DEBUG_SEVERITY_LOW:
                msg.level = uvre::DebugMessageLevel::INFO;
                break;
            case GL_DEBUG_SEVERITY_NOTIFICATION:
                msg.level = uvre::DebugMessageLevel::DEBUG;
                break;
        }

        device->create_info.onDebugMessage(msg);
    }
}

static void destroyShader(uvre::Shader_S *shader)
{
    glDeleteShader(shader->shader);
    delete shader;
}

static void destroyPipeline(uvre::Pipeline_S *pipeline, uvre::RenderDeviceImpl *device)
{
    // Remove ourselves from the notify list.
    for(std::vector<uvre::Pipeline_S *>::const_iterator it = device->pipelines.cbegin(); it != device->pipelines.cend(); it++) {
        if(*it != pipeline)
            continue;
        device->pipelines.erase(it);
        break;
    }

    // Chain-free the VAO list
    for(uvre::VertexArray_S *node = pipeline->vaos; node;) {
        uvre::VertexArray_S *next = node->next;
        glDeleteVertexArrays(1, &node->vaobj);
        delete node;
        node = next;
    }

    glDeleteProgram(pipeline->program);
    delete pipeline;
}

static void destroyBuffer(uvre::Buffer_S *buffer, uvre::RenderDeviceImpl *device)
{
    // Remove ourselves from the notify list.
    for(std::vector<uvre::Buffer_S *>::const_iterator it = device->buffers.cbegin(); it != device->buffers.cend(); it++) {
        if(*it != buffer)
            continue;
        device->buffers.erase(it);
        buffer->vbo->is_free = true;
        break;
    }

    glDeleteBuffers(1, &buffer->bufobj);
    delete buffer;
}

static void destroySampler(uvre::Sampler_S *sampler)
{
    glDeleteSamplers(1, &sampler->ssobj);
    delete sampler;
}

static void destroyTexture(uvre::Texture_S *texture)
{
    glDeleteTextures(1, &texture->texobj);
    delete texture;
}

static void destroyRenderTarget(uvre::RenderTarget_S *target)
{
    glDeleteFramebuffers(1, &target->fbobj);
    delete target;
}

uvre::RenderDeviceImpl::RenderDeviceImpl(const uvre::DeviceCreateInfo &create_info)
    : create_info(create_info), vbos(nullptr), bound_pipeline(), null_pipeline(), pipelines(), buffers(), commandlists()
{
    glGetIntegerv(GL_MAX_VERTEX_ATTRIB_BINDINGS, &max_vbo_bindings);

    std::memset(&info, 0, sizeof(uvre::DeviceInfo));
    info.impl_family = uvre::ImplFamily::OPENGL;
    info.impl_version_major = 3;
    info.impl_version_minor = 3;
    info.supports_anisotropic = false;
    info.supports_storage_buffers = false;
    info.supports_shader_format[static_cast<int>(uvre::ShaderFormat::SOURCE_GLSL)] = true;

    null_pipeline.blending.enabled = false;
    null_pipeline.depth_testing.enabled = false;
    null_pipeline.face_culling.enabled = false;
    null_pipeline.index_type = GL_UNSIGNED_SHORT;
    null_pipeline.primitive_mode = GL_TRIANGLES;
    null_pipeline.fill_mode = GL_LINES;
    null_pipeline.vertex_stride = 0;
    null_pipeline.num_attributes = 0;
    null_pipeline.attributes = 0;
    null_pipeline.vaos = nullptr;
    bound_pipeline = null_pipeline;

    vbos = new uvre::VBOBinding;
    vbos->index = 0;
    vbos->is_free = true;
    vbos->next = nullptr;

    if(create_info.onDebugMessage) {
        if(GLAD_GL_KHR_debug) {
            glEnable(GL_DEBUG_OUTPUT);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(debugCallback, this);
        }
        else {
            uvre::DebugMessageInfo msg = {};
            msg.level = uvre::DebugMessageLevel::WARN;
            msg.text = "GLAD_GL_KHR_debug not present";
            create_info.onDebugMessage(msg);
        }
    }
}

uvre::RenderDeviceImpl::~RenderDeviceImpl()
{
    for(uvre::CommandListImpl *commandlist : commandlists)
        delete commandlist;

    pipelines.clear();
    buffers.clear();
    commandlists.clear();

    // Make sure that the GL context doesn't use it anymore
    glDisable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(nullptr, nullptr);
}

const uvre::DeviceInfo &uvre::RenderDeviceImpl::getInfo() const
{
    return info;
}

uvre::Shader uvre::RenderDeviceImpl::createShader(const uvre::ShaderCreateInfo &info)
{
    std::stringstream ss;
    ss << "#version 330 core" << std::endl;
    ss << "#define _UVRE_ 1" << std::endl;

    uint32_t stage = 0;
    switch(info.stage) {
        case uvre::ShaderStage::VERTEX:
            stage = GL_VERTEX_SHADER;
            ss << "#define _VERTEX_SHADER_ 1" << std::endl;
            break;
        case uvre::ShaderStage::FRAGMENT:
            stage = GL_FRAGMENT_SHADER;
            ss << "#define _FRAGMENT_SHADER_ 1" << std::endl;
            break;
    }

    int32_t status, info_log_length;
    std::string info_log;
    uint32_t shobj = glCreateShader(stage);
    const char *source_cstr;
    std::string source;

    switch(info.format) {
        case uvre::ShaderFormat::SOURCE_GLSL:
            ss << "#define _GLSL_ 1" << std::endl;
            source = ss.str() + reinterpret_cast<const char *>(info.code);
            source_cstr = source.c_str();
            glShaderSource(shobj, 1, &source_cstr, nullptr);
            glCompileShader(shobj);
            break;
        default:
            glDeleteShader(shobj);
            return nullptr;
    }

    if(create_info.onDebugMessage) {
        glGetShaderiv(shobj, GL_INFO_LOG_LENGTH, &info_log_length);
        if(info_log_length > 1) {
            info_log.resize(info_log_length);
            glGetShaderInfoLog(shobj, static_cast<GLsizei>(info_log.size()), nullptr, &info_log[0]);

            uvre::DebugMessageInfo msg = {};
            msg.level = uvre::DebugMessageLevel::INFO;
            msg.text = info_log.c_str();
            create_info.onDebugMessage(msg);
        }
    }

    glGetShaderiv(shobj, GL_COMPILE_STATUS, &status);
    if(!status) {
        glDeleteShader(shobj);
        return nullptr;
    }

    uvre::Shader shader(new uvre::Shader_S, destroyShader);
    shader->shader = shobj;
    shader->stage = info.stage;

    return shader;
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
        case uvre::VertexAttribType::SIGNED_INT32:
            return GL_INT;
        case uvre::VertexAttribType::UNSIGNED_INT32:
            return GL_UNSIGNED_INT;
        default:
            return 0;
    }
}

static inline size_t getIndexSize(uvre::IndexType type)
{
    switch(type) {
        case uvre::IndexType::INDEX16:
            return sizeof(uvre::Index16);
        case uvre::IndexType::INDEX32:
            return sizeof(uvre::Index32);
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

static inline void setVertexFormat(uvre::VertexArray_S *vao, const uvre::Pipeline_S *pipeline)
{
    if(vao && vao != &dummy_vao) {
        glBindVertexArray(vao->vaobj);
        for(size_t i = 0; i < pipeline->num_attributes; i++) {
            uvre::VertexAttrib &attrib = pipeline->attributes[i];
            glEnableVertexAttribArray(attrib.id);
            switch(attrib.type) {
                case uvre::VertexAttribType::FLOAT32:
                    glVertexAttribFormat(attrib.id, static_cast<GLint>(attrib.count), getAttribType(attrib.type), attrib.normalized ? GL_TRUE : GL_FALSE, static_cast<GLuint>(attrib.offset));
                    break;
                case uvre::VertexAttribType::SIGNED_INT32:
                case uvre::VertexAttribType::UNSIGNED_INT32:
                    // Oh, OpenGL, you did it again. You shat itself.
                    glVertexAttribIFormat(attrib.id, static_cast<GLint>(attrib.count), getAttribType(attrib.type), static_cast<GLuint>(attrib.offset));
                    break;
            }
        }
    }
}

static inline uvre::VertexArray_S *getVertexArray(uvre::VertexArray_S **head, uint32_t index, const uvre::Pipeline_S *pipeline)
{
    for(uvre::VertexArray_S *node = *head; node; node = node->next) {
        if(index != node->index)
            continue;
        return node;
    }

    uvre::VertexArray_S *next = new uvre::VertexArray_S;
    next->index = (*head)->index + 1;
    glGenVertexArrays(1, &next->vaobj);
    next->vbobj = 0;
    setVertexFormat(next, pipeline);
    next->next = *head;
    *head = next;
    return next;
}

uvre::Pipeline uvre::RenderDeviceImpl::createPipeline(const uvre::PipelineCreateInfo &info)
{
    uvre::Pipeline pipeline(new uvre::Pipeline_S, std::bind(destroyPipeline, std::placeholders::_1, this));

    pipeline->program = glCreateProgram();
    for(size_t i = 0; i < info.num_shaders; i++)
        glAttachShader(pipeline->program, info.shaders[i]->shader);
    glLinkProgram(pipeline->program);

    if(create_info.onDebugMessage) {
        int info_log_length;
        std::string info_log;
        glGetProgramiv(pipeline->program, GL_INFO_LOG_LENGTH, &info_log_length);
        if(info_log_length > 1) {
            info_log.resize(info_log_length);
            glGetProgramInfoLog(pipeline->program, static_cast<GLsizei>(info_log.size()), nullptr, &info_log[0]);

            uvre::DebugMessageInfo msg = {};
            msg.level = uvre::DebugMessageLevel::INFO;
            msg.text = info_log.c_str();
            create_info.onDebugMessage(msg);
        }
    }

    int status;
    glGetProgramiv(pipeline->program, GL_LINK_STATUS, &status);
    if(!status) {
        pipeline = nullptr;
        return nullptr;
    }

    pipeline->bound_ibo = 0;
    pipeline->bound_vao = 0;
    pipeline->blending.enabled = info.blending.enabled;
    pipeline->blending.equation = getBlendEquation(info.blending.equation);
    pipeline->blending.sfactor = getBlendFunc(info.blending.sfactor);
    pipeline->blending.dfactor = getBlendFunc(info.blending.dfactor);
    pipeline->depth_testing.enabled = info.depth_testing.enabled;
    pipeline->depth_testing.func = getDepthFunc(info.depth_testing.func);
    pipeline->face_culling.enabled = info.face_culling.enabled;
    pipeline->face_culling.front_face = (info.face_culling.flags & uvre::CULL_CLOCKWISE) ? GL_CW : GL_CCW;
    pipeline->face_culling.cull_face = getCullFace(info.face_culling.flags & uvre::CULL_BACK, info.face_culling.flags & uvre::CULL_FRONT);
    pipeline->index_size = getIndexSize(info.index_type);
    pipeline->index_type = getIndexType(info.index_type);
    pipeline->primitive_mode = getPrimitiveType(info.primitive_mode);
    pipeline->fill_mode = getFillMode(info.fill_mode);
    pipeline->vertex_stride = info.vertex_stride;
    pipeline->num_attributes = info.num_vertex_attribs;
    pipeline->attributes = new uvre::VertexAttrib[pipeline->num_attributes];
    std::copy(info.vertex_attribs, info.vertex_attribs + info.num_vertex_attribs, pipeline->attributes);

    pipeline->vaos = new uvre::VertexArray_S;
    pipeline->vaos->index = 0;
    glGenVertexArrays(1, &pipeline->vaos->vaobj);
    pipeline->vaos->next = nullptr;
    setVertexFormat(pipeline->vaos, pipeline.get());

    // Notify the buffers
    for(uvre::Buffer_S *buffer : buffers) {
        // offset is zero and that is hardcoded
        glBindVertexArray(getVertexArray(&pipeline->vaos, buffer->vbo->index / max_vbo_bindings, pipeline.get())->vaobj);
        glBindVertexBuffer(buffer->vbo->index % max_vbo_bindings, buffer->bufobj, 0, static_cast<GLsizei>(pipeline->vertex_stride));
    }

    // Add ourselves to the notify list.
    pipelines.push_back(pipeline.get());

    return pipeline;
}

static uvre::VBOBinding *getFreeVBOBinding(uvre::VBOBinding **head)
{
    for(uvre::VBOBinding *node = *head; node; node = node->next) {
        if(!node->is_free)
            continue;
        return node;
    }

    uvre::VBOBinding *next = new uvre::VBOBinding;
    next->index = (*head)->index + 1;
    next->is_free = true;
    next->next = *head;
    *head = next;
    return next;
}

uvre::Buffer uvre::RenderDeviceImpl::createBuffer(const uvre::BufferCreateInfo &info)
{
    uvre::Buffer buffer(new uvre::Buffer_S, std::bind(destroyBuffer, std::placeholders::_1, this));

    glGenBuffers(1, &buffer->bufobj);

    buffer->size = info.size;
    buffer->vbo = nullptr;

    if(info.type == uvre::BufferType::VERTEX_BUFFER) {
        buffer->vbo = getFreeVBOBinding(&vbos);
        buffer->vbo->is_free = false;

        // Notify the pipeline objects
        for(uvre::Pipeline_S *pipeline : pipelines) {
            // offset is zero and that is hardcoded
            glBindVertexArray(getVertexArray(&pipeline->vaos, buffer->vbo->index / max_vbo_bindings, pipeline)->vaobj);
            glBindVertexBuffer(buffer->vbo->index % max_vbo_bindings, buffer->bufobj, 0, static_cast<GLsizei>(pipeline->vertex_stride));
        }

        // Add ourselves to the notify list
        buffers.push_back(buffer.get());
    }

    glBindBuffer(GL_COPY_READ_BUFFER, buffer->bufobj);
    glBufferData(GL_COPY_READ_BUFFER, static_cast<GLsizeiptr>(buffer->size), info.data, GL_DYNAMIC_DRAW);
    return buffer;
}

void uvre::RenderDeviceImpl::writeBuffer(uvre::Buffer buffer, size_t offset, size_t size, const void *data)
{
    if(offset + size > buffer->size)
        return;
    glBindBuffer(GL_COPY_READ_BUFFER, buffer->bufobj);
    glBufferSubData(GL_COPY_READ_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
}

uvre::Sampler uvre::RenderDeviceImpl::createSampler(const uvre::SamplerCreateInfo &info)
{
    uint32_t ssobj;
    glGenSamplers(1, &ssobj);

    glSamplerParameteri(ssobj, GL_TEXTURE_WRAP_S, (info.flags & SAMPLER_CLAMP_S) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glSamplerParameteri(ssobj, GL_TEXTURE_WRAP_T, (info.flags & SAMPLER_CLAMP_T) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glSamplerParameteri(ssobj, GL_TEXTURE_WRAP_R, (info.flags & SAMPLER_CLAMP_R) ? GL_CLAMP_TO_EDGE : GL_REPEAT);

    if(info.flags & SAMPLER_FILTER) {
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

    uvre::Sampler sampler(new uvre::Sampler_S, destroySampler);
    sampler->ssobj = ssobj;

    return sampler;
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

uvre::Texture uvre::RenderDeviceImpl::createTexture(const uvre::TextureCreateInfo &info)
{
    uint32_t texobj;
    uint32_t format = getInternalFormat(info.format);
    uint32_t target;
    int32_t mip_levels = std::max<int32_t>(1, static_cast<int32_t>(info.mip_levels));

    // TODO: account mip levels somehow

    glGenTextures(1, &texobj);
    switch(info.type) {
        case uvre::TextureType::TEXTURE_2D:
            target = GL_TEXTURE_2D;
            glBindTexture(target, texobj);
            glTexImage2D(target, 0, format, info.width, info.height, 0, GL_RED, GL_FLOAT, nullptr);
            break;
        case uvre::TextureType::TEXTURE_CUBE:
            target = GL_TEXTURE_CUBE_MAP;
            glBindTexture(target, texobj);
            glTexImage2D(target, 0, format, info.width, info.height, 0, GL_RED, GL_FLOAT, nullptr);
            break;
        case uvre::TextureType::TEXTURE_ARRAY:
            target = GL_TEXTURE_2D_ARRAY;
            glBindTexture(target, texobj);
            glTexImage3D(target, 0, format, info.width, info.height, info.depth, 0, GL_RED, GL_FLOAT, nullptr);
            break;
        default:
            return nullptr;
    }

    uvre::Texture texture(new uvre::Texture_S, destroyTexture);
    texture->texobj = texobj;
    texture->format = format;
    texture->target = target;
    texture->width = info.width;
    texture->height = info.height;
    texture->depth = info.depth;

    return texture;
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

void uvre::RenderDeviceImpl::writeTexture2D(uvre::Texture texture, int x, int y, int w, int h, uvre::PixelFormat format, const void *data)
{
    uint32_t fmt, type;
    if(!getExternalFormat(format, fmt, type))
        return;
    glBindTexture(GL_TEXTURE_2D, texture->texobj);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, fmt, type, data);
}

void uvre::RenderDeviceImpl::writeTextureCube(uvre::Texture texture, int face, int x, int y, int w, int h, uvre::PixelFormat format, const void *data)
{
    uint32_t fmt, type;
    if(!getExternalFormat(format, fmt, type))
        return;
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture->texobj);
    glTexSubImage3D(GL_TEXTURE_CUBE_MAP, 0, x, y, face, w, h, 1, fmt, type, data);
}

void uvre::RenderDeviceImpl::writeTextureArray(uvre::Texture texture, int x, int y, int z, int w, int h, int d, uvre::PixelFormat format, const void *data)
{
    uint32_t fmt, type;
    if(!getExternalFormat(format, fmt, type))
        return;
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture->texobj);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, x, y, z, w, h, d, fmt, type, data);
}

uvre::RenderTarget uvre::RenderDeviceImpl::createRenderTarget(const uvre::RenderTargetCreateInfo &info)
{
    uint32_t fbobj;
    glGenFramebuffers(1, &fbobj);

    glBindFramebuffer(GL_FRAMEBUFFER, fbobj);

    if(info.depth_attachment)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, info.depth_attachment->texobj, 0);
    if(info.stencil_attachment)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, info.stencil_attachment->texobj, 0);
    for(size_t i = 0; i < info.num_color_attachments; i++)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + info.color_attachments[i].id, GL_TEXTURE_2D, info.color_attachments[i].color->texobj, 0);

    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fbobj);
        return nullptr;
    }

    uvre::RenderTarget target(new uvre::RenderTarget_S, destroyRenderTarget);
    target->fbobj = fbobj;

    return target;
}

uvre::ICommandList *uvre::RenderDeviceImpl::createCommandList()
{
    uvre::CommandListImpl *commands = new uvre::CommandListImpl();
    commandlists.push_back(commands);
    return commands;
}

void uvre::RenderDeviceImpl::destroyCommandList(uvre::ICommandList *commands)
{
    for(std::vector<uvre::CommandListImpl *>::const_iterator it = commandlists.cbegin(); it != commandlists.cend(); it++) {
        if(*it == commands) {
            commandlists.erase(it);
            delete commands;
            return;
        }
    }
}

void uvre::RenderDeviceImpl::startRecording(uvre::ICommandList *commands)
{
    uvre::CommandListImpl *glcommands = static_cast<uvre::CommandListImpl *>(commands);
    glcommands->num_commands = 0;
}

void uvre::RenderDeviceImpl::submit(uvre::ICommandList *commands)
{
    int32_t last_binding;
    uvre::CommandListImpl *glcommands = static_cast<uvre::CommandListImpl *>(commands);
    for(size_t i = 0; i < glcommands->num_commands; i++) {
        const uvre::Command &cmd = glcommands->commands[i];
        uvre::VertexArray_S *vaonode = nullptr;
        switch(cmd.type) {
            case uvre::CommandType::SET_SCISSOR:
                glScissor(cmd.scvp.x, cmd.scvp.y, cmd.scvp.w, cmd.scvp.h);
                break;
            case uvre::CommandType::SET_VIEWPORT:
                glViewport(cmd.scvp.x, cmd.scvp.y, cmd.scvp.w, cmd.scvp.h);
                break;
            case uvre::CommandType::SET_CLEAR_DEPTH:
                glClearDepth(static_cast<GLdouble>(cmd.depth));
                break;
            case uvre::CommandType::SET_CLEAR_COLOR:
                glClearColor(cmd.color[0], cmd.color[1], cmd.color[2], cmd.color[3]);
                break;
            case uvre::CommandType::CLEAR:
                glClear(cmd.clear_mask);
                break;
            case uvre::CommandType::BIND_PIPELINE:
                bound_pipeline = cmd.pipeline;
                glDisable(GL_BLEND);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                glDisable(GL_SCISSOR_TEST);
                if(bound_pipeline.blending.enabled) {
                    glEnable(GL_BLEND);
                    glBlendEquation(bound_pipeline.blending.equation);
                    glBlendFunc(bound_pipeline.blending.sfactor, bound_pipeline.blending.dfactor);
                }
                if(bound_pipeline.depth_testing.enabled) {
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(bound_pipeline.depth_testing.func);
                }
                if(bound_pipeline.face_culling.enabled) {
                    glEnable(GL_CULL_FACE);
                    glCullFace(bound_pipeline.face_culling.cull_face);
                    glFrontFace(bound_pipeline.face_culling.front_face);
                }
                if(bound_pipeline.scissor_test)
                    glEnable(GL_SCISSOR_TEST);
                glPolygonMode(GL_FRONT_AND_BACK, bound_pipeline.fill_mode);
                glUseProgram(bound_pipeline.program);
                break;
            case uvre::CommandType::BIND_UNIFORM_BUFFER:
                glBindBufferBase(GL_UNIFORM_BUFFER, cmd.bind_index, cmd.object);
                break;
            case uvre::CommandType::BIND_INDEX_BUFFER: // OPTIMIZE
                bound_pipeline.bound_ibo = cmd.object;
                break;
            case uvre::CommandType::BIND_VERTEX_BUFFER: // OPTIMIZE
                vaonode = getVertexArray(&bound_pipeline.vaos, cmd.buffer.vbo->index / max_vbo_bindings, &bound_pipeline);
                if(vaonode->vaobj != bound_pipeline.bound_vao) {
                    bound_pipeline.bound_vao = vaonode->vaobj;
                    glBindVertexArray(vaonode->vaobj);
                }
                if(vaonode->vbobj != cmd.buffer.bufobj) {
                    vaonode->vbobj = cmd.buffer.bufobj;
                    for(size_t j = 0; j < bound_pipeline.num_attributes; j++)
                        glVertexAttribBinding(bound_pipeline.attributes[j].id, cmd.buffer.vbo->index % max_vbo_bindings);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bound_pipeline.bound_ibo);
                }
                break;
            case uvre::CommandType::BIND_SAMPLER:
                glBindSampler(cmd.bind_index, cmd.object);
                break;
            case uvre::CommandType::BIND_TEXTURE:
                glActiveTexture(GL_TEXTURE0 + cmd.bind_index);
                glBindTexture(cmd.tex_target, cmd.object);
                break;
            case uvre::CommandType::BIND_RENDER_TARGET:
                glBindFramebuffer(GL_FRAMEBUFFER, cmd.object);
                break;
            case uvre::CommandType::WRITE_BUFFER:
                glBindBuffer(GL_COPY_READ_BUFFER, cmd.buffer_write.buffer);
                glBufferSubData(GL_COPY_READ_BUFFER, static_cast<GLintptr>(cmd.buffer_write.offset), static_cast<GLsizeiptr>(cmd.buffer_write.size), cmd.buffer_write.data_ptr);
                break;
            case uvre::CommandType::COPY_RENDER_TARGET:
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_binding);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, cmd.rt_copy.src);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cmd.rt_copy.dst);
                glBlitFramebuffer(cmd.rt_copy.sx0, cmd.rt_copy.sy0, cmd.rt_copy.sx1, cmd.rt_copy.sy1, cmd.rt_copy.dx0, cmd.rt_copy.dy0, cmd.rt_copy.dx1, cmd.rt_copy.dy1, cmd.rt_copy.mask, cmd.rt_copy.filter);
                glBindFramebuffer(GL_FRAMEBUFFER, static_cast<uint32_t>(last_binding));
                break;
            case uvre::CommandType::DRAW:
                glDrawArraysInstancedBaseInstance(bound_pipeline.primitive_mode, cmd.draw.a.base_vertex, cmd.draw.a.vertices, cmd.draw.a.instances, cmd.draw.a.base_instance);
                break;
            case uvre::CommandType::IDRAW:
                glDrawElementsInstancedBaseVertexBaseInstance(bound_pipeline.primitive_mode, cmd.draw.e.indices, bound_pipeline.index_type, reinterpret_cast<const void *>(static_cast<uintptr_t>(bound_pipeline.index_size * cmd.draw.e.base_index)), cmd.draw.e.instances, cmd.draw.e.base_vertex, cmd.draw.e.base_instance);
                break;
        }
    }
}

void uvre::RenderDeviceImpl::prepare()
{
    // Third-party overlay applications
    // can cause mayhem if this is not called.
    glUseProgram(0);
}

void uvre::RenderDeviceImpl::present()
{
    create_info.gl.swapBuffers(create_info.gl.user_data);
}

void uvre::RenderDeviceImpl::vsync(bool enable)
{
    create_info.gl.setSwapInterval(create_info.gl.user_data, enable ? 1 : 0);
}

void uvre::RenderDeviceImpl::mode(int, int)
{
    // Nothing in OpenGL
}
