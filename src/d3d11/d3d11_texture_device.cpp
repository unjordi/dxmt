/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Copyright 2026 Marc-Aurel Zent for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "d3d11_enumerable.hpp"
#include "d3d11_view.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_staging.hpp"
#include "dxmt_texture.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"

namespace dxmt {

#pragma region DeviceTexture

template <typename tag_texture>
class DeviceTexture : public TResourceBase<tag_texture, IMTLMinLODClampable> {
private:
  Rc<RenamableTexturePool> renamable_;
  float min_lod = 0.0;
  D3DKMT_HANDLE local_kmt_ = 0;
  D3DKMT_HANDLE global_kmt_ = 0;

  using SRVBase =
      TResourceViewBase<tag_shader_resource_view<DeviceTexture<tag_texture>>>;
  class TextureSRV : public SRVBase {
  public:
    TextureSRV(const TextureViewDescriptor &descriptor,
               const tag_shader_resource_view<>::DESC1 *pDesc,
               DeviceTexture *pResource, Texture *pTexture, MTLD3D11Device *pDevice)
        : SRVBase(pDesc, pResource, pDevice) {
      this->texture_ = pTexture;
      this->view_id_ = this->texture_->createView(descriptor);
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }

  };

  using UAVBase =
      TResourceViewBase<tag_unordered_access_view<DeviceTexture<tag_texture>>>;
  class TextureUAV : public UAVBase {
  public:
    TextureUAV(const TextureViewDescriptor &descriptor,
               const tag_unordered_access_view<>::DESC1 *pDesc,
               DeviceTexture *pResource, Texture *pTexture, MTLD3D11Device *pDevice)
        : UAVBase(pDesc, pResource, pDevice) {
      this->texture_ = pTexture;
      this->view_id_ = this->texture_->createView(descriptor);
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }
  };

  using RTVBase =
      TResourceViewBase<tag_render_target_view<DeviceTexture<tag_texture>>>;
  class TextureRTV : public RTVBase {
  public:
    TextureRTV(
        const TextureViewDescriptor &descriptor, WMTPixelFormat view_format, const tag_render_target_view<>::DESC1 *pDesc,
        DeviceTexture *pResource, Texture *pTexture, MTLD3D11Device *pDevice,
        const MTL_RENDER_PASS_ATTACHMENT_DESC &mtl_rtv_desc
    ) :
        RTVBase(pDesc, pResource, pDevice) {
      this->texture_ = pTexture;
      this->view_id_ = this->texture_->createView(descriptor);
      this->format_ = view_format;
      this->pass_desc_ = mtl_rtv_desc;
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }
  };

