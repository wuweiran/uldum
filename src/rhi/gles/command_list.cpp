// OpenGL ES 3.1 implementation of rhi::CommandList.
//
// GL has no separate command-buffer object; every method here records
// straight into the current GL context. The `m_cmd` void* on a GLES
// CommandList holds the `Rhi*` (set in gles_rhi.cpp's begin_frame /
// begin_oneshot). All state — bound pipeline, vertex/index buffers,
// descriptor sets — lives in `Rhi::CmdState` and is flushed inside
// draw() / draw_indexed() because GL needs vertex-buffer bindings
// active before glVertexAttribPointer takes effect.

#include "rhi/command_list.h"
#include "rhi/gles/gles_rhi.h"
#include "rhi/gles/gles_records.h"
#include "core/log.h"

#include <GLES3/gl32.h>

#include <algorithm>

namespace uldum::rhi {

static constexpr const char* TAG = "RHI.GLES.Cmd";

// ── Translation helpers ─────────────────────────────────────────────

static inline Rhi& rhi_of(void* p) { return *static_cast<Rhi*>(p); }

static GLenum to_gl_topology(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::TriangleList:  return GL_TRIANGLES;
        case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
        case PrimitiveTopology::LineList:      return GL_LINES;
    }
    return GL_TRIANGLES;
}

static GLenum to_gl_blend_factor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:             return GL_ZERO;
        case BlendFactor::One:              return GL_ONE;
        case BlendFactor::SrcColor:         return GL_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return GL_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColor:         return GL_DST_COLOR;
        case BlendFactor::OneMinusDstColor: return GL_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlpha:         return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:         return GL_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
    }
    return GL_ZERO;
}

static GLenum to_gl_blend_op(BlendOp o) {
    switch (o) {
        case BlendOp::Add:             return GL_FUNC_ADD;
        case BlendOp::Subtract:        return GL_FUNC_SUBTRACT;
        case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case BlendOp::Min:             return GL_MIN;
        case BlendOp::Max:             return GL_MAX;
    }
    return GL_FUNC_ADD;
}

static GLenum to_gl_compare(CompareOp o) {
    switch (o) {
        case CompareOp::Never:        return GL_NEVER;
        case CompareOp::Less:         return GL_LESS;
        case CompareOp::Equal:        return GL_EQUAL;
        case CompareOp::LessEqual:    return GL_LEQUAL;
        case CompareOp::Greater:      return GL_GREATER;
        case CompareOp::NotEqual:     return GL_NOTEQUAL;
        case CompareOp::GreaterEqual: return GL_GEQUAL;
        case CompareOp::Always:       return GL_ALWAYS;
    }
    return GL_LESS;
}

// Vertex-attribute format → (component count, GL type, normalized, integer)
struct AttrFormat { GLint count; GLenum type; GLboolean normalized; bool is_integer; };
static AttrFormat to_gl_attr(TextureFormat f) {
    using TF = TextureFormat;
    switch (f) {
        case TF::R32_SFLOAT:          return { 1, GL_FLOAT,         GL_FALSE, false };
        case TF::R32G32_SFLOAT:       return { 2, GL_FLOAT,         GL_FALSE, false };
        case TF::R32G32B32_SFLOAT:    return { 3, GL_FLOAT,         GL_FALSE, false };
        case TF::R32G32B32A32_SFLOAT: return { 4, GL_FLOAT,         GL_FALSE, false };
        case TF::R8G8B8A8_UNORM:      return { 4, GL_UNSIGNED_BYTE, GL_TRUE,  false };
        case TF::R32_UINT:            return { 1, GL_UNSIGNED_INT,  GL_FALSE, true };
        case TF::R32G32B32A32_UINT:   return { 4, GL_UNSIGNED_INT,  GL_FALSE, true };
        default: break;
    }
    return { 4, GL_FLOAT, GL_FALSE, false };
}

