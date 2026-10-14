#include "d3d11_private.h"
#include "d3d11_enumerable.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_format.hpp"
#include "dxmt_staging.hpp"
#include "d3d11_resource.hpp"
namespace dxmt {

#pragma region StagingBuffer

class StagingBuffer : public TResourceBase<tag_buffer> {
private:
  Rc<StagingResource> internal;

public:
  StagingBuffer(const tag_buffer::DESC1 *pDesc, MTLD3D11Device *device,
                Rc<StagingResource> &&internal)
      : TResourceBase<tag_buffer>(*pDesc, device),
        internal(std::move(internal)) {}

  void OnSetDebugObjectName(LPCSTR Name) override {
  }

  Rc<StagingResource>
  staging(UINT Subresource) final {
    assert(Subresource == 0);
    return internal;
  }
  Rc<DynamicBuffer>
  dynamicBuffer(UINT*, UINT*) final {
    return {};
  };
  Rc<DynamicLinearTexture>
  dynamicLinearTexture(UINT *, UINT *) final {
    return {};
  };
  Rc<DynamicBuffer>
  dynamicTexture(UINT , UINT *, UINT *) final {
    return {};
  };
};

HRESULT
CreateStagingBuffer(MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc,
                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                    ID3D11Buffer **ppBuffer) {
  auto metal = pDevice->GetMTLDevice();
  auto byte_width = pDesc->ByteWidth;
  auto buffer = new StagingResource(metal, byte_width, byte_width, byte_width);
  if (pInitialData) {
    memcpy(buffer->mappedImmediateMemory(), pInitialData->pSysMem, byte_width);
  }
  *ppBuffer = reinterpret_cast<ID3D11Buffer *>(ref(new StagingBuffer(pDesc, pDevice, buffer)));
  return S_OK;
}

#pragma endregion

#pragma region StagingTexture

template <typename tag_texture>
class StagingTexture : public TResourceBase<tag_texture> {
  std::vector<Rc<StagingResource>> subresources;
  uint32_t subresource_count;

public:
  StagingTexture(const tag_texture::DESC1 *pDesc, MTLD3D11Device *pDevice,
                 std::vector<Rc<StagingResource>> &&subresources)
      : TResourceBase<tag_texture>(*pDesc, pDevice),
        subresources(std::move(subresources)),
        subresource_count(this->subresources.size()) {}

  void OnSetDebugObjectName(LPCSTR Name) override {
    if (!Name) {
      return;
    }
  }

  Rc<StagingResource>
  staging(UINT Subresource) final {
    return subresources.at(Subresource);
  }
  Rc<DynamicBuffer>
  dynamicBuffer(UINT *, UINT *) final {
    return {};
  };
  Rc<DynamicLinearTexture>
  dynamicLinearTexture(UINT *, UINT *) final {
    return {};
  };
  Rc<DynamicBuffer>
  dynamicTexture(UINT , UINT *, UINT *) final {
    return {};
  };
};

#pragma endregion

template <typename tag>
HRESULT CreateStagingTextureInternal(MTLD3D11Device *pDevice,
                                     const typename tag::DESC1 *pDesc,
                                     const D3D11_SUBRESOURCE_DATA *pInitialData,
                                     typename tag::COM_IMPL **ppTexture) {
  auto metal = pDevice->GetMTLDevice();
  typename tag::DESC1 finalDesc;
  WMTTextureInfo texDesc; // unused
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc, &texDesc))) {
    return E_INVALIDARG;
  }
  std::vector<Rc<StagingResource>> subresources;
  bool is_1d_tex = tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D;
  bool is_3d_tex = tag::dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
  for (auto &sub : EnumerateSubresources(finalDesc)) {
    uint32_t w, h, d;
    GetMipmapSize(&finalDesc, sub.MipLevel, &w, &h, &d);
    uint32_t bpr, bpi, buf_len;
    if (FAILED(GetLinearTextureLayout(pDevice, finalDesc, sub.MipLevel, bpr, bpi, buf_len))) {
      return E_FAIL;
    }
    D3D11_ASSERT(subresources.size() == sub.SubresourceId);
    auto buffer = new StagingResource(metal, buf_len, bpr, bpi);
    if (pInitialData) {
      auto mapped = buffer->mappedImmediateMemory();
      auto bpi_read = is_3d_tex ? pInitialData[sub.SubresourceId].SysMemSlicePitch : 0;
      auto bpr_read = is_1d_tex ? 0 : pInitialData[sub.SubresourceId].SysMemPitch;
      for (auto image = 0u; image < d; image++) {
        for (auto row = 0u; row < (bpi / bpr); row++) {
          auto dst_data = ptr_add(mapped, image * bpi + row * bpr);
          auto src_data = ptr_add(pInitialData[sub.SubresourceId].pSysMem, image * bpi_read + row * bpr_read);
          memcpy(dst_data, src_data, std::min(bpr, bpr_read));
        }
      }
    }
    subresources.push_back(buffer);
  }

  *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(ref(
      new StagingTexture<tag>(&finalDesc, pDevice, std::move(subresources))));
  return S_OK;
}