  using DSVBase =
      TResourceViewBase<tag_depth_stencil_view<DeviceTexture<tag_texture>>>;
  class TextureDSV : public DSVBase {
  public:
    TextureDSV(
        const TextureViewDescriptor &descriptor, WMTPixelFormat view_format, const tag_depth_stencil_view<>::DESC1 *pDesc,
        DeviceTexture *pResource, MTLD3D11Device *pDevice, const MTL_RENDER_PASS_ATTACHMENT_DESC &attachment_desc
    ) :
        DSVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->texture_.ptr();
      this->view_id_ = this->texture_->createView(descriptor);
      this->format_ = view_format;
      this->pass_desc_ = attachment_desc;
      this->renamable_ = pResource->renamable_.ptr();
      this->readonly_flags_ = this->desc.Flags & 0b11;
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength(),
        this->readonly_flags_
      );
    }
  };

  /**
   * Selects the texture a view addresses. For a multi-planar texture the plane is taken from
   * `PlaneSlice` if non-zero, otherwise from the texel size of the view format (as in D3D11, where
   * e.g. an R8 view of NV12 addresses luma and an R8G8 view addresses chroma).
   */
  template <typename VIEW_DESC>
  HRESULT
  ResolveViewPlane(VIEW_DESC &ViewDesc, Texture **ppTexture, UINT **ppPlaneSlice) {
    *ppPlaneSlice = nullptr;
    if (!this->texture_plane1_) {
      *ppTexture = this->texture_.ptr();
      return S_OK;
    }
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE2D_DESC1>) {
      MTL_DXGI_PLANAR_FORMAT_DESC planar;
      if (!MTLQueryDXGIPlanarFormat(this->desc.Format, planar))
        return E_FAIL;
      MTL_DXGI_FORMAT_DESC view_format;
      if (FAILED(MTLQueryDXGIFormat(this->m_parent->GetMTLDevice(), ViewDesc.Format, view_format)))
        return E_INVALIDARG;
      UINT *plane_slice = nullptr;
      static_assert(UINT(D3D11_SRV_DIMENSION_TEXTURE2D) == UINT(D3D11_RTV_DIMENSION_TEXTURE2D));
      static_assert(UINT(D3D11_SRV_DIMENSION_TEXTURE2D) == UINT(D3D11_UAV_DIMENSION_TEXTURE2D));
      static_assert(UINT(D3D11_SRV_DIMENSION_TEXTURE2DARRAY) == UINT(D3D11_RTV_DIMENSION_TEXTURE2DARRAY));
      static_assert(UINT(D3D11_SRV_DIMENSION_TEXTURE2DARRAY) == UINT(D3D11_UAV_DIMENSION_TEXTURE2DARRAY));
      if (UINT(ViewDesc.ViewDimension) == UINT(D3D11_SRV_DIMENSION_TEXTURE2D))
        plane_slice = &ViewDesc.Texture2D.PlaneSlice;
      else if (UINT(ViewDesc.ViewDimension) == UINT(D3D11_SRV_DIMENSION_TEXTURE2DARRAY))
        plane_slice = &ViewDesc.Texture2DArray.PlaneSlice;
      else
        return E_INVALIDARG;
      uint32_t plane;
      if (*plane_slice)
        plane = *plane_slice;
      else
        plane = view_format.BytesPerTexel == planar.PlaneBytesPerTexel[1] ? 1 : 0;
      if (plane >= planar.PlaneCount || view_format.BytesPerTexel != planar.PlaneBytesPerTexel[plane]) {
        ERR("DeviceTexture: view format ", ViewDesc.Format, " is incompatible with plane ", plane, " of ", this->desc.Format);
        return E_INVALIDARG;
      }
      *ppTexture = plane ? this->texture_plane1_.ptr() : this->texture_.ptr();
      *ppPlaneSlice = plane_slice;
      return S_OK;
    }
    return E_FAIL;
  }