// Apply fixed-function state from the bound pipeline. Called inside
// flush_for_draw so toggling pipelines mid-frame works.
static void apply_pipeline_state(const Rhi::PipelineRecord& p) {
    glUseProgram(p.program);

    // Cull
    if (p.rasterizer.cull_mode == CullMode::None) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(p.rasterizer.cull_mode == CullMode::Front ? GL_FRONT : GL_BACK);
    }
    glFrontFace(p.rasterizer.front_face == FrontFace::CounterClockwise ? GL_CCW : GL_CW);
    if (p.rasterizer.depth_bias_enable) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(p.rasterizer.depth_bias_slope_factor,
                        p.rasterizer.depth_bias_constant_factor);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // Depth
    if (p.depth_stencil.depth_test_enable) {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(p.depth_stencil.depth_write_enable ? GL_TRUE : GL_FALSE);
        glDepthFunc(to_gl_compare(p.depth_stencil.depth_compare));
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }

    // Blend (single attachment)
    if (p.blend.blend_enable) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(to_gl_blend_factor(p.blend.src_color_factor),
                            to_gl_blend_factor(p.blend.dst_color_factor),
                            to_gl_blend_factor(p.blend.src_alpha_factor),
                            to_gl_blend_factor(p.blend.dst_alpha_factor));
        glBlendEquationSeparate(to_gl_blend_op(p.blend.color_op),
                                to_gl_blend_op(p.blend.alpha_op));
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask((p.blend.write_mask & 0x1) ? GL_TRUE : GL_FALSE,
                (p.blend.write_mask & 0x2) ? GL_TRUE : GL_FALSE,
                (p.blend.write_mask & 0x4) ? GL_TRUE : GL_FALSE,
                (p.blend.write_mask & 0x8) ? GL_TRUE : GL_FALSE);
}

// Bind all currently active descriptor sets to GL slots. UBO + sampler
// bindings use the flat `(set * kBindingsPerSet + binding)` formula —
// the hand-written GLES shaders declare `layout(binding=N)` matching
// those flat numbers. SSBO bindings use the descriptor's `binding`
// VALUE DIRECTLY, ignoring the set index: ES 3.1 spec only guarantees
// 4 SSBO bindings total, so the flat formula (which produces slot 32
// for set 2 binding 0) violates spec on real Mali/Adreno hardware and
// silently fails to bind. Our engine puts at most one SSBO per
// pipeline at binding 0, so packing them to slot 0 is collision-free.
static void apply_descriptor_bindings(Rhi& rhi, const Rhi::PipelineLayoutRecord& pl,
                                      std::span<const DescriptorSetHandle> sets) {
    for (u32 s = 0; s < sets.size(); ++s) {
        if (!sets[s].is_valid()) continue;
        const auto* set = rhi.descriptor_set_record(sets[s]);
        if (!set) continue;
        const u32 base = s * Rhi::PipelineLayoutRecord::kBindingsPerSet;
        for (const auto& b : set->bindings) {
            switch (b.type) {
                case DescriptorType::UniformBuffer: {
                    rhi.sync_buffer_to_gpu(b.buffer);
                    const auto* buf = rhi.buffer_record(b.buffer);
                    if (!buf) break;
                    const GLuint slot = base + b.binding;
                    const GLintptr  off = static_cast<GLintptr>(b.buffer_offset);
                    const GLsizeiptr range = (b.buffer_range == ~0ull)
                        ? static_cast<GLsizeiptr>(buf->size)
                        : static_cast<GLsizeiptr>(b.buffer_range);
                    glBindBufferRange(GL_UNIFORM_BUFFER, slot, buf->name, off, range);
                    break;
                }
                case DescriptorType::StorageBuffer: {
                    rhi.sync_buffer_to_gpu(b.buffer);
                    const auto* buf = rhi.buffer_record(b.buffer);
                    if (!buf) break;
                    // SSBO uses b.binding directly — see header comment.
                    const GLuint slot = b.binding;
                    const GLintptr  off = static_cast<GLintptr>(b.buffer_offset);
                    const GLsizeiptr range = (b.buffer_range == ~0ull)
                        ? static_cast<GLsizeiptr>(buf->size)
                        : static_cast<GLsizeiptr>(b.buffer_range);
                    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, slot, buf->name, off, range);
                    break;
                }
                case DescriptorType::SampledImage:
                case DescriptorType::CombinedImageSampler: {
                    const auto* tex = rhi.texture_record(b.texture);
                    if (!tex) break;
                    const GLuint slot = base + b.binding;
                    glActiveTexture(GL_TEXTURE0 + slot);
                    glBindTexture(tex->target, tex->name);
                    if (b.type == DescriptorType::CombinedImageSampler) {
                        const auto* smp = rhi.sampler_record(b.sampler);
                        glBindSampler(slot, smp ? smp->name : 0);
                    }
                    break;
                }
            }
        }
    }
    // Note: push-constant UBO is bound from the bound pipeline's layout
    // in flush_for_draw, not here. `cs.sets_layout` may be stale (left
    // over from a previous pipeline that did take descriptor sets), so
    // binding the push UBO from it could overwrite the current
    // pipeline's data with someone else's.
}

