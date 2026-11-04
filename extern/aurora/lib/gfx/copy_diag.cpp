#include "copy_diag.hpp"

#include "../logging.hpp"
#include "../webgpu/gpu.hpp"

#include <absl/container/flat_hash_map.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace aurora::gfx::copy_diag {
namespace {
constexpr Module Log{"aurora::gfx::copy_diag"};

// Distinct copies and draws a run keeps count of; a run that logs more is not readable anyway.
constexpr size_t kMaxKeys = 512;
// Readbacks a run makes (each is one small buffer).
constexpr uint32_t kMaxReadbacks = 240;
constexpr uint32_t kPoints = 5;
constexpr uint32_t kRowBytes = 256; // bytesPerRow alignment

std::mutex g_mutex;
absl::flat_hash_map<std::string, uint32_t> g_counts;
uint32_t g_readbacks = 0;

bool env_is(const char* name, const char* value) noexcept {
  const char* v = std::getenv(name);
  return v != nullptr && std::strcmp(v, value) == 0;
}

uint32_t texel_bytes(wgpu::TextureFormat format) noexcept {
  switch (format) {
  case wgpu::TextureFormat::R8Unorm:
    return 1;
  case wgpu::TextureFormat::RG8Unorm:
  case wgpu::TextureFormat::R16Float:
    return 2;
  case wgpu::TextureFormat::RGBA8Unorm:
  case wgpu::TextureFormat::RGBA8UnormSrgb:
  case wgpu::TextureFormat::BGRA8Unorm:
  case wgpu::TextureFormat::BGRA8UnormSrgb:
  case wgpu::TextureFormat::RGB10A2Unorm:
  case wgpu::TextureFormat::RG16Float:
  case wgpu::TextureFormat::R32Float:
    return 4;
  case wgpu::TextureFormat::RGBA16Float:
    return 8;
  default:
    return 0;
  }
}

std::string hex(const uint8_t* bytes, uint32_t count) {
  std::string out;
  for (uint32_t i = 0; i < count; ++i) {
    out += fmt::format("{:02x}", bytes[i]);
  }
  return out;
}

} // namespace

bool enabled() noexcept {
  static const bool on = env_is("MP_COPY_DIAG", "1");
  return on;
}

bool eye_copies_off() noexcept {
  static const bool off = env_is("MP_EYE_COPIES", "0");
  return off;
}

uint32_t occurrence(const std::string& key) noexcept {
  std::lock_guard lock{g_mutex};
  auto it = g_counts.find(key);
  if (it == g_counts.end()) {
    if (g_counts.size() >= kMaxKeys) {
      return 0;
    }
    it = g_counts.emplace(key, 0).first;
  }
  return ++it->second;
}

const char* route_name(uint32_t route) noexcept {
  static constexpr std::array<const char*, 12> names{
      "world",        "head-locked",     "screen-2d",   "fullscreen", "per-eye-resolve", "skip",
      "head-locked-2d", "sky",           "floating-hud", "anchored-screen", "anchored-3d", "floating-3d",
  };
  return route < names.size() ? names[route] : "unknown";
}

