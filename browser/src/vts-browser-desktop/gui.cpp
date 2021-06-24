/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <cstring>
#include <set>

#include <vts-browser/enumNames.hpp>
#include <vts-browser/mapStatistics.hpp>
#include <vts-browser/mapView.hpp>
#include <vts-browser/cameraStatistics.hpp>
#include <vts-browser/cameraDraws.hpp>
#include <vts-browser/search.hpp>
#include <vts-browser/celestial.hpp>
#include <vts-browser/position.hpp>

#include "mainWindow.hpp"
#include "guiSkin.hpp"
#include "editor.hpp"

#include <GLFW/glfw3.h>

using namespace vts;
using namespace renderer;

namespace
{

constexpr const nk_rune FontUnicodeRanges[] = {
    // 0x0020, 0x007F, // Basic Latin
    // 0x00A0, 0x00FF, // Latin-1 Supplement
    // 0x0100, 0x017F, // Latin Extended-A
    // 0x0180, 0x024F, // Latin Extended-B
    // 0x0300, 0x036F, // Combining Diacritical Marks
    // 0x0400, 0x04FF, // Cyrillic
    0x0001, 0x5000, // all multilingual characters
    0
};

constexpr const char *ControlOptionsPath = "vts-browser-desktop.control-options.json";

constexpr const char *LodBlendingModeNames[] = {
    "off",
    "basic",
    "precise",
};

constexpr const char *GeodataDebugNames[] = {
    "off",
    "importance",
    "rects",
    "glyphs",
};

constexpr const char *FpsSlowdownNames[] = {
    "off",
    "on",
    "periodic",
};

void clipboardPaste(nk_handle, struct nk_text_edit *edit)
{
    const char *text = glfwGetClipboardString(nullptr);
    if (text)
        nk_textedit_paste(edit, text, strlen(text));
}

void clipboardCopy(nk_handle, const char *text, int len)
{
    assert(len < 300);
    char buffer[301];
    memcpy(buffer, text, len);
    buffer[len] = 0;
    glfwSetClipboardString(nullptr, buffer);
}

} // namespace

class GuiImpl
{
public:
    struct vertex
    {
        float position[2];
        float uv[2];
        nk_byte col[4];
    };

    GuiImpl(MainWindow *window) : window(window)
    {
        gladLoadGLLoader((GLADloadproc)&glfwGetProcAddress);

        searchText[0] = 0;
        searchTextPrev[0] = 0;
        positionInputText[0] = 0;

        // load font
        {
            struct nk_font_config cfg = nk_font_config(0);
            cfg.oversample_h = 3;
            cfg.oversample_v = 2;
            cfg.range = FontUnicodeRanges;
            nk_font_atlas_init_default(&atlas);
            nk_font_atlas_begin(&atlas);
            Buffer buffer = readInternalMemoryBuffer("data/fonts/Roboto-Regular.ttf");
            font = nk_font_atlas_add_from_memory(&atlas, buffer.data(), buffer.size(), 14, &cfg);
            GpuTextureSpec spec;
            static_assert(sizeof(int) == sizeof(uint32), "incompatible reinterpret cast");
            const void* img = nk_font_atlas_bake(&atlas, (int*)&spec.width, (int*)&spec.height, NK_FONT_ATLAS_RGBA32);
            spec.components = 4;
            spec.buffer.allocate(spec.width * spec.height * spec.components);
            memcpy(spec.buffer.data(), img, spec.buffer.size());
            fontTexture = std::make_shared<Texture>();
            vts::ResourceInfo ri;
            fontTexture->load(ri, spec, "data/fonts/Roboto-Regular.ttf");
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            nk_font_atlas_end(&atlas, nk_handle_id(fontTexture->getId()), &null);
        }

        nk_init_default(&ctx, &font->handle);
        nk_buffer_init_default(&cmds);

        ctx.clip.paste = &clipboardPaste;
        ctx.clip.copy = &clipboardCopy;
        ctx.clip.userdata.ptr = window->window;

        static const nk_draw_vertex_layout_element vertex_layout[] =
        {
            { NK_VERTEX_POSITION, NK_FORMAT_FLOAT, 0 },
            { NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, 8 },
            { NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, 16 },
            { NK_VERTEX_LAYOUT_END }
        };
        memset(&config, 0, sizeof(config));
        config.vertex_layout = vertex_layout;
        config.vertex_size = sizeof(vertex);
        config.vertex_alignment = alignof(vertex);
        config.circle_segment_count = 22;
        config.curve_segment_count = 22;
        config.arc_segment_count = 22;
        config.global_alpha = 1.0f;
        config.shape_AA = NK_ANTI_ALIASING_ON;
        config.line_AA = NK_ANTI_ALIASING_ON;
        config.null = null;

        initializeGuiSkin(ctx, skinMedia, skinTexture);

        // load shader
        {
            shader = std::make_shared<Shader>();
            shader->setDebugId("data/shaders/gui.*.glsl");
            Buffer vert = readInternalMemoryBuffer("data/shaders/gui.vert.glsl");
            Buffer frag = readInternalMemoryBuffer("data/shaders/gui.frag.glsl");
            shader->load(std::string(vert.data(), vert.size()), std::string(frag.data(), frag.size()));
            std::vector<uint32> &uls = shader->uniformLocations;
            GLuint id = shader->getId();
            uls.push_back(glGetUniformLocation(id, "ProjMtx"));
            glUseProgram(id);
            glUniform1i(glGetUniformLocation(id, "Texture"), 0);
        }

        // prepare mesh buffers
        {
            vts::GpuMeshSpec spec;
            spec.attributes[0].enable = true;
            spec.attributes[0].components = 2;
            spec.attributes[0].type = vts::GpuTypeEnum::Float;
            spec.attributes[0].normalized = false;
            spec.attributes[0].stride = sizeof(vertex);
            spec.attributes[0].offset = 0;
            spec.attributes[1].enable = true;
            spec.attributes[1].components = 2;
            spec.attributes[1].type = vts::GpuTypeEnum::Float;
            spec.attributes[1].normalized = false;
            spec.attributes[1].stride = sizeof(vertex);
            spec.attributes[1].offset = 8;
            spec.attributes[2].enable = true;
            spec.attributes[2].components = 4;
            spec.attributes[2].type = vts::GpuTypeEnum::UnsignedByte;
            spec.attributes[2].normalized = true;
            spec.attributes[2].stride = sizeof(vertex);
            spec.attributes[2].offset = 16;
            spec.verticesCount = 1;
            spec.indicesCount = 1;
            vts::ResourceInfo info;
            mesh = std::make_shared<Mesh>();
            mesh->load(info, spec, "guiMesh");
            glBufferData(GL_ARRAY_BUFFER, MaxVertexMemory, NULL, GL_STREAM_DRAW);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, MaxElementMemory, NULL, GL_STREAM_DRAW);
            mesh->setDebugId("guiMesh");
        }

