#pragma once

// EFB copy diagnostics for the stereo replay (MP_COPY_DIAG=1), and a switch that takes copies
// from the mono EFB only (MP_EYE_COPIES=0), for effects that come out as a flat colour in the
// headset (the charge shot's warp, the pause and map backgrounds, the scan visor's window).
//
// With MP_COPY_DIAG=1 each distinct copy (its route, format, rectangle and target sizes) logs
// once how it was taken, from the mono EFB and from each eye, how the pass it was taken from was
// replayed into the eyes (multiview or per eye, and how many of its draws the eyes skipped), and
// the values of five texels of the mono copy and of each eye's copy, read back from the GPU after
// the frame. Each distinct draw that samples a copy logs once whether it got the eye copies.

#include "types.hpp"

#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <string>
#include <vector>

namespace aurora::gfx::copy_diag {

bool enabled() noexcept;
// MP_EYE_COPIES=0: no copy is taken from the eyes; the eyes sample the mono copies.
bool eye_copies_off() noexcept;
// How many times `key` has been seen, this call included (0 once the table is full).
uint32_t occurrence(const std::string& key) noexcept;
// Whether a copy seen `count` times should be read back now: its 1st, 60th and 600th time.
inline bool readback_due(uint32_t count) noexcept { return count == 1 || count == 60 || count == 600; }
const char* route_name(uint32_t route) noexcept;
// Records copies of five texels (the centre and the four quarter points) of `layer` of `texture`
// into a readback buffer and, once the frame is submitted, logs them after `label`. Logs why
// instead when the format is not one it decodes or the texture can't be copied from.
void readback(const wgpu::CommandEncoder& cmd, std::vector<AfterSubmitCallback>& afterSubmit,
              const wgpu::Texture& texture, wgpu::Extent3D size, uint32_t layer, std::string label) noexcept;

} // namespace aurora::gfx::copy_diag

namespace aurora::gfx::copy_diag {
// Once per run (MP_COPY_DIAG=1): draws an 8x8 test pattern through small copy shaders built like
// tex_copy_conv's (a fullscreen triangle from private arrays, the UV transform from a uniform) and
// variants of them, reads every result back and logs which reproduce the source. It isolates why
// every shader copy (blit and convert) comes out as one colour on the Quest 1 while plain texture
// copies don't: the uniform read (in the vertex or the fragment stage, from a small buffer or deep
// in a large one) or the vertex-indexed private arrays.
void run_shader_probe() noexcept;
// Reads back the copy's UV transform (tex_copy_conv::Uniforms: offset, scale, opaque alpha) from
// `buffer` at `offset` as the copy's pass is encoded, and logs it after `label`.
void readback_uniform(const wgpu::CommandEncoder& cmd, std::vector<AfterSubmitCallback>& afterSubmit,
                      const wgpu::Buffer& buffer, uint64_t offset, uint64_t size, std::string label) noexcept;
} // namespace aurora::gfx::copy_diag