public:
  DeviceTexture(const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, MTLD3D11Device *pDevice) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice) {
        this->texture_ = std::move(u_texture);
      }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, Rc<RenamableTexturePool> &&renamable,
      MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      renamable_(std::move(renamable)) {
        this->texture_ = std::move(u_texture);
      }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, Rc<KeyedMutex> &&keyed_mutex,
      MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice) {
        this->texture_ = std::move(u_texture);
        this->keyed_mutex_ = std::move(keyed_mutex);
      }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, D3DKMT_HANDLE localHandle, D3DKMT_HANDLE globalHandle,
      Rc<KeyedMutex> && keyed_mutex, MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      local_kmt_(localHandle), global_kmt_(globalHandle) {
        this->texture_ = std::move(u_texture);
        this->keyed_mutex_ = std::move(keyed_mutex);
      }

  ~DeviceTexture() {
    if (local_kmt_) {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = this->m_parent->GetLocalD3DKMT();
      destroy.hResource = local_kmt_;
      D3DKMTDestroyAllocation(&destroy);
    }
  }

  Rc<StagingResource> staging(UINT) final { return nullptr; }
  Rc<DynamicBuffer> dynamicBuffer(UINT*, UINT*) final { return {}; }
  Rc<DynamicLinearTexture> dynamicLinearTexture(UINT*, UINT*) final { return {}; };
  Rc<DynamicBuffer> dynamicTexture(UINT , UINT *, UINT *) final { return {}; };

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc,
                                 ID3D11RenderTargetView1 **ppView) override {
    D3D11_RENDER_TARGET_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    Texture *texture;
    UINT *plane_slice;
    if (HRESULT hr = ResolveViewPlane(finalDesc, &texture, &plane_slice); FAILED(hr))
      return hr;
    UINT plane = plane_slice ? std::exchange(*plane_slice, 0) : 0;
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, texture, finalDesc,
            attachment_desc, descriptor
        ))) {
      return E_FAIL;
    }
    if (plane_slice)
      *plane_slice = plane;
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureRTV(descriptor, descriptor.format, &finalDesc, this, texture, this->m_parent, attachment_desc));
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE CreateDepthStencilView(const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
                                 ID3D11DepthStencilView **ppView) override {
    if (this->texture_plane1_)
      return E_INVALIDARG;
    D3D11_DEPTH_STENCIL_VIEW_DESC finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->texture_.ptr(), finalDesc,
            attachment_desc, descriptor
        ))) {
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureDSV(descriptor, descriptor.format, &finalDesc, this, this->m_parent, attachment_desc));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
                           ID3D11ShaderResourceView1 **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      ERR("DeviceTexture: Failed to create SRV descriptor");
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    Texture *texture;
    UINT *plane_slice;
    if (HRESULT hr = ResolveViewPlane(finalDesc, &texture, &plane_slice); FAILED(hr))
      return hr;
    UINT plane = plane_slice ? std::exchange(*plane_slice, 0) : 0;
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, texture, finalDesc, descriptor
        ))) {
      ERR("DeviceTexture: Failed to create texture SRV");
      return E_FAIL;
    }
    if (plane_slice)
      *plane_slice = plane;
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureSRV(descriptor, &finalDesc, this, texture, this->m_parent));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc,
                            ID3D11UnorderedAccessView1 **ppView) override {
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    Texture *texture;
    UINT *plane_slice;
    if (HRESULT hr = ResolveViewPlane(finalDesc, &texture, &plane_slice); FAILED(hr))
      return hr;
    UINT plane = plane_slice ? std::exchange(*plane_slice, 0) : 0;
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, texture, finalDesc, descriptor
        ))) {
      ERR("DeviceTexture: Failed to create texture UAV");
      return E_FAIL;
    }
    if (plane_slice)
      *plane_slice = plane;
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureUAV(descriptor, &finalDesc, this, texture, this->m_parent));
    return S_OK;
  };

  virtual HRESULT
  GetSharedHandle(HANDLE *pSharedHandle) override {
    return GetSharedResourceHandle(this->desc.MiscFlags, global_kmt_, pSharedHandle);
  }

  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    return CreateSharedResourceNtHandle(local_kmt_, Attributes, Access, pName, pNTHandle);
  }

  void SetMinLOD(float MinLod) override { min_lod = MinLod; }

  float GetMinLOD() override { return min_lod; }
};

template <typename tag>
HRESULT CreateDeviceTextureInternal(MTLD3D11Device *pDevice,
                                    const typename tag::DESC1 *pDesc,
                                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                                    typename tag::COM_IMPL **ppTexture) {
  WMTTextureInfo info;
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc, &info))) {
    return E_INVALIDARG;
  }
  bool single_subresource = info.mipmap_level_count == 1 && info.array_length == 1 &&
                            !(finalDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE);
  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));

  MTL_DXGI_FORMAT_DESC format_desc;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format_desc);

  auto &initializer = pDevice->GetDXMTDevice().queue().initializer;

  auto initialize = [&](Rc<TextureAllocation> &&allocation) {
    texture->rename(std::move(allocation));
    if (!pInitialData) {
      for (auto sub : EnumerateSubresources(finalDesc)) {
        initializer.initWithZero(texture.ptr(), texture->current(), sub.ArraySlice, sub.MipLevel);
      }
    } else {
      for (auto sub : EnumerateSubresources(finalDesc)) {
        auto &data = pInitialData[sub.SubresourceId];
        initializer.initWithData(
            texture.ptr(), texture->current(), sub.ArraySlice, sub.MipLevel, data.pSysMem, data.SysMemPitch,
            data.SysMemSlicePitch, format_desc.Flag
        );
      }
    }
  };

  auto shared_flag =
      D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (finalDesc.MiscFlags & shared_flag) {
    if (!(pDevice->GetLocalD3DKMT() & 0xc0000000)) {
      ERR("DeviceTexture: Invalid device handle");
      return E_FAIL;
    }
    // use a dedicated path for now, because there are other works for private storage mode

    Flags<TextureAllocationFlag> flags;
    flags.set(TextureAllocationFlag::GpuPrivate);
    if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
      flags.set(TextureAllocationFlag::GpuReadonly);
    if (!(finalDesc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
      flags.set(TextureAllocationFlag::ShaderReadonly);
    flags.set(TextureAllocationFlag::Shared);
    auto allocation = texture->allocate(flags);

    mach_port_t mach_port = allocation->machPort;
    if (!mach_port) {
      ERR("DeviceTexture: Failed to get mach port for shared texture");
      return E_FAIL;
    }
    SharedResourceData runtimeData;
    MakeUniqueSharedName(runtimeData.mach_port_name);
    if (!WMTBootstrapRegister(runtimeData.mach_port_name, mach_port)) {
      ERR("DeviceTexture: Failed to register mach port for shared texture");
      return E_FAIL;
    }
    runtimeData.dimension = tag::dimension;
    memcpy(&runtimeData.desc, pDesc, sizeof(typename tag::DESC1));

    Rc<KeyedMutex> keyed_mutex = {};
    if (finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
      keyed_mutex = KeyedMutex::create(pDevice->GetMTLDevice());
    }
    runtimeData.mutex_handle = keyed_mutex ? keyed_mutex->globalHandle() : 0;

    D3DKMT_HANDLE local_kmt, global_kmt;
    if (FAILED(CreateSharedResourceAllocation(
            pDevice, runtimeData, finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE, &local_kmt, &global_kmt
        ))) {
      ERR("DeviceTexture: Failed to create D3DKMT for shared texture");
      return E_FAIL;
    }

    initialize(std::move(allocation));
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), local_kmt,
                                   global_kmt, std::move(keyed_mutex), pDevice))
    );
    return S_OK;
  }

  Flags<TextureAllocationFlag> flags;
  flags.set(finalDesc.CPUAccessFlags ? TextureAllocationFlag::GpuManaged : TextureAllocationFlag::GpuPrivate);
  if (!(finalDesc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
    flags.set(TextureAllocationFlag::ShaderReadonly);
  if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
    flags.set(TextureAllocationFlag::GpuReadonly);
  if (single_subresource && (finalDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
    Rc<RenamableTexturePool> renamable = new RenamableTexturePool(texture.ptr(), 32, flags);
    initialize(renamable->getNext(0));
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), std::move(renamable), pDevice)));
  } else {
    initialize(texture->allocate(flags));
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), pDevice)));
  }
  return S_OK;
}