void readback(const wgpu::CommandEncoder& cmd, std::vector<AfterSubmitCallback>& afterSubmit,
              const wgpu::Texture& texture, wgpu::Extent3D size, uint32_t layer, std::string label) noexcept {
  if (!texture) {
    Log.info("{}: no texture to read back", label);
    return;
  }
  const auto format = texture.GetFormat();
  const uint32_t bytes = texel_bytes(format);
  if (bytes == 0) {
    Log.info("{}: format {} not read back", label, static_cast<uint32_t>(format));
    return;
  }
  if (!(texture.GetUsage() & wgpu::TextureUsage::CopySrc)) {
    Log.info("{}: texture has no CopySrc usage, not read back", label);
    return;
  }
  if (size.width == 0 || size.height == 0) {
    Log.info("{}: empty texture ({}x{})", label, size.width, size.height);
    return;
  }
  {
    std::lock_guard lock{g_mutex};
    if (g_readbacks >= kMaxReadbacks) {
      return;
    }
    ++g_readbacks;
  }
  const wgpu::BufferDescriptor descriptor{
      .label = "Copy diag readback",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
      .size = kRowBytes * kPoints,
  };
  auto buffer = webgpu::g_device.CreateBuffer(&descriptor);
  const std::array<std::array<uint32_t, 2>, kPoints> points{{
      {size.width / 2, size.height / 2},
      {size.width / 4, size.height / 4},
      {size.width * 3 / 4, size.height / 4},
      {size.width / 4, size.height * 3 / 4},
      {size.width * 3 / 4, size.height * 3 / 4},
  }};
  for (uint32_t i = 0; i < kPoints; ++i) {
    const wgpu::TexelCopyTextureInfo src{
        .texture = texture,
        .origin = {points[i][0], points[i][1], layer},
    };
    const wgpu::TexelCopyBufferInfo dst{
        .layout = {.offset = static_cast<uint64_t>(kRowBytes) * i, .bytesPerRow = kRowBytes, .rowsPerImage = 1},
        .buffer = buffer,
    };
    const wgpu::Extent3D one{1, 1, 1};
    cmd.CopyTextureToBuffer(&src, &dst, &one);
  }
  afterSubmit.emplace_back([buffer, bytes, label = std::move(label), format, size, layer] {
    buffer.MapAsync(
        wgpu::MapMode::Read, 0, kRowBytes * kPoints, wgpu::CallbackMode::AllowSpontaneous,
        [buffer, bytes, label, format, size, layer](wgpu::MapAsyncStatus status, wgpu::StringView message) {
          if (status != wgpu::MapAsyncStatus::Success) {
            Log.info("{}: readback failed ({}): {}", label, static_cast<uint32_t>(status), std::string_view{message});
            return;
          }
          const auto* mapped = static_cast<const uint8_t*>(buffer.GetConstMappedRange(0, kRowBytes * kPoints));
          if (mapped == nullptr) {
            Log.info("{}: readback not mapped", label);
            return;
          }
          std::string values;
          bool flat = true;
          for (uint32_t i = 0; i < kPoints; ++i) {
            values += (i == 0 ? "" : " ") + hex(mapped + kRowBytes * i, bytes);
            flat = flat && std::memcmp(mapped, mapped + kRowBytes * i, bytes) == 0;
          }
          buffer.Unmap();
          Log.info("{}: {}x{} layer {} format {}: texels centre,q1..q4 = {}{}", label, size.width, size.height, layer,
                   static_cast<uint32_t>(format), values, flat ? " (ALL EQUAL)" : "");
        });
  });
}


void readback_uniform(const wgpu::CommandEncoder& cmd, std::vector<AfterSubmitCallback>& afterSubmit,
                      const wgpu::Buffer& buffer, uint64_t offset, uint64_t size, std::string label) noexcept {
  if (!buffer || size < 20) {
    Log.info("{}: no uniform to read back (size {})", label, size);
    return;
  }
  constexpr uint64_t kBytes = 32;
  const wgpu::BufferDescriptor descriptor{
      .label = "Copy diag uniform readback",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
      .size = kBytes,
  };
  auto readback = webgpu::g_device.CreateBuffer(&descriptor);
  const uint64_t bytes = std::min<uint64_t>(kBytes, (size + 3) & ~uint64_t{3});
  cmd.CopyBufferToBuffer(buffer, offset, readback, 0, bytes);
  afterSubmit.emplace_back([readback, label = std::move(label), offset, size] {
    readback.MapAsync(wgpu::MapMode::Read, 0, kBytes, wgpu::CallbackMode::AllowSpontaneous,
                      [readback, label, offset, size](wgpu::MapAsyncStatus status, wgpu::StringView message) {
                        if (status != wgpu::MapAsyncStatus::Success) {
                          Log.info("{}: uniform readback failed ({}): {}", label, static_cast<uint32_t>(status),
                                   std::string_view{message});
                          return;
                        }
                        const auto* m = static_cast<const uint8_t*>(readback.GetConstMappedRange(0, kBytes));
                        if (m == nullptr) {
                          return;
                        }
                        std::array<float, 4> f{};
                        uint32_t flag = 0;
                        std::memcpy(f.data(), m, sizeof(f));
                        std::memcpy(&flag, m + 16, sizeof(flag));
                        readback.Unmap();
                        Log.info("{}: UV transform at uniform offset {} (size {}): offset ({}, {}) scale ({}, {}) opaque {}",
                                 label, offset, size, f[0], f[1], f[2], f[3], flag);
                      });
  });
}