        // load control options
        try
        {
            window->navigation->options().applyJson(readLocalFileBuffer(ControlOptionsPath).str());
        }
        catch(...)
        {
            // do nothing
        }
    }

    ~GuiImpl()
    {
        nk_buffer_free(&cmds);
        nk_font_atlas_clear(&atlas);
        nk_free(&ctx);
    }

    const char *getClipboard()
    {
        return glfwGetClipboardString(window->window);
    }

    void dispatch(int width, int height)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_SCISSOR_TEST);
        glActiveTexture(GL_TEXTURE0);
        mesh->bind();
        shader->bind();

        // proj matrix
        {
            GLfloat ortho[4][4] = {
                    {2.0f, 0.0f, 0.0f, 0.0f},
                    {0.0f,-2.0f, 0.0f, 0.0f},
                    {0.0f, 0.0f,-1.0f, 0.0f},
                    {-1.0f,1.0f, 0.0f, 1.0f},
            };
            ortho[0][0] *= (GLfloat)(scale / width);
            ortho[1][1] *= (GLfloat)(scale / height);
            glUniformMatrix4fv(shader->uniformLocations[0], 1, GL_FALSE, &ortho[0][0]);
        }

        // upload buffer data
        {
            void *vertices = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
            void *elements = glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_WRITE_ONLY);
            nk_buffer vbuf, ebuf;
            nk_buffer_init_fixed(&vbuf, vertices, MaxVertexMemory);
            nk_buffer_init_fixed(&ebuf, elements, MaxElementMemory);
            nk_convert(&ctx, &cmds, &vbuf, &ebuf, &config);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        }

        // draw commands
        {
            const nk_draw_command *cmd = nullptr;
            const nk_draw_index *offset = nullptr;
            nk_draw_foreach(cmd, &ctx, &cmds)
            {
                if (!cmd->elem_count)
                    continue;
                glBindTexture(GL_TEXTURE_2D, cmd->texture.id);
                glScissor(
                    (GLint)(cmd->clip_rect.x * scale),
                    (GLint)(height - (cmd->clip_rect.y + cmd->clip_rect.h) * scale),
                    (GLint)(cmd->clip_rect.w * scale),
                    (GLint)(cmd->clip_rect.h * scale));
                glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count, GL_UNSIGNED_SHORT, offset);
                offset += cmd->elem_count;
            }
        }

        nk_clear(&ctx);

        glUseProgram(0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
    }

    void prepareOptions()
    {
        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Options", nk_rect(10, 10, 250, 600), flags))
        {
            MapRuntimeOptions &mr = window->map->options();
            CameraOptions &c = window->camera->options();
            NavigationOptions &n = window->navigation->options();
            AppOptions &a = window->appOptions;
            renderer::RenderOptions &r = window->view->options();
            const float width = nk_window_get_content_region_size(&ctx).x - 30;
            char buffer[256];

            // camera control sensitivity
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Mouse Sensitivity", NK_MINIMIZED))
            {
                const float ratio[] = { width * 0.4f, width * 0.45f, width * 0.15f };
                nk_layout_row(&ctx, NK_STATIC, 16, 3, ratio);

                // sensitivity
                nk_label(&ctx, "Pan speed:", NK_TEXT_LEFT);
                n.sensitivityPan = nk_slide_float(&ctx, 0.1, n.sensitivityPan, 3, 0.01);
                sprintf(buffer, "%4.2f", n.sensitivityPan);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Zoom speed:", NK_TEXT_LEFT);
                n.sensitivityZoom = nk_slide_float(&ctx, 0.1, n.sensitivityZoom, 3, 0.01);
                sprintf(buffer, "%4.2f", n.sensitivityZoom);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Rotate speed:", NK_TEXT_LEFT);
                n.sensitivityRotate = nk_slide_float(&ctx, 0.1, n.sensitivityRotate, 3, 0.01);
                sprintf(buffer, "%4.2f", n.sensitivityRotate);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                // inertia
                nk_label(&ctx, "Pan inertia:", NK_TEXT_LEFT);
                n.inertiaPan = nk_slide_float(&ctx, 0, n.inertiaPan, 0.99, 0.01);
                sprintf(buffer, "%4.2f", n.inertiaPan);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Zoom inertia:", NK_TEXT_LEFT);
                n.inertiaZoom = nk_slide_float(&ctx, 0, n.inertiaZoom, 0.99, 0.01);
                sprintf(buffer, "%4.2f", n.inertiaZoom);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Rotate inertia:", NK_TEXT_LEFT);
                n.inertiaRotate = nk_slide_float(&ctx, 0, n.inertiaRotate, 0.99, 0.01);
                sprintf(buffer, "%4.2f", n.inertiaRotate);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);

                // save
                if (nk_button_label(&ctx, "Save"))
                {
                    try
                    {
                        writeLocalFileBuffer(ControlOptionsPath, Buffer(n.toJson()));
                    }
                    catch(...)
                    {
                        // do nothing
                    }
                }

                // load
                if (nk_button_label(&ctx, "Load"))
                {
                    try
                    {
                        n.applyJson(readLocalFileBuffer(ControlOptionsPath).str());
                    }
                    catch(...)
                    {
                        // do nothing
                    }
                }

                // reset
                if (nk_button_label(&ctx, "Reset"))
                {
                    NavigationOptions d;
                    n.sensitivityPan       = d.sensitivityPan;
                    n.sensitivityZoom      = d.sensitivityZoom;
                    n.sensitivityRotate    = d.sensitivityRotate;
                    n.inertiaPan           = d.inertiaPan;
                    n.inertiaZoom          = d.inertiaZoom;
                    n.inertiaRotate        = d.inertiaRotate;
                }

                // end group
                nk_tree_pop(&ctx);
            }

            // navigation
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Navigation", NK_MINIMIZED))
            {
                {
                    const float ratio[] = { width * 0.4f, width * 0.6f };
                    nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);

                    // navigation type
                    nk_label(&ctx, "Nav. type:", NK_TEXT_LEFT);
                    if (nk_combo_begin_label(&ctx, NavigationTypeNames[(int)n.type], nk_vec2(nk_widget_width(&ctx), 200)))
                    {
                        nk_layout_row_dynamic(&ctx, 16, 1);
                        for (unsigned i = 0; i < sizeof(NavigationTypeNames) / sizeof(NavigationTypeNames[0]); i++)
                        {
                            if (nk_combo_item_label(&ctx, NavigationTypeNames[i], NK_TEXT_LEFT))
                                n.type = (NavigationType)i;
                        }
                        nk_combo_end(&ctx);
                    }

                    // navigation mode
                    nk_label(&ctx, "Nav. mode:", NK_TEXT_LEFT);
                    if (nk_combo_begin_label(&ctx, NavigationModeNames[(int)n.mode], nk_vec2(nk_widget_width(&ctx), 200)))
                    {
                        nk_layout_row_dynamic(&ctx, 16, 1);
                        for (unsigned i = 0; i < sizeof(NavigationModeNames) / sizeof(NavigationModeNames[0]); i++)
                        {
                            if (nk_combo_item_label(&ctx, NavigationModeNames[i],NK_TEXT_LEFT))
                                n.mode = (NavigationMode)i;
                        }
                        nk_combo_end(&ctx);
                    }
                }

                {
                    const float ratio[] = { width * 0.4f, width * 0.45f, width * 0.15f };
                    nk_layout_row(&ctx, NK_STATIC, 16, 3, ratio);

                    // flyOverSpikinessFactor
                    nk_label(&ctx, "FlyOver spikiness:", NK_TEXT_LEFT);
                    n.flyOverSpikinessFactor = nk_slide_float(&ctx, 0.1, n.flyOverSpikinessFactor, 20, 0.1);
                    sprintf(buffer, "%5.3f", n.flyOverSpikinessFactor);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                    // flyOverMotionChangeFraction
                    nk_label(&ctx, "FlyOver move:", NK_TEXT_LEFT);
                    n.flyOverMotionChangeFraction = nk_slide_float(&ctx, 0.1, n.flyOverMotionChangeFraction, 2, 0.01);
                    sprintf(buffer, "%5.3f", n.flyOverMotionChangeFraction);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                    // flyOverRotationChangeSpeed
                    nk_label(&ctx, "FlyOver rotation:", NK_TEXT_LEFT);
                    n.flyOverRotationChangeSpeed = nk_slide_float(&ctx, 0.1, n.flyOverRotationChangeSpeed, 2, 0.01);
                    sprintf(buffer, "%5.3f", n.flyOverRotationChangeSpeed);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                    // navigation samples per view extent
                    nk_label(&ctx, "Nav. samples:", NK_TEXT_LEFT);
                    c.samplesForAltitudeLodSelection = nk_slide_float(&ctx, 1, c.samplesForAltitudeLodSelection, 16, 1);
                    sprintf(buffer, "%4.1f", c.samplesForAltitudeLodSelection);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                }

                {
                    nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);

                    // fps compensation
                    n.fpsCompensation = nk_check_label(&ctx, "FPS compensation", n.fpsCompensation);

                    // enable camera normalization
                    n.enableNormalization = nk_check_label(&ctx, "Camera normalization", n.enableNormalization);

                    // obstruction prevention
                    n.enableObstructionPrevention = nk_check_label(&ctx, "Obstruction prevention", n.enableObstructionPrevention);

                    // altitude corrections
                    n.enableAltitudeCorrections = nk_check_label(&ctx, "Altitude corrections", n.enableAltitudeCorrections);
                }

                {
                    const float ratio[] = { width * 0.4f, width * 0.45f, width * 0.15f };
                    nk_layout_row(&ctx, NK_STATIC, 16, 3, ratio);

                    // obstructionPreventionSmoothingDuration
                    nk_label(&ctx, "Smooth duration:", NK_TEXT_LEFT);
                    n.obstructionPreventionSmoothingDuration = nk_slide_float(&ctx, 0, n.obstructionPreventionSmoothingDuration, 30, 0.05);
                    sprintf(buffer, "%5.2f", n.obstructionPreventionSmoothingDuration);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                    // altitude fade out
                    nk_label(&ctx, "Altitude fade:", NK_TEXT_LEFT);
                    n.altitudeFadeOutFactor = nk_slide_float(&ctx, 0, n.altitudeFadeOutFactor, 1, 0.01);
                    sprintf(buffer, "%4.2f", n.altitudeFadeOutFactor);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                }

                // end group
                nk_tree_pop(&ctx);
            }

            // rendering
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Rendering", NK_MINIMIZED))
            {
                {
                    const float ratio[] = { width * 0.4f, width * 0.45f, width * 0.15f };
                    nk_layout_row(&ctx, NK_STATIC, 16, 3, ratio);

                    // traverse mode (surfaces)
                    nk_label(&ctx, "Surfaces:", NK_TEXT_LEFT);
                    if (nk_combo_begin_label(&ctx, TraverseModeNames[(int)c.traverseModeSurfaces], nk_vec2(nk_widget_width(&ctx), 200)))
                    {
                        nk_layout_row_dynamic(&ctx, 16, 1);
                        for (unsigned i = 0; i < sizeof(TraverseModeNames) / sizeof(TraverseModeNames[0]); i++)
                        {
                            if (nk_combo_item_label(&ctx, TraverseModeNames[i], NK_TEXT_LEFT))
                                c.traverseModeSurfaces = (TraverseMode)i;
                        }
                        nk_combo_end(&ctx);
                    }
                    nk_label(&ctx, "", NK_TEXT_RIGHT);

                    // targetPixelRatioSurfaces
                    nk_label(&ctx, "Target ratio:", NK_TEXT_LEFT);
                    c.targetPixelRatioSurfaces = nk_slide_float(&ctx, 0.3, c.targetPixelRatioSurfaces, 30, 0.1);
                    sprintf(buffer, "%3.1f", c.targetPixelRatioSurfaces);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                    // traverse mode (geodata)
                    nk_label(&ctx, "Geodata:", NK_TEXT_LEFT);
                    if (nk_combo_begin_label(&ctx, TraverseModeNames[(int)c.traverseModeGeodata], nk_vec2(nk_widget_width(&ctx), 200)))
                    {
                        nk_layout_row_dynamic(&ctx, 16, 1);
                        for (unsigned i = 0; i < sizeof(TraverseModeNames) / sizeof(TraverseModeNames[0]); i++)
                        {
                            if (nk_combo_item_label(&ctx, TraverseModeNames[i], NK_TEXT_LEFT))
                                c.traverseModeGeodata = (TraverseMode)i;
                        }
                        nk_combo_end(&ctx);
                    }
                    nk_label(&ctx, "", NK_TEXT_RIGHT);

                    // targetPixelRatioGeodata
                    nk_label(&ctx, "Target ratio:", NK_TEXT_LEFT);
                    c.targetPixelRatioGeodata = nk_slide_float(&ctx, 0.3, c.targetPixelRatioGeodata, 30, 0.1);
                    sprintf(buffer, "%3.1f", c.targetPixelRatioGeodata);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                    // fixed traversal
                    if (c.traverseModeSurfaces == TraverseMode::Fixed || c.traverseModeGeodata == TraverseMode::Fixed)
                    {
                        // fixedTraversalLod
                        nk_label(&ctx, "Fixed Lod:", NK_TEXT_LEFT);
                        c.fixedTraversalLod = nk_slide_int(&ctx, 0, c.fixedTraversalLod, 30, 1);
                        sprintf(buffer, "%d", c.fixedTraversalLod);
                        nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                        // fixedTraversalDistance
                        nk_label(&ctx, "Fixed distance:", NK_TEXT_LEFT);
                        c.fixedTraversalDistance = nk_slide_float(&ctx, 100, c.fixedTraversalDistance, 10000, 100);
                        sprintf(buffer, "%5.0f", c.fixedTraversalDistance);
                        nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                    }

                    // cullingOffsetDistance
                    nk_label(&ctx, "Culling offset:", NK_TEXT_LEFT);
                    c.cullingOffsetDistance = nk_slide_float(&ctx, 0.0, c.cullingOffsetDistance, 500, 1.0);
                    sprintf(buffer, "%3.1f", c.cullingOffsetDistance);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                    // antialiasing samples
                    nk_label(&ctx, "Antialiasing:", NK_TEXT_LEFT);
                    r.antialiasingSamples = nk_slide_int(&ctx, 1, r.antialiasingSamples, 16, 1);
                    if (r.antialiasingSamples > 1)
                    {
                        sprintf(buffer, "%d", r.antialiasingSamples);
                        nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                    }
                    else
                        nk_label(&ctx, "no", NK_TEXT_RIGHT);

                    // maxResourcesMemory
                    nk_label(&ctx, "Target memory:", NK_TEXT_LEFT);
                    mr.targetResourcesMemoryKB = 1024 * nk_slide_int(&ctx, 0, mr.targetResourcesMemoryKB / 1024, 8192, 128);
                    sprintf(buffer, "%3d", mr.targetResourcesMemoryKB / 1024);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                    // text scale
                    {
                        float &s = window->view->options().textScale;
                        nk_label(&ctx, "Text scale:", NK_TEXT_LEFT);
                        s = nk_slide_float(&ctx, 0.2, s, 5, 0.1);
                        sprintf(buffer, "%3.1f", s);
                        nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                    }
                }

                // end group
                nk_tree_pop(&ctx);
            }

            // display
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Display", NK_MINIMIZED))
            {
                nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);

                // render atmosphere
                r.renderAtmosphere = nk_check_label(&ctx, "Atmosphere", r.renderAtmosphere);

                // render mesh boxes
                c.debugRenderMeshBoxes = nk_check_label(&ctx, "Mesh boxes", c.debugRenderMeshBoxes);

                // render tile boxes
                c.debugRenderTileBoxes = nk_check_label(&ctx, "Tile boxes", c.debugRenderTileBoxes);

                // render surrogates
                c.debugRenderSurrogates = nk_check_label(&ctx, "Surrogates", c.debugRenderSurrogates);

                // render objective position
                n.debugRenderObjectPosition = nk_check_label(&ctx, "Objective position", n.debugRenderObjectPosition);

                // render target position
                n.debugRenderTargetPosition = nk_check_label(&ctx, "Target position", n.debugRenderTargetPosition);

                // altitude surrogates
                n.debugRenderAltitudeSurrogates = nk_check_label(&ctx, "Altitude surrogates", n.debugRenderAltitudeSurrogates);

                // obstruction surrogates
                n.debugRenderCameraObstructionSurrogates = nk_check_label(&ctx, "Obstruction surrogates", n.debugRenderCameraObstructionSurrogates);

                // flat shading
                r.debugFlatShading = nk_check_label(&ctx, "Flat shading", r.debugFlatShading);

                // polygon edges
                r.debugWireframe = nk_check_label(&ctx, "Wireframe", r.debugWireframe);

                // render compas
                nk_checkbox_label(&ctx, "Compas", &a.renderCompas);

                // end group
                nk_tree_pop(&ctx);
            }

            // Tile Diagnostics
            c.debugRenderTileDiagnostics = false;
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Tile diagnostics", NK_MINIMIZED))
            {
                c.debugRenderTileDiagnostics = true;

                float ratio2[] = { width * 0.45f, width * 0.45f };
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio2);

                // Bigger Text
                c.debugRenderTileBigText = nk_check_label(&ctx, "Big Text", c.debugRenderTileBigText);

                // Only Geodata
                c.debugRenderTileGeodataOnly = nk_check_label(&ctx, "Only Geodata", c.debugRenderTileGeodataOnly);

                // LODs
                c.debugRenderTileLod = nk_check_label(&ctx, "LOD", c.debugRenderTileLod);

                // Indices
                c.debugRenderTileIndices = nk_check_label(&ctx, "Indices", c.debugRenderTileIndices);

                // Texelsize
                c.debugRenderTileTexelSize = nk_check_label(&ctx, "Texel size", c.debugRenderTileTexelSize);

                // Face count
                c.debugRenderTileFaces = nk_check_label(&ctx, "Face count", c.debugRenderTileFaces);

                // Texture size
                c.debugRenderTileTextureSize = nk_check_label(&ctx, "Texture size", c.debugRenderTileTextureSize);

                // Surface
                c.debugRenderTileSurface = nk_check_label(&ctx, "Surface", c.debugRenderTileSurface);

                // Bound layer
                c.debugRenderTileBoundLayer = nk_check_label(&ctx, "Bound layer", c.debugRenderTileBoundLayer);

                // Credits
                c.debugRenderTileCredits = nk_check_label(&ctx, "Credits", c.debugRenderTileCredits);

                nk_tree_pop(&ctx);
            }

            // debug
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Debug", NK_MINIMIZED))
            {
                // simulated fps slowdown
                {
                    const float ratio[] = { width * 0.4f, width * 0.6f };
                    nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);

                    nk_label(&ctx, "FPS slowdown:", NK_TEXT_LEFT);
                    if (nk_combo_begin_label(&ctx, FpsSlowdownNames[(int)a.simulatedFpsSlowdown], nk_vec2(nk_widget_width(&ctx), 200)))
                    {
                        nk_layout_row_dynamic(&ctx, 16, 1);
                        for (unsigned i = 0; i < sizeof(FpsSlowdownNames) / sizeof(FpsSlowdownNames[0]); i++)
                        {
                            if (nk_combo_item_label(&ctx, FpsSlowdownNames[i], NK_TEXT_LEFT))
                                a.simulatedFpsSlowdown = i;
                        }
                        nk_combo_end(&ctx);
                    }
                }

                // geodata debug mode
                {
                    const float ratio[] = { width * 0.4f, width * 0.6f };
                    nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);

                    nk_label(&ctx, "Geodata:", NK_TEXT_LEFT);
                    if (nk_combo_begin_label(&ctx, GeodataDebugNames[(int)r.debugGeodataMode], nk_vec2(nk_widget_width(&ctx), 200)))
                    {
                        nk_layout_row_dynamic(&ctx, 16, 1);
                        for (unsigned i = 0; i < sizeof(GeodataDebugNames) / sizeof(GeodataDebugNames[0]); i++)
                        {
                            if (nk_combo_item_label(&ctx, GeodataDebugNames[i], NK_TEXT_LEFT))
                                r.debugGeodataMode = i;
                        }
                        nk_combo_end(&ctx);
                    }
                }

                nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);

                // geodata hysteresis
                r.geodataHysteresis = nk_check_label(&ctx, "Geodata hysteresis", r.geodataHysteresis);

                // camera zoom limit
                {
                    int e = viewExtentLimitScaleMax == std::numeric_limits<double>::infinity();
                    int ePrev = e;
                    nk_checkbox_label(&ctx, "Zoom limit", &e);
                    if (e != ePrev)
                    {
                        std::swap(viewExtentLimitScaleMin, n.viewExtentLimitScaleMin);
                        std::swap(viewExtentLimitScaleMax, n.viewExtentLimitScaleMax);
                    }
                }

                // detached camera
                c.debugDetachedCamera = nk_check_label(&ctx, "Detached camera", c.debugDetachedCamera);

                // virtual surfaces
                {
                    bool old = mr.debugVirtualSurfaces;
                    mr.debugVirtualSurfaces = nk_check_label(&ctx, "virtual surfaces", mr.debugVirtualSurfaces);
                    if (old != mr.debugVirtualSurfaces)
                        window->map->purgeViewCache();
                }

                // coarseness disks
                mr.debugCoarsenessDisks = nk_check_label(&ctx, "Coarseness disks", mr.debugCoarsenessDisks);

                // depth feedback
                r.debugDepthFeedback = nk_check_label(&ctx, "Depth feedback", r.debugDepthFeedback);

                // geodata validation
                {
                    bool old = mr.debugValidateGeodataStyles;
                    mr.debugValidateGeodataStyles = nk_check_label(&ctx, "Validate geodata styles", mr.debugValidateGeodataStyles);
                    if (old != mr.debugValidateGeodataStyles)
                        window->map->purgeViewCache();
                }

                // purge disk cache
                if (nk_button_label(&ctx, "Purge disk cache"))
                    window->map->purgeDiskCache();

                // end group
                nk_tree_pop(&ctx);
            }
        }

        // end window
        nk_end(&ctx);
    }

    template<class T>
    void S(const char *name, T value, const char *unit)
    {
        nk_label(&ctx, name, NK_TEXT_LEFT);
        std::ostringstream ss;
        ss << value;
        if (unit && unit[0] != 0)
            ss << unit;
        nk_label(&ctx, ss.str().c_str(), NK_TEXT_RIGHT);
    }

    void prepareStatistics()
    {
        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Statistics", nk_rect(270, 10, 250, 650), flags))
        {
            MapStatistics &ms = window->map->statistics();
            CameraStatistics &cs = window->camera->statistics();
            float width = nk_window_get_content_region_size(&ctx).x - 30;

            char buffer[256];

            // general
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Timing", NK_MAXIMIZED))
            {
                const float ratio[] = { width * 0.5f, width * 0.5f };
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);

                S("Time map avg:", uint32(window->timingMapSmooth.avg() * 1000), " ms");
                S("Time map max:", uint32(window->timingMapSmooth.max() * 1000), " ms");
                S("Time app:", uint32(window->timingAppProcess * 1000), " ms");
                S("Time frame avg:", uint32(window->timingFrameSmooth.avg() * 1000), " ms");
                S("Time frame max:", uint32(window->timingFrameSmooth.max() * 1000), " ms");

                nk_tree_pop(&ctx);
            }

            // resources
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Resources", NK_MAXIMIZED))
            {
                const float ratio[] = { width * 0.5f, width * 0.5f };
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);

                nk_label(&ctx, "Loading:", NK_TEXT_LEFT);
                if (window->map->getMapRenderComplete())
                    nk_label(&ctx, "done", NK_TEXT_RIGHT);
                else
                    nk_prog(&ctx, (int)(1000 * window->map->getMapRenderProgress()), 1000, false);

                S("GPU memory:", ms.currentGpuMemUseKB / 1024, " MB");
                S("RAM memory:", ms.currentRamMemUseKB / 1024, " MB");
                S("Node meta updates:", cs.currentNodeMetaUpdates, "");
                S("Node draw updates:", cs.currentNodeDrawsUpdates, "");
                S("Preparing:", ms.resourcesPreparing, "");
                S("Downloading:", ms.resourcesDownloading, "");
                S("Accessing:", ms.resourcesAccessed, "");

                if (nk_tree_push(&ctx, NK_TREE_TAB, "Queues", NK_MINIMIZED))
                {
                    float ratio2[] = { width * 0.45f, width * 0.45f };
                    nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio2);

                    S("Cache read:", ms.resourcesQueueCacheRead, "");
                    S("Cache write:", ms.resourcesQueueCacheWrite, "");
                    S("Downloads:", ms.resourcesQueueDownload, "");
                    S("Decode:", ms.resourcesQueueDecode, "");
                    S("Gpu:", ms.resourcesQueueUpload, "");

                    nk_tree_pop(&ctx);
                }

                if (nk_tree_push(&ctx, NK_TREE_TAB, "Total", NK_MINIMIZED))
                {
                    float ratio2[] = { width * 0.45f, width * 0.45f };
                    nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio2);

                    S("Exists:", ms.resourcesExists, "");
                    S("Active:", ms.resourcesActive, "");
                    S("Downloaded:", ms.resourcesDownloaded, "");
                    S("Disk loaded:", ms.resourcesDiskLoaded, "");
                    S("Decoded:", ms.resourcesDecoded, "");
                    S("Uploaded:", ms.resourcesUploaded, "");
                    S("Created:", ms.resourcesCreated, "");
                    S("Released:", ms.resourcesReleased, "");
                    S("Failed:", ms.resourcesFailed, "");

                    nk_tree_pop(&ctx);
                }

                nk_tree_pop(&ctx);
            }

            // traversed
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Traversed nodes", NK_MINIMIZED))
            {
                const float ratio[] = { width * 0.5f, width * 0.5f };
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);

                for (unsigned i = 0; i < CameraStatistics::MaxLods; i++)
                {
                    if (cs.metaNodesTraversedPerLod[i] == 0)
                        continue;
                    sprintf(buffer, "[%d]:", i);
                    S(buffer, cs.metaNodesTraversedPerLod[i], "");
                }

                S("Total:", cs.metaNodesTraversedTotal, "");

                nk_tree_pop(&ctx);
            }

            // rendered nodes
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Rendered nodes", NK_MINIMIZED))
            {
                const float ratio[] = { width * 0.5f, width * 0.5f };
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);

                for (unsigned i = 0; i < CameraStatistics::MaxLods; i++)
                {
                    if (cs.nodesRenderedPerLod[i] == 0)
                        continue;
                    sprintf(buffer, "[%d]:", i);
                    S(buffer, cs.nodesRenderedPerLod[i], "");
                }
                S("Total:", cs.nodesRenderedTotal, "");

                nk_tree_pop(&ctx);
            }

            // task counts
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Task counts", NK_MINIMIZED))
            {
                const float ratio[] = { width * 0.5f, width * 0.5f };
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);

                const CameraDraws &d = window->camera->draws();
                S("Opaque: ", d.opaque.size(), "");
                S("Transparent: ", d.transparent.size(), "");
                S("Geodata: ", d.geodata.size(), "");
                S("Infographics: ", d.infographics.size(), "");

                nk_tree_pop(&ctx);
            }
        }

        // end window
        nk_end(&ctx);
    }

    void preparePosition()
    {
        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Position", nk_rect(890, 10, 250, 400), flags))
        {
            const float width = nk_window_get_content_region_size(&ctx).x - 30;

            // loading?
            if (!window->map->getMapconfigAvailable())
            {
                nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);
                nk_label(&ctx, "Loading...", NK_TEXT_LEFT);
                nk_end(&ctx);
                return;
            }

            const float ratio[] = { width * 0.4f, width * 0.6f };
            nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
            char buffer[256];

            // input
            {
                nk_label(&ctx, "Input:", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Use from clipboard"))
                {
                    try
                    {
                        const char *text = getClipboard();
                        window->navigation->options().type = vts::NavigationType::FlyOver;
                        window->navigation->setPosition(vts::Position(text));
                    }
                    catch(...)
                    {
                        // do nothing
                    }
                }
            }

            // subjective position
            {
                int subj = window->navigation->getSubjective();
                const int prev = subj;
                nk_label(&ctx, "Type:", NK_TEXT_LEFT);
                nk_checkbox_label(&ctx, "subjective", &subj);
                if (subj != prev)
                    window->navigation->setSubjective(!!subj, true);
            }

            // srs
            {
                nk_label(&ctx, "Srs:", NK_TEXT_LEFT);
                if (nk_combo_begin_label(&ctx, SrsNames[positionSrs], nk_vec2(nk_widget_width(&ctx), 200)))
                {
                    nk_layout_row_dynamic(&ctx, 16, 1);
                    for (int i = 0; i < 3; i++)
                        if (nk_combo_item_label(&ctx, SrsNames[i], NK_TEXT_LEFT))
                            positionSrs = i;
                    nk_combo_end(&ctx);
                }
            }

            // position
            {
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
                double n[3];
                window->navigation->getPoint(n);
                try
                {
                    window->map->convert(n, n, Srs::Navigation, (Srs)positionSrs);
                }
                catch (const std::exception &)
                {
                    for (int i = 0; i < 3; i++)
                        n[i] = nan1();
                }
                nk_label(&ctx, "X:", NK_TEXT_LEFT);
                sprintf(buffer, "%.8f", n[0]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Y:", NK_TEXT_LEFT);
                sprintf(buffer, "%.8f", n[1]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Z:", NK_TEXT_LEFT);
                sprintf(buffer, "%.8f", n[2]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Reset altitude"))
                {
                    window->navigation->options().type = vts::NavigationType::Quick;
                    window->navigation->resetAltitude();
                }
            }

            // rotation
            {
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
                double n[3];
                window->navigation->getRotation(n);
                // yaw
                nk_label(&ctx, "Yaw:", NK_TEXT_LEFT);
                sprintf(buffer, "%5.1f", n[0]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                // pitch/tilt
                nk_label(&ctx, "Pitch:", NK_TEXT_LEFT);
                sprintf(buffer, "%5.1f", n[1]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                // roll
                nk_label(&ctx, "Roll:", NK_TEXT_LEFT);
                sprintf(buffer, "%5.1f", n[2]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                // reset
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
                nk_label(&ctx, "", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Reset rotation"))
                {
                    window->navigation->setRotation({0,270,0});
                    window->navigation->options().type = vts::NavigationType::Quick;
                    window->navigation->resetNavigationMode();
                }
            }

            // view extent
            {
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
                nk_label(&ctx, "View extent:", NK_TEXT_LEFT);
                sprintf(buffer, "%10.1f", window->navigation->getViewExtent());
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
            }

            // fov
            {
                const float ratio[] = { width * 0.4f, width * 0.45f, width * 0.15f };
                nk_layout_row(&ctx, NK_STATIC, 16, 3, ratio);
                nk_label(&ctx, "Fov:", NK_TEXT_LEFT);
                const float prev = window->navigation->getFov();
                const float fov = nk_slide_float(&ctx, 1, prev, 100, 1);
                if (std::abs(fov - prev) > 1e-7)
                    window->navigation->setFov(fov);
                sprintf(buffer, "%5.1f", fov);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
            }

            // output
            {
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
                nk_label(&ctx, "Output:", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Copy to clipboard"))
                    glfwSetClipboardString(window->window, window->navigation->getPosition().toUrl().c_str());
            }

            // camera
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Camera", NK_MINIMIZED))
            {
                const float ratio[] = { width * 0.5f, width * 0.5f };
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
                const auto &c = window->camera->draws().camera;
                nk_label(&ctx, "Target Distance:", NK_TEXT_LEFT);
                sprintf(buffer, "%.8f", c.targetDistance);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "View Extent:", NK_TEXT_LEFT);
                sprintf(buffer, "%.8f", c.viewExtent);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Altitude Over Ellipsoid:", NK_TEXT_LEFT);
                sprintf(buffer, "%.8f", c.altitudeOverEllipsoid);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Altitude Over Surface:", NK_TEXT_LEFT);
                sprintf(buffer, "%.8f", c.altitudeOverSurface);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Near:", NK_TEXT_LEFT);
                sprintf(buffer, "%.3f", c.proj[14] / (c.proj[10] - 1));
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Far:", NK_TEXT_LEFT);
                sprintf(buffer, "%.3f", c.proj[14] / (c.proj[10] + 1));
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_tree_pop(&ctx);
            }

            // auto movement
            if (nk_tree_push(&ctx, NK_TREE_TAB, "Auto", NK_MINIMIZED))
            {
                const float ratio[] = { width * 0.4f, width * 0.6f };
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);

                for (int i = 0; i < 3; i++)
                {
                    nk_label(&ctx, i == 0 ? "Move:" : "", NK_TEXT_LEFT);
                    posAutoMotion[i] = nk_slide_float(&ctx, -2, posAutoMotion[i], 2, 0.05);
                }
                nk_label(&ctx, "Rotate:", NK_TEXT_LEFT);
                posAutoRotation = nk_slide_float(&ctx, -0.5, posAutoRotation, 0.5, 0.02);
                window->navigation->pan(vec3(300 * posAutoMotion * window->timingTotalFrame).data());
                window->navigation->rotate({ 300 * posAutoRotation * window->timingTotalFrame, 0, 0 });
                window->navigation->options().type = vts::NavigationType::Quick;
                nk_tree_pop(&ctx);
            }
        }
        nk_end(&ctx);
    }

    std::string labelWithCounts(const std::string &label, std::size_t a, std::size_t b)
    {
        std::ostringstream ss;
        if (b == 0)
            ss << label << " (0)";
        else
            ss << label << " (" << a << " / " << b << ")";
        return ss.str();
    }

    bool prepareViewsBoundLayers(std::vector<MapView::BoundLayerInfo> &bl, uint32 &bid)
    {
        const std::vector<std::string> boundLayers = window->map->getResourceBoundLayers();
        if (nk_tree_push_id(&ctx, NK_TREE_NODE, labelWithCounts("Bound Layers", bl.size(), boundLayers.size()).c_str(), NK_MINIMIZED, bid++))
        {
            struct Ender
            {
                nk_context *ctx;
                Ender(nk_context *ctx) : ctx(ctx) {};
                ~Ender() { nk_tree_pop(ctx); }
            } ender(&ctx);

            std::set<std::string> bls(boundLayers.begin(), boundLayers.end());
            float width = nk_window_get_content_region_size(&ctx).x - 70;

            // enabled layers
            bool changed = false;
            if (!bl.empty())
            {
                const float ratio[] = { width * 0.7f, width * 0.3f, 20};
                nk_layout_row(&ctx, NK_STATIC, 16, 3, ratio);
                int idx = 0;
                for (auto &bn : bl)
                {
                    if (!nk_check_label(&ctx, bn.id.c_str(), 1))
                    {
                        bl.erase(bl.begin() + idx);
                        return true;
                    }
                    bls.erase(bn.id);

                    // alpha
                    double a2 = nk_slide_float(&ctx, 0.1, bn.alpha , 1, 0.1);
                    if (bn.alpha != a2)
                    {
                        bn.alpha = a2;
                        changed = true;
                    }

                    // arrows
                    if (idx > 0)
                    {
                        if (nk_button_label(&ctx, "^"))
                        {
                            std::swap(bl[idx - 1], bl[idx]);
                            return true;
                        }
                    }
                    else
                        nk_label(&ctx, "", NK_TEXT_LEFT);

                    idx++;
                }
            }

            // available layers
            if (!bls.empty())
            {
                nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);
                for (auto &bn : bls)
                {
                    if (nk_check_label(&ctx, bn.c_str(), 0))
                    {
                        MapView::BoundLayerInfo bli;
                        bli.id = bn;
                        bl.push_back(bli);
                        return true;
                    }
                }
            }
            return changed;
        }
        return false;
    }

    uint32 currentMapConfig()
    {
        std::string current = window->map->getMapconfigPath();
        uint32 idx = 0;
        for (auto it : window->appOptions.paths)
        {
            if (it.mapConfig == current)
                return idx;
            idx++;
        }
        return 0;
    }

    void selectMapconfig(uint32 index)
    {
        window->marks.clear();
        if (index == (uint32)-1)
            index = window->appOptions.paths.size() - 1;
        else
            index = index % window->appOptions.paths.size();
        window->setMapConfigPath(window->appOptions.paths[index]);
    }

    void prepareViews()
    {
        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Views", nk_rect(530, 10, 350, 600), flags))
        {
            const float width = nk_window_get_content_region_size(&ctx).x - 30;

            // mapconfig selector
            if (window->appOptions.paths.size() > 1)
            {
                // combo selector
                nk_layout_row(&ctx, NK_STATIC, 20, 1, &width);
                if (nk_combo_begin_label(&ctx, window->map->getMapconfigPath().c_str(), nk_vec2(nk_widget_width(&ctx), 200)))
                {
                    nk_layout_row_dynamic(&ctx, 16, 1);
                    for (int i = 0, e = window->appOptions.paths.size(); i < e; i++)
                    {
                        if (nk_combo_item_label(&ctx, window->appOptions.paths[i].mapConfig.c_str(), NK_TEXT_LEFT))
                        {
                            selectMapconfig(i);
                            nk_combo_end(&ctx);
                            nk_end(&ctx);
                            return;
                        }
                    }
                    nk_combo_end(&ctx);
                }

                // buttons
                {
                    const float ratio[] = { width * 0.5f, width * 0.5f };
                    nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
                    if (nk_button_label(&ctx, "< Prev"))
                    {
                        selectMapconfig(currentMapConfig() - 1);
                        nk_end(&ctx);
                        return;
                    }
                    if (nk_button_label(&ctx, "Next >"))
                    {
                        selectMapconfig(currentMapConfig() + 1);
                        nk_end(&ctx);
                        return;
                    }
                }
            }

            // add mapconfig
            {
                nk_layout_row(&ctx, NK_STATIC, 20, 1, &width);
                if (nk_button_label(&ctx, "Use mapconfig from clipboard"))
                {
                    MapPaths p;
                    p.mapConfig = getClipboard();
                    if (!p.mapConfig.empty())
                    {
                        window->appOptions.paths.push_back(p);
                        selectMapconfig(window->appOptions.paths.size() - 1);
                        nk_end(&ctx);
                        return;
                    }
                }
            }

            // loading?
            if (!window->map->getMapconfigAvailable())
            {
                nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);
                nk_label(&ctx, "Loading...", NK_TEXT_LEFT);
                nk_end(&ctx);
                return;
            }

            // named view selector
            const std::vector<std::string> names = window->map->listViews();
            if (names.size() > 1)
            {
                nk_layout_row(&ctx, NK_STATIC, 20, 1, &width);
                if (nk_combo_begin_label(&ctx, window->map->selectedView().c_str(), nk_vec2(nk_widget_width(&ctx), 200)))
                {
                    nk_layout_row_dynamic(&ctx, 16, 1);
                    for (int i = 0, e = names.size(); i < e; i++)
                        if (nk_combo_item_label(&ctx, names[i].c_str(), NK_TEXT_LEFT))
                            window->map->selectView(names[i]);
                    nk_combo_end(&ctx);
                }
            }

            // current view
            bool viewChanged = false;
            MapView view = window->map->getView(window->map->selectedView());

            // input
            {
                nk_layout_row(&ctx, NK_STATIC, 20, 1, &width);
                if (nk_button_label(&ctx, "Use view from clipboard"))
                {
                    try
                    {
                        view = MapView(getClipboard());
                        viewChanged = true;
                    }
                    catch (...)
                    {
                        // do nothing
                    }
                }
            }

            // surfaces
            const std::vector<std::string> surfaces = window->map->getResourceSurfaces();
            if (nk_tree_push(&ctx, NK_TREE_TAB, labelWithCounts("Surfaces", view.surfaces.size(), surfaces.size()).c_str(), NK_MINIMIZED))
            {
                uint32 bid = 0;
                for (const std::string &sn : surfaces)
                {
                    nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);
                    bool v1 = view.surfaces.find(sn) != view.surfaces.end();
                    bool v2 = nk_check_label(&ctx, sn.c_str(), v1);
                    if (v2)
                    {
                        // bound layers
                        MapView::SurfaceInfo &s = view.surfaces[sn];
                        viewChanged = viewChanged || prepareViewsBoundLayers(s.boundLayers, bid);
                    }
                    else
                        view.surfaces.erase(sn);
                    if (v1 != v2)
                        viewChanged = true;
                }
                nk_tree_pop(&ctx);
            }

            // free layers
            const std::vector<std::string> freeLayers = window->map->getResourceFreeLayers();
            if (nk_tree_push(&ctx, NK_TREE_TAB, labelWithCounts("Free Layers", view.freeLayers.size(), freeLayers.size()).c_str(), NK_MINIMIZED))
            {
                uint32 bid = 2000000000;
                for (const std::string &ln : freeLayers)
                {
                    nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);
                    bool v1 = view.freeLayers.find(ln) != view.freeLayers.end();
                    bool v2 = nk_check_label(&ctx, ln.c_str(), v1);
                    if (v2)
                    {
                        MapView::FreeLayerInfo &s = view.freeLayers[ln];
                        bool editableStyle = false;
                        bool editableGeodata = false;
                        switch (window->map->getResourceFreeLayerType(ln))
                        {
                        case FreeLayerType::TiledMeshes:
                        {
                            // bound layers
                            viewChanged = viewChanged || prepareViewsBoundLayers(s.boundLayers, bid);
                        } break;
                        case FreeLayerType::MonolithicGeodata:
                            editableGeodata = true;
                            editableStyle = true;
                            break;
                        case FreeLayerType::TiledGeodata:
                            editableStyle = true;
                            break;
                        default:
                            break;
                        }
                        if (editableGeodata || editableStyle)
                        {
                            const float ratio[] = { 15, (width - 15) * 0.5f, (width - 15) * 0.5f };
                            nk_layout_row(&ctx, NK_STATIC, 16, 3, ratio);
                            nk_label(&ctx, "", NK_TEXT_LEFT);
                            if (editableStyle)
                            {
                                if (nk_button_label(&ctx, "Style"))
                                {
                                    std::string v = window->map->getResourceFreeLayerStyle(ln);
                                    v = editor(ln + ".style.json", v);
                                    window->map->setResourceFreeLayerStyle(ln, v);
                                }
                            }
                            else
                                nk_label(&ctx, "", NK_TEXT_LEFT);
                            if (editableGeodata)
                            {
                                if (nk_button_label(&ctx, "Geodata"))
                                {
                                    std::string v = window->map->getResourceFreeLayerGeodata(ln);
                                    v = editor(ln + ".geo.json", v);
                                    window->map->setResourceFreeLayerGeodata(ln, v);
                                }
                            }
                            else
                                nk_label(&ctx, "", NK_TEXT_LEFT);
                        }
                    }
                    else
                        view.freeLayers.erase(ln);
                    if (v1 != v2)
                        viewChanged = true;
                }

                // fabricate geodata layer
                nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);
                if (nk_button_label(&ctx, "Add Geodata Layer"))
                {
                    const std::set<std::string> fls(freeLayers.begin(), freeLayers.end());
                    for (uint32 i = 1; true; i++)
                    {
                        std::ostringstream ss;
                        ss << "Geodata " << i;
                        std::string n = ss.str();
                        if (fls.count(n) == 0)
                        {
                            window->map->fabricateResourceFreeLayerGeodata(n);
                            break;
                        }
                    }
                }

                nk_tree_pop(&ctx);
            }

            if (viewChanged)
            {
                window->map->setView("", view);
                window->map->selectView("");
            }

            // output
            {
                nk_layout_row(&ctx, NK_STATIC, 16, 1, &width);
                if (nk_button_label(&ctx, "Copy view to clipboard"))
                    glfwSetClipboardString(window->window, view.toUrl().c_str());
            }
        }

        // end window
        nk_end(&ctx);
    }

    void prepareMarks()
    {
        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Marks", nk_rect(1150, 10, 250, 400), flags))
        {
            std::vector<Mark> &marks = window->marks;
            const float width = nk_window_get_content_region_size(&ctx).x - 15;
            const float ratio[] = { width * 0.6f, width * 0.4f };
            nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
            char buffer[256];
            Mark *prev = nullptr;
            int i = 0;
            double length = 0;
            for (Mark &m : marks)
            {
                sprintf(buffer, "%d", (i + 1));
                nk_checkbox_label(&ctx, buffer, &m.open);
                double l = prev ? vts::length(vec3(prev->coord - m.coord)) : 0;
                length += l;
                sprintf(buffer, "%.3f", l);
                nk_color c;
                c.r = 255 * m.color(0);
                c.g = 255 * m.color(1);
                c.b = 255 * m.color(2);
                c.a = 255;
                nk_label_colored(&ctx, buffer, NK_TEXT_RIGHT, c);
                if (m.open)
                {
                    double n[3] = { m.coord(0), m.coord(1), m.coord(2) };
                    try
                    {
                        window->map->convert(n, n, Srs::Physical, (Srs)positionSrs);
                    }
                    catch(...)
                    {
                        for (int i = 0; i < 3; i++)
                            n[i] = std::numeric_limits<double>::quiet_NaN();
                    }
                    sprintf(buffer, "%.8f", n[0]);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                    if (nk_button_label(&ctx, "Go"))
                    {
                        double n[3] = { m.coord(0), m.coord(1), m.coord(2) };
                        window->map->convert(n, n, Srs::Physical, Srs::Navigation);
                        window->navigation->setPoint(n);
                        window->navigation->options().type = vts::NavigationType::FlyOver;
                    }
                    sprintf(buffer, "%.8f", n[1]);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                    nk_label(&ctx, "", NK_TEXT_RIGHT);
                    sprintf(buffer, "%.8f", n[2]);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                    if (nk_button_label(&ctx, "Remove"))
                    {
                        marks.erase(marks.begin() + i);
                        break;
                    }
                }
                prev = &m;
                i++;
            }
            nk_label(&ctx, "Total:", NK_TEXT_LEFT);
            sprintf(buffer, "%.3f", length);
            nk_label(&ctx, buffer, NK_TEXT_RIGHT);
            nk_label(&ctx, "", NK_TEXT_LEFT);
            if (nk_button_label(&ctx, "Clear all"))
                marks.clear();
        }
        nk_end(&ctx);
    }

    void prepareSearch()
    {
        if (search && window->map->statistics().renderTicks % 120 == 60)
        {
            try
            {
                double point[3];
                window->navigation->getPoint(point);
                search->updateDistances(point);
            }
            catch (...)
            {
                search.reset();
            }
        }

        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Search", nk_rect(1410, 10, 350, 500), flags))
        {
            const float width = nk_window_get_content_region_size(&ctx).x - 30;

            if (!window->map->searchable())
            {
                nk_layout_row(&ctx, NK_STATIC, 20, 1, &width);
                nk_label(&ctx, "Search not available.", NK_TEXT_LEFT);
                nk_end(&ctx);
                return;
            }

            // search query
            {
                const float ratio[] = { width * 0.15f, width * 0.85f };
                nk_layout_row(&ctx, NK_STATIC, 22, 2, ratio);
                nk_label(&ctx, "Query:", NK_TEXT_LEFT);
                int len = strlen(searchText);
                nk_edit_string(&ctx, NK_EDIT_FIELD | NK_EDIT_AUTO_SELECT, searchText, &len, MaxSearchTextLength - 1, nullptr);
                searchText[len] = 0;
                if (strcmp(searchText, searchTextPrev) != 0)
                {
                    if (nk_utf_len(searchText, len) >= 3)
                        search = window->map->search(searchText);
                    else
                        search.reset();
                    strcpy(searchTextPrev, searchText);
                }
            }

            // search results
            if (!search)
            {
                nk_end(&ctx);
                return;
            }

            if (!search->done)
            {
                nk_layout_row(&ctx, NK_STATIC, 20, 1, &width);
                nk_label(&ctx, "Searching...", NK_TEXT_LEFT);
                nk_end(&ctx);
                return;
            }

            if (search->results.empty())
            {
                nk_layout_row(&ctx, NK_STATIC, 20, 1, &width);
                nk_label(&ctx, "No results.", NK_TEXT_LEFT);
                nk_end(&ctx);
                return;
            }

            char buffer[200];
            std::vector<SearchItem> &res = search->results;
            int index = 0;
            for (auto &r : res)
            {
                const float ratio[] = { width * 0.7f, width * 0.18f, width * 0.12f };
                nk_layout_row(&ctx, NK_STATIC, 18, 3, ratio);

                // title
                nk_label(&ctx, r.title.c_str(), NK_TEXT_LEFT);

                // distance
                if (r.distance >= 1e3)
                    sprintf(buffer, "%.1lf km", r.distance * 1e-3);
                else
                    sprintf(buffer, "%.1lf m", r.distance);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                // go button
                if (!std::isnan(r.position[0]))
                {
                    if (nk_button_label(&ctx, "Go"))
                    {
                        window->navigation->setSubjective(false, false);
                        window->navigation->setViewExtent(std::max(6667.0, std::isnan(r.radius) ? 0 : r.radius * 2));
                        window->navigation->setRotation({0,270,0});
                        window->navigation->resetAltitude();
                        window->navigation->resetNavigationMode();
                        window->navigation->setPoint(r.position);
                        window->navigation->options().type = vts::NavigationType::FlyOver;
                    }
                }
                else
                    nk_label(&ctx, "", NK_TEXT_LEFT);

                // region
                if (nk_tree_push_id(&ctx, NK_TREE_NODE, r.region.c_str(), NK_MINIMIZED, index))
                {
                    const float ratio[] = { width };
                    int len = r.json.length();
                    nk_layout_row(&ctx, NK_STATIC, 300, 1, ratio);
                    nk_edit_string(&ctx, NK_EDIT_DEFAULT //NK_EDIT_READ_ONLY
                        | NK_EDIT_MULTILINE | NK_EDIT_SELECTABLE
                        | NK_EDIT_CLIPBOARD | NK_EDIT_AUTO_SELECT,
                        (char*)r.json.c_str(), &len, len, 0);
                    nk_tree_pop(&ctx);
                }
                index++;
            }
        }
        nk_end(&ctx);
    }

    void prepare()
    {
        prepareOptions();
        prepareStatistics();
        preparePosition();
        prepareViews();
        prepareMarks();
        prepareSearch();
        prepareFirst = false;
    }

    void render(int width, int height)
    {
        prepare();
        if (!hideTheGui)
            dispatch(width, height);
    }

    static constexpr int MaxSearchTextLength = 200;
    char searchText[MaxSearchTextLength] = {};
    char searchTextPrev[MaxSearchTextLength] = {};
    char positionInputText[MaxSearchTextLength] = {};

    std::shared_ptr<Texture> fontTexture;
    std::shared_ptr<Texture> skinTexture;
    std::shared_ptr<Shader> shader;
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<SearchTask> search;

    GuiSkinMedia skinMedia;
    nk_context ctx;
    nk_font_atlas atlas;
    nk_font *font = nullptr;
    nk_buffer cmds;
    nk_convert_config config;
    nk_draw_null_texture null;

    vec3 posAutoMotion = { 0, 0, 0 };
    double posAutoRotation = 0;
    double viewExtentLimitScaleMin = 0;
    double viewExtentLimitScaleMax = std::numeric_limits<double>::infinity();
    int positionSrs = 2;

    MainWindow *window = nullptr;
    bool prepareFirst = true;
    bool hideTheGui = false;
    double scale = 1;

    static constexpr int MaxVertexMemory = 4 * 1024 * 1024;
    static constexpr int MaxElementMemory = 4 * 1024 * 1024;
};

