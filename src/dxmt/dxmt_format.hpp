#pragma once

#include "Metal.hpp"
#include <map>

namespace dxmt {
enum class FormatCapability : int {
  None = 0,
  Atomic = 0x1,
  Filter = 0x2,
  Write = 0x4,
  Color = 0x8,
  Blend = 0x10,
  MSAA = 0x20,
  Sparse = 0x40,
  Resolve = 0x80,
  DepthStencil = 0x100,
  TextureBufferRead = 0x200,
  TextureBufferWrite = 0x400,
  TextureBufferReadWrite = 0x800
};

class FormatCapabilityInspector {
public:
  std::map<WMTPixelFormat, FormatCapability> textureCapabilities{};
  void Inspect(WMT::Device device);
};

WMTPixelFormat Forget_sRGB(WMTPixelFormat format);
WMTPixelFormat Recall_sRGB(WMTPixelFormat format);

inline bool
Is_sRGBVariant(WMTPixelFormat format) {
  return Forget_sRGB(format) != format;
}

bool IsBlockCompressionFormat(WMTPixelFormat format);

uint32_t DepthStencilPlanarFlags(WMTPixelFormat format);

enum MTL_DXGI_FORMAT_FLAG {
  MTL_DXGI_FORMAT_TYPELESS = 1,
  MTL_DXGI_FORMAT_BC = 2,
  MTL_DXGI_FORMAT_BACKBUFFER = 4,
  MTL_DXGI_FORMAT_DEPTH_PLANER = 16,
  MTL_DXGI_FORMAT_STENCIL_PLANER = 32,
  MTL_DXGI_FORMAT_EMULATED_D24 = 256,
  MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL = 512,
};

struct MTL_DXGI_FORMAT_DESC {
  WMTPixelFormat PixelFormat;
  WMTAttributeFormat AttributeFormat;
  union {
    uint32_t BytesPerTexel;
    uint32_t BlockSize;
  };
  uint16_t PlanarCount;
  uint16_t Flag;
};

int32_t MTLQueryDXGIFormat(WMT::Device device, uint32_t format, MTL_DXGI_FORMAT_DESC &description);

/**
 * Layout of a multi-planar (4:2:0) YUV format. Each plane is backed by its own texture.
 * Plane 0 is luma at full resolution, plane 1 is interleaved chroma at half width and half height.
 */
struct MTL_DXGI_PLANAR_FORMAT_DESC {
  uint32_t PlaneCount;
  uint32_t PlaneFormat[2]; // DXGI_FORMAT of the view that addresses each plane
  WMTPixelFormat PlanePixelFormat[2];
  uint32_t PlaneBytesPerTexel[2];
  uint32_t PlaneSubsampleShift[2]; // log2 of the subsampling factor, applied to both width and height
};

/**
 * \returns true if `format` is a supported multi-planar format, filling `description`
 */
bool MTLQueryDXGIPlanarFormat(uint32_t format, MTL_DXGI_PLANAR_FORMAT_DESC &description);

uint32_t MTLGetTexelSize(WMTPixelFormat format);

WMTPixelFormat MTLGetUnsignedIntegerFormat(WMTPixelFormat format);

bool IsUnorm8RenderTargetFormat(WMTPixelFormat format);

bool IsIntegerFormat(WMTPixelFormat format);

void SanitizeRTVClearColor(WMTPixelFormat format, WMTClearColor &color);

} // namespace dxmt