namespace {
struct ProbeVariant {
  const char* name;
  bool uniform;     // binds the UV transform
  bool bigBuffer;   // the uniform sits 16 MiB into a 24 MiB buffer instead of at 0 in a small one
  bool colorOut;    // the result is a readout of the uniform, not a copy of the pattern
  const char* vs;   // vertex body (vi, o)
  const char* fs;   // fragment body (i)
};

constexpr const char* kArrayPos = "o.pos = vec4f(P[vi], 0.0, 1.0);";
constexpr const char* kArithPos =
    "let t = vec2f(f32(vi & 2u), f32((vi << 1u) & 2u)); o.pos = vec4f(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);";

std::string probe_source(const ProbeVariant& v) {
  std::string s = R"(
@group(0) @binding(0) var samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;
struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f, };
var<private> P: array<vec2f, 3> = array(vec2f(-1.0, 1.0), vec2f(-1.0, -3.0), vec2f(3.0, 1.0));
var<private> T: array<vec2f, 3> = array(vec2f(0.0, 0.0), vec2f(0.0, 2.0), vec2f(2.0, 0.0));
)";
  if (v.uniform) {
    s += "struct U { offset: vec2f, scale: vec2f, flag: u32, };\n@group(0) @binding(2) var<uniform> u: U;\n";
  }
  s += fmt::format("@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VO {{\n  var o: VO;\n  {}\n  return o;\n}}\n", v.vs);
  s += fmt::format("@fragment fn fs_main(i: VO) -> @location(0) vec4f {{\n  {}\n}}\n", v.fs);
  return s;
}
} // namespace