HRESULT
CreateDeviceTexture1D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE1D_DESC *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture1D **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_1d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDeviceTexture2D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE2D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture2D1 **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_2d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreatePlanarDeviceTexture2D(
    MTLD3D11Device *pDevice, const D3D11_TEXTURE2D_DESC1 *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Texture2D1 **ppTexture
) {
  MTL_DXGI_PLANAR_FORMAT_DESC planar;
  if (!MTLQueryDXGIPlanarFormat(pDesc->Format, planar))
    return E_INVALIDARG;
  if (pDesc->Width == 0 || pDesc->Height == 0 || pDesc->ArraySize == 0)
    return E_INVALIDARG;
  if ((pDesc->Width | pDesc->Height) & 1) {
    ERR("CreatePlanarDeviceTexture2D: width and height of ", pDesc->Format, " must be even");
    return E_INVALIDARG;
  }
  if (pDesc->MipLevels > 1 || (pDesc->MipLevels == 0 && (pDesc->Width > 1 || pDesc->Height > 1)))
    return E_INVALIDARG;
  if (pDesc->SampleDesc.Count != 1)
    return E_INVALIDARG;
  if (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL)
    return E_INVALIDARG;
  if (pDesc->MiscFlags & (D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS))
    return E_INVALIDARG;
  if (pDesc->MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) {
    ERR("CreatePlanarDeviceTexture2D: shared planar textures are not supported");
    return E_INVALIDARG;
  }

  D3D11_TEXTURE2D_DESC1 finalDesc = *pDesc;
  finalDesc.MipLevels = 1;

  Flags<TextureAllocationFlag> flags;
  flags.set(finalDesc.CPUAccessFlags ? TextureAllocationFlag::GpuManaged : TextureAllocationFlag::GpuPrivate);
  if (!(finalDesc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET)))
    flags.set(TextureAllocationFlag::ShaderReadonly);
  if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
    flags.set(TextureAllocationFlag::GpuReadonly);

  auto &initializer = pDevice->GetDXMTDevice().queue().initializer;

  Rc<Texture> planes[2];
  for (unsigned plane = 0; plane < planar.PlaneCount; plane++) {
    auto shift = planar.PlaneSubsampleShift[plane];
    WMTTextureInfo info;
    if (FAILED(CreateMTLTextureDescriptorInternal(
            pDevice, D3D11_RESOURCE_DIMENSION_TEXTURE2D, finalDesc.Width >> shift, finalDesc.Height >> shift, 1,
            finalDesc.ArraySize, 1, finalDesc.BindFlags, finalDesc.CPUAccessFlags, finalDesc.MiscFlags,
            finalDesc.Usage, 1, (DXGI_FORMAT)planar.PlaneFormat[plane], &info
        )))
      return E_INVALIDARG;
    // views may reinterpret the plane (e.g. R8_UINT on the luma plane of NV12)
    WMTTextureUsage usage = info.usage;
    usage |= WMTTextureUsagePixelFormatView;
    info.usage = usage;
    planes[plane] = new Texture(info, pDevice->GetMTLDevice());
    planes[plane]->rename(planes[plane]->allocate(flags));

    for (unsigned slice = 0; slice < finalDesc.ArraySize; slice++) {
      if (!pInitialData) {
        initializer.initWithZero(planes[plane].ptr(), planes[plane]->current(), slice, 0);
        continue;
      }
      auto &data = pInitialData[slice];
      // D3D11 initial data of a planar format: plane 1 follows the rows of plane 0 with the same pitch
      auto plane_data =
          static_cast<const char *>(data.pSysMem) + (plane ? size_t(data.SysMemPitch) * finalDesc.Height : 0);
      initializer.initWithData(
          planes[plane].ptr(), planes[plane]->current(), slice, 0, plane_data, data.SysMemPitch,
          data.SysMemPitch * (finalDesc.Height >> shift), 0
      );
    }
  }

  auto texture = new DeviceTexture<tag_texture_2d>(&finalDesc, std::move(planes[0]), pDevice);
  texture->texture_plane1_ = std::move(planes[1]);
  *ppTexture = reinterpret_cast<ID3D11Texture2D1 *>(ref(texture));
  return S_OK;
}