// Bind the vertex format described by the pipeline + the buffers from the
// command state. Has to happen at draw time because glVertexAttribPointer
// snapshots whichever VBO is active at call time.
static void apply_vertex_input(Rhi& rhi, const Rhi::PipelineRecord& p) {
    auto& cs = rhi.cmd_state();
    // Disable all attributes first; we'll re-enable the ones the pipeline
    // declares. GL caps the number at GL_MAX_VERTEX_ATTRIBS (typically 16).
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);

    for (const auto& attr : p.vertex_attrs) {
        if (attr.binding >= cs.vertex_buffers.size()) continue;
        const auto& vb = cs.vertex_buffers[attr.binding];
        rhi.sync_buffer_to_gpu(vb.buffer);
        const auto* buf = rhi.buffer_record(vb.buffer);
        if (!buf) continue;
        // Look up stride from the pipeline's binding entry.
        u32 stride = 0;
        for (const auto& vb_desc : p.vertex_bindings) {
            if (vb_desc.binding == attr.binding) { stride = vb_desc.stride; break; }
        }
        glBindBuffer(GL_ARRAY_BUFFER, buf->name);
        const auto fmt = to_gl_attr(attr.format);
        const void* ptr = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(vb.offset + attr.offset));
        glEnableVertexAttribArray(attr.location);
        if (fmt.is_integer) {
            glVertexAttribIPointer(attr.location, fmt.count, fmt.type,
                                   static_cast<GLsizei>(stride), ptr);
        } else {
            glVertexAttribPointer(attr.location, fmt.count, fmt.type, fmt.normalized,
                                  static_cast<GLsizei>(stride), ptr);
        }
    }
}

// Flush all the deferred state we recorded since the last draw.
static const Rhi::PipelineRecord* flush_for_draw(Rhi& rhi) {
    auto& cs = rhi.cmd_state();
    const auto* p = rhi.pipeline_record(cs.pipeline);
    if (!p) {
        log::warn(TAG, "draw with no valid pipeline bound");
        return nullptr;
    }
    apply_pipeline_state(*p);
    // Push-constant UBO is keyed by the bound pipeline's layout, not by
    // the last bind_descriptor_sets call. Pipelines that don't take
    // any descriptor sets (e.g. particles, push-constant-only pipelines)
    // never call bind_descriptor_sets — without this, their push UBO
    // would never get bound at slot 30 and shaders would read stale
    // data from a previous pipeline's layout.
    if (const auto* ppl = rhi.pipeline_layout_record(p->layout);
        ppl && ppl->push_constant_ubo != 0) {
        glBindBufferBase(GL_UNIFORM_BUFFER,
                         Rhi::PipelineLayoutRecord::kPushConstantSlot,
                         ppl->push_constant_ubo);
    }
    if (const auto* pl = rhi.pipeline_layout_record(cs.sets_layout)) {
        // Replay only the sets THIS layout declares — not the whole cs.sets
        // array. bind_descriptor_sets never clears slots, so cs.sets holds
        // stale handles from earlier passes. Most binding types separate by
        // the (set*kBindingsPerSet+binding) slot formula, but SSBOs collapse
        // to their binding VALUE alone (see apply_descriptor_bindings), so a
        // stale set's SSBO at binding 0 would overwrite the current set's at
        // the same GL slot. Clamping to the layout's set count mirrors
        // Vulkan (which scopes sets to the bound layout) and stops e.g. the
        // skinned-mesh bone SSBO (set 2) from clobbering the skinned-shadow
        // bone SSBO (set 0) in the next pass.
        const usize n = std::min(cs.sets.size(), pl->set_layouts.size());
        apply_descriptor_bindings(rhi, *pl, std::span{cs.sets.data(), n});
    }
    apply_vertex_input(rhi, *p);
    if (cs.index_buffer.is_valid()) {
        rhi.sync_buffer_to_gpu(cs.index_buffer);
        const auto* ib = rhi.buffer_record(cs.index_buffer);
        if (ib) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->name);
    }
    cs.dirty = false;
    return p;
}

// ── CommandList API ─────────────────────────────────────────────────