void run_shader_probe() noexcept {
  static std::once_flag once;
  std::call_once(once, [] {
    constexpr uint32_t N = 8;
    // The pattern: texel (x, y) = (32x, 32y, 0x80, 0xff).
    std::array<uint8_t, N * N * 4> pattern{};
    for (uint32_t y = 0; y < N; ++y) {
      for (uint32_t x = 0; x < N; ++x) {
        uint8_t* t = &pattern[(y * N + x) * 4];
        t[0] = static_cast<uint8_t>(x * 32);
        t[1] = static_cast<uint8_t>(y * 32);
        t[2] = 0x80;
        t[3] = 0xff;
      }
    }
    auto& device = webgpu::g_device;
    auto& queue = webgpu::g_queue;
    const wgpu::TextureDescriptor srcDesc{
        .label = "Copy probe source",
        .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
        .size = {N, N, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
    };
    const auto srcTex = device.CreateTexture(&srcDesc);
    {
      const wgpu::TexelCopyTextureInfo dst{.texture = srcTex};
      const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = N * 4, .rowsPerImage = N};
      const wgpu::Extent3D extent{N, N, 1};
      queue.WriteTexture(&dst, pattern.data(), pattern.size(), &layout, &extent);
    }
    const auto srcView = srcTex.CreateView();
    const wgpu::SamplerDescriptor samplerDesc{
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Nearest,
        .minFilter = wgpu::FilterMode::Nearest,
    };
    const auto sampler = device.CreateSampler(&samplerDesc);

    // The UV transform: offset (0, 0), scale (1, 1): an exact copy.
    const std::array<float, 8> uniformData{0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f};
    constexpr uint64_t kBigSize = 24ull << 20;
    constexpr uint64_t kBigOffset = 16ull << 20;
    const wgpu::BufferDescriptor smallDesc{
        .label = "Copy probe uniform (small)",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = 256,
    };
    const auto smallBuffer = device.CreateBuffer(&smallDesc);
    queue.WriteBuffer(smallBuffer, 0, uniformData.data(), sizeof(uniformData));
    const wgpu::BufferDescriptor bigDesc{
        .label = "Copy probe uniform (24 MiB)",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = kBigSize,
    };
    const auto bigBuffer = device.CreateBuffer(&bigDesc);
    queue.WriteBuffer(bigBuffer, kBigOffset, uniformData.data(), sizeof(uniformData));

    static constexpr std::array<ProbeVariant, 9> variants{{
        // tex_copy_conv's own shape.
        {"array+uniform(vs)", true, false, false, "o.pos = vec4f(P[vi], 0.0, 1.0); o.uv = T[vi] * u.scale + u.offset;",
         "return textureSample(src, samp, i.uv);"},
        {"array+uniform(vs) 16MiB in", true, true, false,
         "o.pos = vec4f(P[vi], 0.0, 1.0); o.uv = T[vi] * u.scale + u.offset;", "return textureSample(src, samp, i.uv);"},
        {"array only", false, false, false, "o.pos = vec4f(P[vi], 0.0, 1.0); o.uv = T[vi];",
         "return textureSample(src, samp, i.uv);"},
        {"arith+uniform(vs)", true, false, false,
         "let t = vec2f(f32(vi & 2u), f32((vi << 1u) & 2u)); o.pos = vec4f(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0); "
         "o.uv = t * u.scale + u.offset;",
         "return textureSample(src, samp, i.uv);"},
        {"arith only", false, false, false,
         "let t = vec2f(f32(vi & 2u), f32((vi << 1u) & 2u)); o.pos = vec4f(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0); "
         "o.uv = t;",
         "return textureSample(src, samp, i.uv);"},
        {"array, uniform(fs)", true, false, false, "o.pos = vec4f(P[vi], 0.0, 1.0); o.uv = T[vi];",
         "return textureSample(src, samp, i.uv * u.scale + u.offset);"},
        // Readouts: expect 80 40 80 ff (scale.x / 2, scale.y / 4, offset.x + 0.5).
        {"uniform readout (fs)", true, false, true, "o.pos = vec4f(P[vi], 0.0, 1.0); o.uv = T[vi];",
         "return vec4f(u.scale.x * 0.5, u.scale.y * 0.25, u.offset.x + 0.5, 1.0);"},
        {"uniform readout (vs)", true, false, true,
         "o.pos = vec4f(P[vi], 0.0, 1.0); o.uv = vec2f(u.scale.x * 0.5, u.scale.y * 0.25);",
         "return vec4f(i.uv, 0.5, 1.0);"},
        {"uniform readout (vs) 16MiB in", true, true, true,
         "o.pos = vec4f(P[vi], 0.0, 1.0); o.uv = vec2f(u.scale.x * 0.5, u.scale.y * 0.25);",
         "return vec4f(i.uv, 0.5, 1.0);"},
    }};
    (void)kArrayPos;
    (void)kArithPos;

    // Explicit layouts: an automatic one leaves out what a variant's shader doesn't use (the readouts never
    // sample the texture), and a bind group with an entry its layout lacks aborts with validation skipped.
    const auto stages = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    const std::array layoutEntries{
        wgpu::BindGroupLayoutEntry{
            .binding = 0, .visibility = stages, .sampler = {.type = wgpu::SamplerBindingType::Filtering}},
        wgpu::BindGroupLayoutEntry{
            .binding = 1,
            .visibility = stages,
            .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
        },
        wgpu::BindGroupLayoutEntry{
            .binding = 2,
            .visibility = stages,
            .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = sizeof(uniformData)},
        },
    };
    std::array<wgpu::BindGroupLayout, 2> bindGroupLayouts; // [0] without the uniform, [1] with it
    std::array<wgpu::PipelineLayout, 2> pipelineLayouts;
    for (size_t withUniform = 0; withUniform < 2; ++withUniform) {
      const wgpu::BindGroupLayoutDescriptor bglDesc{
          .label = "Copy probe bind group layout",
          .entryCount = withUniform ? 3u : 2u,
          .entries = layoutEntries.data(),
      };
      bindGroupLayouts[withUniform] = device.CreateBindGroupLayout(&bglDesc);
      const wgpu::PipelineLayoutDescriptor plDesc{
          .label = "Copy probe pipeline layout",
          .bindGroupLayoutCount = 1,
          .bindGroupLayouts = &bindGroupLayouts[withUniform],
      };
      pipelineLayouts[withUniform] = device.CreatePipelineLayout(&plDesc);
    }

    const wgpu::CommandEncoderDescriptor encDesc{.label = "Copy probe"};
    auto cmd = device.CreateCommandEncoder(&encDesc);
    struct Result {
      const ProbeVariant* variant;
      wgpu::Buffer readback;
      wgpu::Texture dst;
    };
    std::vector<Result> results;
    for (const auto& v : variants) {
      const std::string source = probe_source(v);
      const wgpu::ShaderSourceWGSL wgsl{wgpu::ShaderSourceWGSL::Init{.code = source.c_str()}};
      const wgpu::ShaderModuleDescriptor moduleDesc{.nextInChain = &wgsl, .label = v.name};
      const auto module = device.CreateShaderModule(&moduleDesc);
      const std::array targets{wgpu::ColorTargetState{.format = wgpu::TextureFormat::RGBA8Unorm}};
      const wgpu::FragmentState fragment{
          .module = module,
          .entryPoint = "fs_main",
          .targetCount = targets.size(),
          .targets = targets.data(),
      };
      const wgpu::RenderPipelineDescriptor pipeDesc{
          .label = v.name,
          .layout = pipelineLayouts[v.uniform ? 1 : 0],
          .vertex = wgpu::VertexState{.module = module, .entryPoint = "vs_main"},
          .primitive = wgpu::PrimitiveState{.topology = wgpu::PrimitiveTopology::TriangleList},
          .fragment = &fragment,
      };
      const auto pipeline = device.CreateRenderPipeline(&pipeDesc);
      std::vector<wgpu::BindGroupEntry> entries{
          wgpu::BindGroupEntry{.binding = 0, .sampler = sampler},
          wgpu::BindGroupEntry{.binding = 1, .textureView = srcView},
      };
      if (v.uniform) {
        entries.push_back(wgpu::BindGroupEntry{
            .binding = 2,
            .buffer = v.bigBuffer ? bigBuffer : smallBuffer,
            .offset = v.bigBuffer ? kBigOffset : 0,
            .size = sizeof(uniformData),
        });
      }
      const wgpu::BindGroupDescriptor bgDesc{
          .layout = bindGroupLayouts[v.uniform ? 1 : 0],
          .entryCount = entries.size(),
          .entries = entries.data(),
      };
      const auto bindGroup = device.CreateBindGroup(&bgDesc);
      const wgpu::TextureDescriptor dstDesc{
          .label = "Copy probe target",
          .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,
          .size = {N, N, 1},
          .format = wgpu::TextureFormat::RGBA8Unorm,
      };
      auto dst = device.CreateTexture(&dstDesc);
      const std::array attachments{wgpu::RenderPassColorAttachment{
          .view = dst.CreateView(),
          .loadOp = wgpu::LoadOp::Clear,
          .storeOp = wgpu::StoreOp::Store,
          .clearValue = {0.0, 0.0, 0.0, 0.0},
      }};
      const wgpu::RenderPassDescriptor passDesc{
          .label = "Copy probe pass",
          .colorAttachmentCount = attachments.size(),
          .colorAttachments = attachments.data(),
      };
      auto pass = cmd.BeginRenderPass(&passDesc);
      pass.SetPipeline(pipeline);
      pass.SetBindGroup(0, bindGroup);
      pass.Draw(3);
      pass.End();
      const wgpu::BufferDescriptor rbDesc{
          .label = "Copy probe readback",
          .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
          .size = kRowBytes * N,
      };
      auto readback = device.CreateBuffer(&rbDesc);
      const wgpu::TexelCopyTextureInfo from{.texture = dst};
      const wgpu::TexelCopyBufferInfo to{.layout = {.bytesPerRow = kRowBytes, .rowsPerImage = N}, .buffer = readback};
      const wgpu::Extent3D extent{N, N, 1};
      cmd.CopyTextureToBuffer(&from, &to, &extent);
      results.push_back({&v, readback, dst});
    }
    const auto commands = cmd.Finish();
    queue.Submit(1, &commands);
    Log.info("shader probe: {} variants submitted (8x8 pattern, texel (x,y) = (32x, 32y, 80, ff))", results.size());
    for (const auto& r : results) {
      r.readback.MapAsync(
          wgpu::MapMode::Read, 0, kRowBytes * N, wgpu::CallbackMode::AllowSpontaneous,
          [r, pattern, srcTex, smallBuffer, bigBuffer](wgpu::MapAsyncStatus status, wgpu::StringView message) {
            const char* name = r.variant->name;
            if (status != wgpu::MapAsyncStatus::Success) {
              Log.info("shader probe {}: readback failed ({}): {}", name, static_cast<uint32_t>(status),
                       std::string_view{message});
              return;
            }
            const auto* m = static_cast<const uint8_t*>(r.readback.GetConstMappedRange(0, kRowBytes * N));
            if (m == nullptr) {
              Log.info("shader probe {}: not mapped", name);
              return;
            }
            uint32_t matches = 0;
            uint32_t equalFirst = 0;
            for (uint32_t y = 0; y < N; ++y) {
              for (uint32_t x = 0; x < N; ++x) {
                const uint8_t* got = m + kRowBytes * y + x * 4;
                matches += std::memcmp(got, &pattern[(y * N + x) * 4], 4) == 0;
                equalFirst += std::memcmp(got, m, 4) == 0;
              }
            }
            const std::string row0 = hex(m, 16);
            const std::string row7 = hex(m + kRowBytes * 7 + 16, 16);
            r.readback.Unmap();
            if (r.variant->colorOut) {
              Log.info("shader probe {}: texel (0,0) = {} (expect 804080ff), {} of 64 texels the same", name, row0.substr(0, 8),
                       equalFirst);
            } else {
              Log.info("shader probe {}: {} of 64 texels match the pattern, {} of 64 equal to texel (0,0); row 0 starts {}, "
                       "row 7 ends {}{}",
                       name, matches, equalFirst, row0, row7, matches == 64 ? " (PASS)" : " (FAIL)");
            }
          });
    }
  });
}

} // namespace aurora::gfx::copy_diag