HRESULT
CreateDeviceTexture3D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE3D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture3D1 **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_3d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

template <typename tag>
HRESULT
ImportSharedTextureInternal(
    MTLD3D11Device *pDevice, const typename tag::DESC1 *pDescUnchecked, mach_port_t MachPort, 
    D3DKMT_HANDLE hSharedKeyedMutex, REFIID riid, void **ppTexture
) {
  WMTTextureInfo info;
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDescUnchecked, &finalDesc, &info)))
    return E_INVALIDARG;

  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));
  auto allocation = texture->import(MachPort);
  if (!allocation)
    return E_FAIL;
  texture->rename(std::move(allocation));

  Rc<KeyedMutex> keyed_mutex;
  if (hSharedKeyedMutex & 0xc0000000)
    keyed_mutex = KeyedMutex::import(pDevice->GetMTLDevice(), hSharedKeyedMutex);

  Com<DeviceTexture<tag>> device_texture = (ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), std::move(keyed_mutex), pDevice)));
  return device_texture->QueryInterface(riid, ppTexture);
}

HRESULT
ImportSharedTexture(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (!(reinterpret_cast<uintptr_t>(hResource) & 0xc0000000)) {
    WARN("ImportSharedTexture: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  struct SharedResourceData runtimeData;

  D3DKMT_QUERYRESOURCEINFO query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hGlobalShare = reinterpret_cast<uintptr_t>(hResource);
  query.pPrivateRuntimeData = &runtimeData;
  query.PrivateRuntimeDataSize = sizeof(runtimeData);

  if (D3DKMTQueryResourceInfo(&query)) {
    WARN("ImportSharedTexture: Failed to query resource: ", hResource);
    return E_INVALIDARG;
  }

  if (query.PrivateRuntimeDataSize != sizeof(runtimeData)) {
    WARN("ImportSharedTexture: Unexpected size: ", query.PrivateRuntimeDataSize);
    return E_INVALIDARG;
  } 

  D3DDDI_OPENALLOCATIONINFO2 alloc = {};
  D3DKMT_OPENRESOURCE open = {};
  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hGlobalShare = reinterpret_cast<uintptr_t>(hResource);
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &alloc;
  open.pPrivateRuntimeData = &runtimeData;
  open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;

  if (D3DKMTOpenResource2(&open)) {
    WARN("ImportSharedTexture: Failed to open resource: ", hResource);
    return E_INVALIDARG;
  }

  D3DKMT_DESTROYALLOCATION destroy = {};
  destroy.hDevice = pDevice->GetLocalD3DKMT();
  destroy.hResource = open.hResource;
  D3DKMTDestroyAllocation(&destroy);

  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(runtimeData.mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
    return E_INVALIDARG;
  }

  switch (runtimeData.dimension)
  {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    return ImportSharedTextureInternal<tag_texture_1d>(
        pDevice, &runtimeData.desc.desc1d, mach_port, runtimeData.mutex_handle, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    return ImportSharedTextureInternal<tag_texture_2d>(
        pDevice, &runtimeData.desc.desc2d, mach_port, runtimeData.mutex_handle, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    return ImportSharedTextureInternal<tag_texture_3d>(
        pDevice, &runtimeData.desc.desc3d, mach_port, runtimeData.mutex_handle, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_BUFFER:
    return ImportSharedBuffer(pDevice, &runtimeData.desc.buffer, mach_port, runtimeData.mutex_handle, riid, ppTexture);
  default:
    ERR("ImportSharedTexture: Unsupported resource dimension");
    return E_INVALIDARG;
  }
}

HRESULT
ImportSharedTextureFromNtHandle(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (reinterpret_cast<uintptr_t>(hResource) & 0xc0000000) {
    WARN("ImportSharedTextureFromNtHandle: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  struct SharedResourceData runtimeData;

  D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hNtHandle = hResource;
  query.pPrivateRuntimeData = &runtimeData;
  query.PrivateRuntimeDataSize = sizeof(runtimeData);

  if (D3DKMTQueryResourceInfoFromNtHandle(&query)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Failed to query resource: ", hResource));
    return E_INVALIDARG;
  }
  
  if (query.PrivateRuntimeDataSize != sizeof(runtimeData)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Unexpected size: ", query.PrivateRuntimeDataSize));
    return E_INVALIDARG;
  }

  D3DDDI_OPENALLOCATIONINFO2 alloc = {};
  D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
  char dummy;

  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hNtHandle = hResource;
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &alloc;
  open.pPrivateRuntimeData = &runtimeData;
  open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
  open.pTotalPrivateDriverDataBuffer = &dummy;
  open.TotalPrivateDriverDataBufferSize = 0;

  if (D3DKMTOpenResourceFromNtHandle(&open)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Failed to open resource: ", hResource));
    return E_INVALIDARG;
  }

  D3DKMT_DESTROYALLOCATION destroy = {};
  destroy.hDevice = pDevice->GetLocalD3DKMT();
  destroy.hResource = open.hResource;
  D3DKMTDestroyAllocation(&destroy);

  if (open.hSyncObject) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Ignoring bundled sync object"));
    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroySync = {};
    destroySync.hSyncObject = open.hSyncObject;
    D3DKMTDestroySynchronizationObject(&destroySync);
  }
  if (open.hKeyedMutex) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Ignoring bundled keyed mutex"));
    D3DKMT_DESTROYKEYEDMUTEX destroyMutex = {};
    destroyMutex.hKeyedMutex = open.hKeyedMutex;
    D3DKMTDestroyKeyedMutex(&destroyMutex);
  }

  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(runtimeData.mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
    return E_INVALIDARG;
  }

  switch (runtimeData.dimension)
  {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    return ImportSharedTextureInternal<tag_texture_1d>(
        pDevice, &runtimeData.desc.desc1d, mach_port, runtimeData.mutex_handle, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    return ImportSharedTextureInternal<tag_texture_2d>(
        pDevice, &runtimeData.desc.desc2d, mach_port, runtimeData.mutex_handle, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    return ImportSharedTextureInternal<tag_texture_3d>(
        pDevice, &runtimeData.desc.desc3d, mach_port, runtimeData.mutex_handle, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_BUFFER:
    return ImportSharedBuffer(pDevice, &runtimeData.desc.buffer, mach_port, runtimeData.mutex_handle, riid, ppTexture);
  default:
    ERR("ImportSharedTexture: Unsupported resource dimension");
    return E_INVALIDARG;
  }
}

HRESULT
ImportSharedTextureByName(
    MTLD3D11Device *pDevice, LPCWSTR lpName, DWORD dwDesiredAccess, REFIID riid, void **ppTexture
) {
  D3DKMT_OPENNTHANDLEFROMNAME openFromName = {};
  openFromName.dwDesiredAccess = dwDesiredAccess;

  OBJECT_ATTRIBUTES attr = {};
  attr.Length = sizeof(attr);

  WCHAR buffer[MAX_PATH];
  UNICODE_STRING name_str;
  DWORD session, len, name_len = wcslen(lpName);

  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
  memcpy(buffer + len, lpName, (name_len + 1) * sizeof(WCHAR));
  name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
  name_str.MaximumLength += sizeof(WCHAR);
  name_str.Buffer = buffer;

  attr.ObjectName = &name_str;
  attr.Attributes = OBJ_CASE_INSENSITIVE;
  openFromName.pObjAttrib = &attr;

  if (D3DKMTOpenNtHandleFromName(&openFromName)) {
    WARN(str::format("ImportSharedTextureByName: Failed to open NT handle from name: ", lpName));
    return E_INVALIDARG;
  }

  HRESULT res = ImportSharedTextureFromNtHandle(pDevice, openFromName.hNtHandle, riid, ppTexture);
  CloseHandle(openFromName.hNtHandle);
  return res;
}

HRESULT
CreateSharedResourceAllocation(
    MTLD3D11Device *pDevice, SharedResourceData &RuntimeData, bool NtHandle, D3DKMT_HANDLE *pLocal,
    D3DKMT_HANDLE *pGlobal
) {
  D3DKMT_CREATEALLOCATION create = {};
  create.hDevice = pDevice->GetLocalD3DKMT();
  create.pPrivateRuntimeData = &RuntimeData;
  create.PrivateRuntimeDataSize = sizeof(RuntimeData);
  create.Flags.StandardAllocation = 1;
  create.NumAllocations = 1;
  D3DDDI_ALLOCATIONINFO2 allocationInfo = {};
  create.pAllocationInfo2 = &allocationInfo;
  D3DKMT_CREATESTANDARDALLOCATION standardAllocation = {};
  create.pStandardAllocation = &standardAllocation;
  standardAllocation.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
  create.Flags.ExistingSysMem = 1;
  D3DDDI_ALLOCATIONINFO systemMem;
  allocationInfo.pSystemMem = &systemMem;
  create.Flags.CreateResource = 1;
  create.Flags.CreateShared = 1;
  create.Flags.NtSecuritySharing = NtHandle;
  if (D3DKMTCreateAllocation2(&create))
    return E_FAIL;
  *pLocal = create.hResource;
  *pGlobal = create.hGlobalShare;
  return S_OK;
}

HRESULT
GetSharedResourceHandle(UINT MiscFlags, D3DKMT_HANDLE GlobalHandle, HANDLE *pSharedHandle) {
  if (pSharedHandle == nullptr || (MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
    return E_INVALIDARG;
  }

  if (!(MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) {
    *pSharedHandle = NULL;
    return S_OK;
  }

  if (!GlobalHandle) {
    return E_INVALIDARG;
  }

  *pSharedHandle = reinterpret_cast<HANDLE>(GlobalHandle);
  return S_OK;
}

HRESULT
CreateSharedResourceNtHandle(
    D3DKMT_HANDLE LocalHandle, const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName,
    HANDLE *pNTHandle
) {
  InitReturnPtr(pNTHandle);
  if (!LocalHandle)
    return E_INVALIDARG;

  OBJECT_ATTRIBUTES attr = {};
  attr.Length = sizeof(attr);
  attr.SecurityDescriptor = const_cast<SECURITY_ATTRIBUTES*>(Attributes);

  WCHAR buffer[MAX_PATH];
  UNICODE_STRING name_str;
  if (pName) {
    DWORD session, len, name_len = wcslen(pName);

    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
    memcpy(buffer + len, pName, (name_len + 1) * sizeof(WCHAR));
    name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
    name_str.MaximumLength += sizeof(WCHAR);
    name_str.Buffer = buffer;

    attr.ObjectName = &name_str;
    attr.Attributes = OBJ_CASE_INSENSITIVE;
  }

  if (D3DKMTShareObjects(1, &LocalHandle, &attr, Access, pNTHandle)) {
    ERR("CreateSharedHandle: Failed to create shared handle");
    return E_FAIL;
  }

  return S_OK;
}

#pragma endregion

} // namespace dxmt