void CommandList::bind_pipeline(PipelineHandle pipeline) {
    auto& cs = rhi_of(m_cmd).cmd_state();
    cs.pipeline = pipeline;
    cs.dirty = true;
}

void CommandList::bind_descriptor_sets(PipelineLayoutHandle layout, u32 first_set,
                                       std::span<const DescriptorSetHandle> sets) {
    auto& cs = rhi_of(m_cmd).cmd_state();
    cs.sets_layout = layout;
    for (u32 i = 0; i < sets.size() && (first_set + i) < cs.sets.size(); ++i) {
        cs.sets[first_set + i] = sets[i];
    }
    cs.dirty = true;
}

void CommandList::bind_vertex_buffer(u32 binding, BufferHandle buf, u64 offset) {
    auto& cs = rhi_of(m_cmd).cmd_state();
    if (binding < cs.vertex_buffers.size()) {
        cs.vertex_buffers[binding] = { buf, offset };
        cs.dirty = true;
    }
}

void CommandList::bind_index_buffer(BufferHandle buf, u64 offset, IndexType type) {
    auto& cs = rhi_of(m_cmd).cmd_state();
    cs.index_buffer = buf;
    cs.index_offset = offset;
    cs.index_type   = (type == IndexType::U16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    cs.dirty = true;
}

void CommandList::set_viewport(f32 x, f32 y, f32 width, f32 height,
                               f32 min_depth, f32 max_depth) {
    glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
               static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDepthRangef(min_depth, max_depth);
}

void CommandList::set_scissor(i32 x, i32 y, u32 width, u32 height) {
    glScissor(x, y, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glEnable(GL_SCISSOR_TEST);
}

void CommandList::set_cull_mode(CullMode mode) {
    // GL cull state is global (apply_pipeline_state writes it at pipeline
    // bind time); flip it here to override the pipeline's static value
    // until the next bind_pipeline restores the pipeline's default.
    if (mode == CullMode::None) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(mode == CullMode::Front ? GL_FRONT : GL_BACK);
    }
}

void CommandList::push_constants(PipelineLayoutHandle layout, ShaderStage /*stages*/,
                                 u32 offset, u32 size, const void* data) {
    auto& rhi = rhi_of(m_cmd);
    const auto* pl = rhi.pipeline_layout_record(layout);
    if (!pl || pl->push_constant_ubo == 0) return;
    glBindBuffer(GL_UNIFORM_BUFFER, pl->push_constant_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, offset, size, data);
}

void CommandList::draw(u32 vertex_count, u32 instance_count,
                       u32 first_vertex, u32 first_instance) {
    auto& rhi = rhi_of(m_cmd);
    const auto* p = flush_for_draw(rhi);
    if (!p) return;
    // GLES doesn't honor first_instance natively (no gl_BaseInstance);
    // feed it via the shared draw-info UBO so shaders that do
    // `instances[gl_InstanceID + u_base_instance]` read the right slice.
    rhi.set_base_instance(first_instance);
    const GLenum mode = to_gl_topology(p->topology);
    if (instance_count == 1) {
        glDrawArrays(mode, static_cast<GLint>(first_vertex), static_cast<GLsizei>(vertex_count));
    } else {
        glDrawArraysInstanced(mode, static_cast<GLint>(first_vertex),
                              static_cast<GLsizei>(vertex_count),
                              static_cast<GLsizei>(instance_count));
    }
}

void CommandList::draw_indexed(u32 index_count, u32 instance_count,
                               u32 first_index, i32 vertex_offset, u32 first_instance) {
    auto& rhi = rhi_of(m_cmd);
    const auto* p = flush_for_draw(rhi);
    if (!p) return;
    rhi.set_base_instance(first_instance);  // see draw() above
    const auto& cs = rhi.cmd_state();
    const GLenum mode = to_gl_topology(p->topology);
    const GLsizei type_size = (cs.index_type == GL_UNSIGNED_SHORT) ? 2 : 4;
    const void* indices = reinterpret_cast<const void*>(
        static_cast<uintptr_t>(cs.index_offset + first_index * type_size));
    if (instance_count == 1 && vertex_offset == 0) {
        glDrawElements(mode, static_cast<GLsizei>(index_count), cs.index_type, indices);
    } else if (vertex_offset == 0) {
        glDrawElementsInstanced(mode, static_cast<GLsizei>(index_count), cs.index_type,
                                indices, static_cast<GLsizei>(instance_count));
    } else {
        glDrawElementsInstancedBaseVertex(mode, static_cast<GLsizei>(index_count), cs.index_type,
                                          indices, static_cast<GLsizei>(instance_count),
                                          vertex_offset);
    }
}

void CommandList::draw_indexed_indirect(BufferHandle buf, u64 offset,
                                        u32 draw_count, u32 stride) {
    auto& rhi = rhi_of(m_cmd);
    const auto* p = flush_for_draw(rhi);
    if (!p) return;
    rhi.sync_buffer_to_gpu(buf);
    const auto* ib = rhi.buffer_record(buf);
    if (!ib) return;
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ib->name);
    const auto& cs = rhi.cmd_state();
    const GLenum mode = to_gl_topology(p->topology);

    // Read the CPU-side shadow of the indirect buffer so we can pull
    // each command's `firstInstance` field. GLES 3.1 doesn't honor it
    // at the shader level (gl_InstanceID resets to 0 per draw), so we
    // feed it via the draw-info UBO at slot 31 and the vertex shader
    // adds it to gl_InstanceID. Mandatory: the renderer's indirect
    // buffer is host-visible (mapped + memcpy'd), so the shadow is the
    // authoritative copy.
    const u8* shadow = ib->shadow.empty() ? nullptr : ib->shadow.data();
    for (u32 i = 0; i < draw_count; ++i) {
        const u64 cmd_offset = offset + static_cast<u64>(i) * stride;
        if (shadow && cmd_offset + sizeof(u32) * 5 <= ib->shadow.size()) {
            // VkDrawIndexedIndirectCommand layout: indexCount,
            // instanceCount, firstIndex, vertexOffset, firstInstance.
            const u32* cmd = reinterpret_cast<const u32*>(shadow + cmd_offset);
            rhi.set_base_instance(cmd[4]);
        }
        const void* off = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(cmd_offset));
        glDrawElementsIndirect(mode, cs.index_type, off);
    }
    // Reset for safety so the next non-indirect draw starts at 0.
    rhi.set_base_instance(0);
}