/**
 * A staging texture of a multi-planar format has one linear allocation per array slice,
 * holding all planes in the layout returned by `GetPlanarLinearTextureLayout`.
 */
static HRESULT
CreatePlanarStagingTexture2D(
    MTLD3D11Device *pDevice, const D3D11_TEXTURE2D_DESC1 *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Texture2D1 **ppTexture
) {
  MTL_DXGI_PLANAR_FORMAT_DESC planar;
  if (!MTLQueryDXGIPlanarFormat(pDesc->Format, planar))
    return E_INVALIDARG;
  if (pDesc->Width == 0 || pDesc->Height == 0 || pDesc->ArraySize == 0)
    return E_INVALIDARG;
  if ((pDesc->Width | pDesc->Height) & 1)
    return E_INVALIDARG;
  if (pDesc->MipLevels > 1 || (pDesc->MipLevels == 0 && (pDesc->Width > 1 || pDesc->Height > 1)))
    return E_INVALIDARG;
  if (pDesc->SampleDesc.Count != 1 || (pDesc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE))
    return E_INVALIDARG;

  D3D11_TEXTURE2D_DESC1 finalDesc = *pDesc;
  finalDesc.MipLevels = 1;

  uint32_t bytes_per_row, plane1_offset, bytes_per_slice;
  if (FAILED(GetPlanarLinearTextureLayout(pDevice, finalDesc, bytes_per_row, plane1_offset, bytes_per_slice)))
    return E_FAIL;

  auto metal = pDevice->GetMTLDevice();
  std::vector<Rc<StagingResource>> subresources;
  for (unsigned slice = 0; slice < finalDesc.ArraySize; slice++) {
    auto buffer = new StagingResource(metal, bytes_per_slice, bytes_per_row, bytes_per_slice);
    if (pInitialData) {
      auto &data = pInitialData[slice];
      auto mapped = static_cast<char *>(buffer->mappedImmediateMemory());
      for (unsigned plane = 0; plane < planar.PlaneCount; plane++) {
        auto shift = planar.PlaneSubsampleShift[plane];
        auto row_size = (finalDesc.Width >> shift) * planar.PlaneBytesPerTexel[plane];
        auto src = static_cast<const char *>(data.pSysMem) + (plane ? size_t(data.SysMemPitch) * finalDesc.Height : 0);
        auto dst = mapped + (plane ? plane1_offset : 0);
        for (unsigned row = 0; row < (finalDesc.Height >> shift); row++)
          memcpy(dst + size_t(row) * bytes_per_row, src + size_t(row) * data.SysMemPitch, row_size);
      }
    }
    subresources.push_back(buffer);
  }

  *ppTexture = reinterpret_cast<ID3D11Texture2D1 *>(
      ref(new StagingTexture<tag_texture_2d>(&finalDesc, pDevice, std::move(subresources)))
  );
  return S_OK;
}

HRESULT
CreateStagingTexture1D(MTLD3D11Device *pDevice,
                       const D3D11_TEXTURE1D_DESC *pDesc,
                       const D3D11_SUBRESOURCE_DATA *pInitialData,
                       ID3D11Texture1D **ppTexture) {
  return CreateStagingTextureInternal<tag_texture_1d>(pDevice, pDesc,
                                                      pInitialData, ppTexture);
}

HRESULT
CreateStagingTexture2D(MTLD3D11Device *pDevice,
                       const D3D11_TEXTURE2D_DESC1 *pDesc,
                       const D3D11_SUBRESOURCE_DATA *pInitialData,
                       ID3D11Texture2D1 **ppTexture) {
  if (MTL_DXGI_PLANAR_FORMAT_DESC planar; MTLQueryDXGIPlanarFormat(pDesc->Format, planar))
    return CreatePlanarStagingTexture2D(pDevice, pDesc, pInitialData, ppTexture);
  return CreateStagingTextureInternal<tag_texture_2d>(pDevice, pDesc,
                                                      pInitialData, ppTexture);
}

HRESULT
CreateStagingTexture3D(MTLD3D11Device *pDevice,
                       const D3D11_TEXTURE3D_DESC1 *pDesc,
                       const D3D11_SUBRESOURCE_DATA *pInitialData,
                       ID3D11Texture3D1 **ppTexture) {
  return CreateStagingTextureInternal<tag_texture_3d>(pDevice, pDesc,
                                                      pInitialData, ppTexture);
}

} // namespace dxmt