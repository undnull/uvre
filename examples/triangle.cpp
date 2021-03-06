/*
 * Copyright (c) 2021, Kirill GPRB.
 * All Rights Reserved.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include <uvre/uvre.hpp>
#include <GLFW/glfw3.h>
#include <exception>
#include <iostream>

using vec2_t = float[2];
struct vertex final {
    vec2_t position;
    vec2_t texcoord;
};

// Vertex shader source
static const char *vert_source = R"(
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;
out VS_OUTPUT {
    vec2 texcoord;
} vert;
void main()
{
    vert.texcoord = texcoord;
    gl_Position = vec4(position, 0.0, 1.0);
})";

// Fragment shader source
static const char *frag_source = R"(
layout(location = 0) out vec4 target;
in VS_OUTPUT {
    vec2 texcoord;
} vert;
void main()
{
    target = vec4(vert.texcoord, 1.0, 1.0);
})";

// GLFW error callback
static void onGlfwError(int, const char *message)
{
    std::cerr << message << std::endl;
}

// Debug callback
static void onDebugMessage(const uvre::DebugMessageInfo &msg)
{
    std::cout << msg.text << std::endl;
}

int main()
{
    // Initialize GLFW
    glfwSetErrorCallback(onGlfwError);
    if(!glfwInit())
        std::terminate();

    // Since UVRE is windowing API-agnostic,
    // some information must be passed back
    // to the windowing API in order for it
    // to be correctly set up for UVRE.
    uvre::ImplInfo impl_info;
    uvre::pollImplInfo(impl_info);

    // Do not require any client API by default
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Non-resizable.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    // If the implementation is OpenGL-ish
    if(impl_info.family == uvre::ImplFamily::OPENGL) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_OPENGL_PROFILE, impl_info.gl.core_profile ? GLFW_OPENGL_CORE_PROFILE : GLFW_OPENGL_COMPAT_PROFILE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, impl_info.gl.version_major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, impl_info.gl.version_minor);

#if defined(__APPLE__)
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    }

    constexpr const int WINDOW_WIDTH = 640;
    constexpr const int WINDOW_HEIGHT = 480;

    // Open a new window
    GLFWwindow *window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "UVRE - Triangle", nullptr, nullptr);
    if(!window)
        std::terminate();

    // Now the windowing API also needs to pass some
    // data to the library before creating a rendering
    // device. Usually this data is API-specific callbacks.
    uvre::DeviceCreateInfo device_info = {};

    // OpenGL-specific callbacks
    if(impl_info.family == uvre::ImplFamily::OPENGL) {
        device_info.gl.user_data = window;
        device_info.gl.getProcAddr = [](void *, const char *procname) { return reinterpret_cast<void *>(glfwGetProcAddress(procname)); };
        device_info.gl.makeContextCurrent = [](void *arg) { glfwMakeContextCurrent(reinterpret_cast<GLFWwindow *>(arg)); };
        device_info.gl.setSwapInterval = [](void *, int interval) { glfwSwapInterval(interval); };
        device_info.gl.swapBuffers = [](void *arg) { glfwSwapBuffers(reinterpret_cast<GLFWwindow *>(arg)); };
    }

    // Message callback
    device_info.onDebugMessage = &onDebugMessage;

    // Now we create the rendering device.
    // Rendering device is an object that works
    // with other objects: creates them, destroys
    // them, operates with their internal data, etc.
    uvre::IRenderDevice *device = uvre::createDevice(device_info);
    if(!device)
        std::terminate();

    // After the rendering device is created and initialized,
    // we need to create a command list object. A command list
    // is an object which whole purpose is to record drawing
    // commands and then submit them to the implementation.
    uvre::ICommandList *commands = device->createCommandList();

    // Now let's talk about how UVRE manages object creation.
    // Unlike createCommandList, any other creation function
    // requires an additional information structure to be passed
    // in order to create a valid object.
    // Some structures may have default values set to their fields
    // but I advise you to fill every field manually (except for
    // maybe LOD levels in uvre::TextureCreateInfo structure).

    // Start a new scope so the objects are safely killed after use.
    {
        // Vertex shader creation info.
        uvre::ShaderCreateInfo vert_info = {};
        vert_info.stage = uvre::ShaderStage::VERTEX;
        vert_info.format = uvre::ShaderFormat::SOURCE_GLSL;
        vert_info.code = vert_source;

        // Fragment shader creation info.
        uvre::ShaderCreateInfo frag_info = {};
        frag_info.stage = uvre::ShaderStage::FRAGMENT;
        frag_info.format = uvre::ShaderFormat::SOURCE_GLSL;
        frag_info.code = frag_source;

        // Now we create the shaders using structures we've
        // set up previously. These shaders are in an array
        // because pipelines require shaders to be in them.
        uvre::Shader shaders[2];
        shaders[0] = device->createShader(vert_info);
        shaders[1] = device->createShader(frag_info);

        // Now it's time to set up the pipeline object.
        // Pipeline objects cover up virtually everything related
        // to shaders, blending, depth testing and rasterization.

        // Vertex format description.
        // The vertex we've defined has two fields, thus we
        // create an array of two attributes with respective offsets.
        uvre::VertexAttrib attributes[2];
        attributes[0] = uvre::VertexAttrib { 0, uvre::VertexAttribType::FLOAT32, 2, offsetof(vertex, position), false };
        attributes[1] = uvre::VertexAttrib { 1, uvre::VertexAttribType::FLOAT32, 2, offsetof(vertex, texcoord), false };

        // Pipeline creation info.
        uvre::PipelineCreateInfo pipeline_info = {};
        pipeline_info.blending.enabled = false;
        pipeline_info.depth_testing.enabled = false;
        pipeline_info.face_culling.enabled = false;
        pipeline_info.index_type = uvre::IndexType::INDEX16;
        pipeline_info.primitive_mode = uvre::PrimitiveMode::TRIANGLES;
        pipeline_info.fill_mode = uvre::FillMode::WIREFRAME;
        pipeline_info.vertex_stride = sizeof(vertex);
        pipeline_info.num_vertex_attribs = 2;
        pipeline_info.vertex_attribs = attributes;
        pipeline_info.num_shaders = 2;
        pipeline_info.shaders = shaders;

        // And again, a creation function inputs this large
        // amount of data that it'd be easier to just pass
        // a structure containing all this data.
        uvre::Pipeline pipeline = device->createPipeline(pipeline_info);

        // Triangle vertices.
        // The coordinates are NDC.
        const vertex vertices[3] = {
            vertex { { -0.8f, -0.8f }, { 0.0f, 1.0f } },
            vertex { { 0.0f, 0.8f }, { 0.5f, 0.0f } },
            vertex { { 0.8f, -0.8f }, { 1.0f, 1.0f } },
        };

        // Vertex buffer creation info.
        uvre::BufferCreateInfo vbo_info = {};
        vbo_info.type = uvre::BufferType::VERTEX_BUFFER;
        vbo_info.size = sizeof(vertices);
        vbo_info.data = vertices;

        // Create the vertex buffer.
        // Unlike OpenGL, UVRE has no concept of Vertex Array
        // objects exposed to its API. Instead, a global VAO
        // is used for each pipeline object.
        uvre::Buffer vbo = device->createBuffer(vbo_info);

        // Color attachment creation info.
        uvre::TextureCreateInfo color_info = {};
        color_info.type = uvre::TextureType::TEXTURE_2D;
        color_info.format = uvre::PixelFormat::R16G16B16_UNORM;
        color_info.width = WINDOW_WIDTH;
        color_info.height = WINDOW_HEIGHT;

        // Color attachment structure
        uvre::ColorAttachment color_attachment = {};
        color_attachment.id = 0;
        color_attachment.color = device->createTexture(color_info);

        // Render target creation info.
        uvre::RenderTargetCreateInfo target_info = {};
        target_info.num_color_attachments = 1;
        target_info.color_attachments = &color_attachment;

        // Create the render target
        uvre::RenderTarget target = device->createRenderTarget(target_info);

        // Now the main loop. It should look pretty much the same
        // for all the implementations. UVRE is not an exception.
        while(!glfwWindowShouldClose(window)) {
            // Prepare the state to a new frame
            device->prepare();

            // Begin recording drawing commands
            // This does nothing for OpenGL.
            device->startRecording(commands);

            // Bind the render target and set viewport.
            // Now every draw operation will output to the RT.
            commands->bindRenderTarget(target);
            commands->setViewport(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);

            // Clear the RT with a nice black color
            commands->setClearColor3f(0.0f, 0.0f, 0.0f);
            commands->clear(uvre::RT_COLOR_BUFFER);

            // Bind and draw
            commands->bindPipeline(pipeline);
            commands->bindVertexBuffer(vbo);
            commands->draw(3, 1, 0, 0);

            // Unbind the render target and set viewport.
            // Now every draw operation will output to the screen.
            commands->bindRenderTarget(nullptr);
            commands->setViewport(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);

            // Clear the screen with a nice dark magenta color.
            commands->setClearColor3f(0.5f, 0.0f, 0.5f);
            commands->clear(uvre::RT_COLOR_BUFFER);

            // Copy or "blit" the render target to the screen.
            // We'll leave a small 16px gap to indicate that it works.
            commands->copyRenderTarget(target, nullptr, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, 32, 32, WINDOW_WIDTH - 32, WINDOW_HEIGHT - 32, uvre::RT_COLOR_BUFFER, true);

            // Finish recording and submit the command list.
            // This does nothing for OpenGL.
            device->submit(commands);

            // Finish the frame
            device->present();

            // Handle window's events
            glfwPollEvents();
        }
    }

    // Destroy the command list
    device->destroyCommandList(commands);

    // Destroy the device
    uvre::destroyDevice(device);

    // Destroy the window
    glfwDestroyWindow(window);

    // De-init GLFW
    glfwTerminate();

    return 0;
}