void CommandList::image_barriers(std::span<const ImageBarrier> /*barriers*/) {
    // GL drivers track read/write hazards automatically. Shader-image-store
    // / SSBO write hazards would need glMemoryBarrier(GL_*_BARRIER_BIT)
    // calls — not yet exercised by the engine.
}

void CommandList::copy_buffer_to_image(BufferHandle src, TextureHandle dst,
                                       std::span<const BufferImageCopy> regions,
                                       ImageLayout /*dst_layout*/) {
    auto& rhi = rhi_of(m_cmd);
    rhi.sync_buffer_to_gpu(src);
    const auto* sbuf = rhi.buffer_record(src);
    const auto* tex  = rhi.texture_record(dst);
    if (!sbuf || !tex) return;

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sbuf->name);
    glBindTexture(tex->target, tex->name);

    for (const auto& r : regions) {
        const void* offset = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(r.buffer_offset));
        // The internal-format tells us the (format, type) pair to use for
        // the upload. For now we infer from common cases.
        GLenum format = GL_RGBA, type = GL_UNSIGNED_BYTE;
        switch (tex->internal_format) {
            case GL_R8:           format = GL_RED;  type = GL_UNSIGNED_BYTE; break;
            case GL_RGBA8:        format = GL_RGBA; type = GL_UNSIGNED_BYTE; break;
            case GL_SRGB8_ALPHA8: format = GL_RGBA; type = GL_UNSIGNED_BYTE; break;
            case GL_RGBA16F:      format = GL_RGBA; type = GL_HALF_FLOAT;    break;
            case GL_R32F:         format = GL_RED;  type = GL_FLOAT;         break;
            default: break;
        }
        if (tex->target == GL_TEXTURE_2D_ARRAY) {
            glTexSubImage3D(tex->target, static_cast<GLint>(r.mip_level),
                            r.image_offset_x, r.image_offset_y,
                            static_cast<GLint>(r.base_array_layer),
                            static_cast<GLsizei>(r.image_extent_w),
                            static_cast<GLsizei>(r.image_extent_h),
                            static_cast<GLsizei>(r.layer_count),
                            format, type, offset);
        } else if (tex->target == GL_TEXTURE_CUBE_MAP) {
            const GLenum face = GL_TEXTURE_CUBE_MAP_POSITIVE_X + r.base_array_layer;
            glTexSubImage2D(face, static_cast<GLint>(r.mip_level),
                            r.image_offset_x, r.image_offset_y,
                            static_cast<GLsizei>(r.image_extent_w),
                            static_cast<GLsizei>(r.image_extent_h),
                            format, type, offset);
        } else {
            glTexSubImage2D(tex->target, static_cast<GLint>(r.mip_level),
                            r.image_offset_x, r.image_offset_y,
                            static_cast<GLsizei>(r.image_extent_w),
                            static_cast<GLsizei>(r.image_extent_h),
                            format, type, offset);
        }
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void CommandList::clear_color_image(TextureHandle image, f32 r, f32 g, f32 b, f32 a,
                                    ImageLayout /*layout*/) {
    auto& rhi = rhi_of(m_cmd);
    const auto* tex = rhi.texture_record(image);
    if (!tex) return;
    // ES 3.2 has glClearTexImage via EXT_clear_texture, but the
    // extension is rare on Android. Use the universally-available
    // FBO + glClear fallback instead: attach the texture as
    // COLOR_ATTACHMENT0 of a scratch FBO, set the clear color, and
    // clear. Restores the previous FBO binding when done.
    //
    // The old implementation was a logged no-op, which left the
    // font atlas reading whatever was previously resident in the
    // allocation — visually most Android drivers zero-init for
    // security so it happened to look fine, but that's not
    // guaranteed and broke on the first driver that didn't.
    const GLfloat color[4] = { r, g, b, a };
    GLint prev_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);

    static GLuint scratch_fbo = 0;
    if (scratch_fbo == 0) glGenFramebuffers(1, &scratch_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scratch_fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (tex->target == GL_TEXTURE_2D) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex->name, 0);
        glClearBufferfv(GL_COLOR, 0, color);
    } else if (tex->target == GL_TEXTURE_2D_ARRAY) {
        // Clear every layer of the array. Atlases are typically a
        // single layer so this loop usually runs once.
        for (u32 layer = 0; layer < tex->layers; ++layer) {
            glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      tex->name, 0, static_cast<GLint>(layer));
            glClearBufferfv(GL_COLOR, 0, color);
        }
    } else if (tex->target == GL_TEXTURE_CUBE_MAP) {
        for (u32 face = 0; face < 6; ++face) {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                                   tex->name, 0);
            glClearBufferfv(GL_COLOR, 0, color);
        }
    } else {
        log::warn(TAG, "clear_color_image: unsupported texture target 0x{:x}", tex->target);
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
}

