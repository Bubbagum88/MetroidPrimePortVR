#include "copy_diag.hpp"

#include "../logging.hpp"
#include "../webgpu/gpu.hpp"

#include <absl/container/flat_hash_map.h>
#include <fmt/format.h>

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

} // namespace aurora::gfx::copy_diag