void MainWindow::Gui::initialize(MainWindow *window)
{
    impl = std::make_shared<GuiImpl>(window);
}

void MainWindow::Gui::render(int width, int height)
{
    impl->render(width, height);
}

void MainWindow::Gui::inputBegin()
{
    nk_input_begin(&impl->ctx);
}

void MainWindow::Gui::inputEnd()
{
    auto ctx = &impl->ctx;
    auto win = impl->window->window;

    nk_input_key(ctx, NK_KEY_DEL, glfwGetKey(win, GLFW_KEY_DELETE) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_ENTER, glfwGetKey(win, GLFW_KEY_ENTER) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_TAB, glfwGetKey(win, GLFW_KEY_TAB) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_BACKSPACE, glfwGetKey(win, GLFW_KEY_BACKSPACE) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_UP, glfwGetKey(win, GLFW_KEY_UP) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_DOWN, glfwGetKey(win, GLFW_KEY_DOWN) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_TEXT_START, glfwGetKey(win, GLFW_KEY_HOME) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_TEXT_END, glfwGetKey(win, GLFW_KEY_END) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_SCROLL_START, glfwGetKey(win, GLFW_KEY_HOME) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_SCROLL_END, glfwGetKey(win, GLFW_KEY_END) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_SCROLL_DOWN, glfwGetKey(win, GLFW_KEY_PAGE_DOWN) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_SCROLL_UP, glfwGetKey(win, GLFW_KEY_PAGE_UP) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_SHIFT, glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);

    if (glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS)
    {
        nk_input_key(ctx, NK_KEY_COPY, glfwGetKey(win, GLFW_KEY_C) == GLFW_PRESS);
        nk_input_key(ctx, NK_KEY_PASTE, glfwGetKey(win, GLFW_KEY_V) == GLFW_PRESS);
        nk_input_key(ctx, NK_KEY_CUT, glfwGetKey(win, GLFW_KEY_X) == GLFW_PRESS);
        nk_input_key(ctx, NK_KEY_TEXT_UNDO, glfwGetKey(win, GLFW_KEY_Z) == GLFW_PRESS);
        nk_input_key(ctx, NK_KEY_TEXT_REDO, glfwGetKey(win, GLFW_KEY_R) == GLFW_PRESS);
        nk_input_key(ctx, NK_KEY_TEXT_WORD_LEFT, glfwGetKey(win, GLFW_KEY_LEFT) == GLFW_PRESS);
        nk_input_key(ctx, NK_KEY_TEXT_WORD_RIGHT, glfwGetKey(win, GLFW_KEY_RIGHT) == GLFW_PRESS);
        nk_input_key(ctx, NK_KEY_TEXT_LINE_START, glfwGetKey(win, GLFW_KEY_B) == GLFW_PRESS);
        nk_input_key(ctx, NK_KEY_TEXT_LINE_END, glfwGetKey(win, GLFW_KEY_E) == GLFW_PRESS);
    }
    else
    {
        nk_input_key(ctx, NK_KEY_LEFT, glfwGetKey(win, GLFW_KEY_LEFT) == GLFW_PRESS);
        nk_input_key(ctx, NK_KEY_RIGHT, glfwGetKey(win, GLFW_KEY_RIGHT) == GLFW_PRESS);
        nk_input_key(ctx, NK_KEY_COPY, 0);
        nk_input_key(ctx, NK_KEY_PASTE, 0);
        nk_input_key(ctx, NK_KEY_CUT, 0);
        nk_input_key(ctx, NK_KEY_SHIFT, 0);
    }

    double x, y;
    glfwGetCursorPos(win, &x, &y);
    x /= impl->scale;
    y /= impl->scale;
    nk_input_motion(ctx, (int)x, (int)y);
    nk_input_button(ctx, NK_BUTTON_LEFT, (int)x, (int)y, glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    nk_input_button(ctx, NK_BUTTON_MIDDLE, (int)x, (int)y, glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS);
    nk_input_button(ctx, NK_BUTTON_RIGHT, (int)x, (int)y, glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
    //nk_input_button(ctx, NK_BUTTON_DOUBLE, (int)glfw.double_click_pos.x, (int)glfw.double_click_pos.y, glfw.is_double_click_down);

    nk_input_end(&impl->ctx);
}

void MainWindow::Gui::finalize()
{
    impl.reset();
}

void MainWindow::Gui::visible(bool visible)
{
    impl->hideTheGui = !visible;
}

void MainWindow::Gui::scale(double scaling)
{
    impl->scale = scaling;
}

bool MainWindow::Gui::key_callback(int key, int scancode, int action, int mods)
{
    return nk_item_is_any_active(&impl->ctx);
}

bool MainWindow::Gui::character_callback(unsigned int codepoint)
{
    nk_input_unicode(&impl->ctx, codepoint);
    return nk_item_is_any_active(&impl->ctx);
}

bool MainWindow::Gui::cursor_position_callback(double xpos, double ypos)
{
    return nk_item_is_any_active(&impl->ctx);
}

bool MainWindow::Gui::mouse_button_callback(int button, int action, int mods)
{
    return nk_item_is_any_active(&impl->ctx);
}

bool MainWindow::Gui::scroll_callback(double xoffset, double yoffset)
{
    struct nk_vec2 sc = { (float)xoffset, (float)yoffset };
    nk_input_scroll(&impl->ctx, sc);
    return nk_item_is_any_active(&impl->ctx);
}