void CommandList::begin_rendering(const RenderingDesc& desc) {
    // Build (or look up) an FBO for the requested attachments. For now,
    // a single transient FBO per call — caching keyed by attachment
    // handles is a future perf pass.
    static GLuint scratch_fbo = 0;
    if (scratch_fbo == 0) glGenFramebuffers(1, &scratch_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, scratch_fbo);
    // Reset state that gates glClear / glClearBuffer*. A stale scissor
    // or color/depth mask from the previous pass would otherwise leave
    // attachment regions un-cleared.
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);

    auto& rhi = rhi_of(m_cmd);
    GLenum draw_buffers[8]{};
    u32 nc = 0;
    for (const auto& a : desc.color_attachments) {
        const auto* tex = rhi.texture_record(a.image);
        if (!tex) continue;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + nc,
                               tex->target, tex->name, 0);
        draw_buffers[nc] = GL_COLOR_ATTACHMENT0 + nc;
        if (a.load == LoadOp::Clear) {
            glClearBufferfv(GL_COLOR, nc, &a.clear.r);
        }
        ++nc;
    }
    if (nc > 0) glDrawBuffers(static_cast<GLsizei>(nc), draw_buffers);
    else        glDrawBuffers(0, nullptr);

    if (desc.depth) {
        const auto* tex = rhi.texture_record(desc.depth->image);
        if (tex) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                   tex->target, tex->name, 0);
            if (desc.depth->load == LoadOp::Clear) {
                glClearBufferfi(GL_DEPTH_STENCIL, 0, desc.depth->clear.depth,
                                static_cast<GLint>(desc.depth->clear.stencil));
            }
        }
    }
    glViewport(desc.area_x, desc.area_y,
               static_cast<GLsizei>(desc.area_width),
               static_cast<GLsizei>(desc.area_height));
}

void CommandList::end_rendering() {
    // No-op for now. A future pass could glInvalidateFramebuffer to flag
    // transient attachments (saves tile-store bandwidth on mobile).
}

} // namespace uldum::rhi
