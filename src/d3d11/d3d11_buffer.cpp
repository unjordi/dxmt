/*
 * Copyright 2026 Feifan He for CodeWeavers
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
#include "dxmt_buffer.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_format.hpp"
#include "d3d11_resource.hpp"

namespace dxmt {

struct BufferViewInfo {
  BufferViewKey viewKey;
  uint32_t viewElementOffset;
  uint32_t viewElementWidth;
  uint32_t byteOffset;
  uint32_t byteWidth;
};

class D3D11Buffer : public TResourceBase<tag_buffer> {
private:
#ifdef DXMT_DEBUG
  std::string debug_name;
#endif
  bool structured;
  bool allow_raw_view;
  D3DKMT_HANDLE local_kmt_ = 0;
  D3DKMT_HANDLE global_kmt_ = 0;

  Rc<DynamicBuffer> dynamic_;

  using SRVBase = TResourceViewBase<tag_shader_resource_view<D3D11Buffer>>;

  class TBufferSRV : public SRVBase {

  public:
    TBufferSRV(
        const tag_shader_resource_view<>::DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
        BufferViewInfo const &info
    ) :
        SRVBase(pDesc, pResource, pDevice) {
      buffer_ = pResource->buffer_.ptr();
      view_id_ = info.viewKey;
      slice_ = {info.byteOffset, info.byteWidth, info.viewElementOffset, info.viewElementWidth };
      subset_ = ResourceSubsetState(info.byteOffset, info.byteWidth);
    }

    ~TBufferSRV() {}
  };

  using UAVBase = TResourceViewBase<tag_unordered_access_view<D3D11Buffer>>;

  class UAVWithCounter : public UAVBase {
  public:
    UAVWithCounter(
        const tag_unordered_access_view<>::DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
        BufferViewInfo const &info, Rc<Counter>&& counter
    ) :
        UAVBase(pDesc, pResource, pDevice) {
      buffer_ = pResource->buffer_.ptr();
      view_id_ = info.viewKey;
      slice_ = {info.byteOffset, info.byteWidth, info.viewElementOffset, info.viewElementWidth };
      counter_ = std::move(counter);
      subset_ = ResourceSubsetState(info.byteOffset, info.byteWidth);
    }
  };

public:
  D3D11Buffer(const tag_buffer::DESC1 *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, MTLD3D11Device *device) :
      TResourceBase<tag_buffer>(*pDesc, device) {
    buffer_ = new Buffer(pDesc->ByteWidth, device->GetMTLDevice());
    Flags<BufferAllocationFlag> flags;
    if (!m_parent->IsTraced() && pDesc->Usage == D3D11_USAGE_DYNAMIC)
      flags.set(BufferAllocationFlag::CpuWriteCombined);
    // if (pDesc->Usage != D3D11_USAGE_DEFAULT)
    if (pDesc->Usage == D3D11_USAGE_IMMUTABLE)
      flags.set(BufferAllocationFlag::GpuReadonly);
    if (pDesc->Usage != D3D11_USAGE_DYNAMIC) {
      if (pInitialData)
        flags.set(BufferAllocationFlag::GpuManaged);
      else if (pDesc->BindFlags & kD3D11OutputBindFlags)
        flags.set(BufferAllocationFlag::GpuPrivate);
      else
        flags.set(BufferAllocationFlag::GpuManaged);
    } else {
      flags.set(BufferAllocationFlag::SuballocateFromOnePage);
#ifdef __i386__
      flags.set(BufferAllocationFlag::CpuPlaced);
#endif
    }
    auto allocation = buffer_->allocate(flags);
    auto &initializer = device->GetDXMTDevice().queue().initializer;
    if (pInitialData) {
      allocation->updateContents(0, pInitialData->pSysMem, pDesc->ByteWidth);
    } else if (flags.test(BufferAllocationFlag::GpuPrivate)) {
      initializer.initWithZero(allocation.ptr(), 0, buffer_->length());
    }
    auto _ = buffer_->rename(std::move(allocation));
    D3D11_ASSERT(_.ptr() == nullptr);
    structured = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    allow_raw_view = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (!(desc.BindFlags & kD3D11OutputBindFlags)) {
      dynamic_ = new DynamicBuffer(buffer_.ptr(), flags);
    }
  }

  /**
   * Wraps a buffer whose current allocation is shared with other processes. The allocation is never renamed:
   * a DYNAMIC buffer gets a pinned DynamicBuffer, a DEFAULT buffer is updated by GPU copies only.
   */
  D3D11Buffer(
      const tag_buffer::DESC1 *pDesc, Rc<Buffer> &&buffer, Flags<BufferAllocationFlag> flags, D3DKMT_HANDLE localHandle,
      D3DKMT_HANDLE globalHandle, Rc<KeyedMutex> &&keyed_mutex, MTLD3D11Device *device
  ) :
      TResourceBase<tag_buffer>(*pDesc, device),
      local_kmt_(localHandle),
      global_kmt_(globalHandle) {
    buffer_ = std::move(buffer);
    keyed_mutex_ = std::move(keyed_mutex);
    structured = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    allow_raw_view = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (pDesc->Usage == D3D11_USAGE_DYNAMIC) {
      dynamic_ = new DynamicBuffer(buffer_.ptr(), flags, true);
    }
  }

  ~D3D11Buffer() {
    if (local_kmt_) {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = m_parent->GetLocalD3DKMT();
      destroy.hResource = local_kmt_;
      D3DKMTDestroyAllocation(&destroy);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    return TResourceBase<tag_buffer>::QueryInterface(riid, ppvObject);
  }

  HRESULT
  GetSharedHandle(HANDLE *pSharedHandle) override {
    return GetSharedResourceHandle(desc.MiscFlags, global_kmt_, pSharedHandle);
  }

  HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    return CreateSharedResourceNtHandle(local_kmt_, Attributes, Access, pName, pNTHandle);
  }

  Rc<StagingResource>
  staging(UINT Subresource) final {
    return nullptr;
  }
  Rc<DynamicBuffer>
  dynamicBuffer(UINT *pBufferLength, UINT *pBindFlags) override {
    *pBufferLength = desc.ByteWidth;
    *pBindFlags = desc.BindFlags;
    return dynamic_;
  }
  Rc<DynamicLinearTexture>
  dynamicLinearTexture(UINT *, UINT *) final {
    return {};
  };
  Rc<DynamicBuffer>
  dynamicTexture(UINT , UINT *, UINT *) final {
    return {};
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc, ID3D11ShaderResourceView1 **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc, &finalDesc))) {
      return E_INVALIDARG;
    }
    if (finalDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER &&
        finalDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX) {
      return E_FAIL;
    }
    WMTPixelFormat view_format = WMTPixelFormatInvalid;
    uint32_t offset, size, viewElementOffset, viewElementWidth;
    if (structured) {
      if (finalDesc.Format != DXGI_FORMAT_UNKNOWN) {
        return E_INVALIDARG;
      }
      // StructuredBuffer
      CalculateBufferViewOffsetAndSize(
          this->desc, desc.StructureByteStride, finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset,
          size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement * (desc.StructureByteStride >> 2);
      viewElementWidth = finalDesc.Buffer.NumElements * (desc.StructureByteStride >> 2);
    } else if (finalDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX && finalDesc.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) {
      if (!allow_raw_view)
        return E_INVALIDARG;
      if (finalDesc.Format != DXGI_FORMAT_R32_TYPELESS)
        return E_INVALIDARG;
      CalculateBufferViewOffsetAndSize(
          this->desc, sizeof(uint32_t), finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset, size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    } else {
      MTL_DXGI_FORMAT_DESC format;
      if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format))) {
        return E_FAIL;
      }
      if (!format.BytesPerTexel) {
        ERR("D3D11Buffer::CreateShaderResourceView: not an ordinary or packed format: ", finalDesc.Format);
        return E_FAIL;
      }

      view_format = format.PixelFormat;
      offset = finalDesc.Buffer.FirstElement * format.BytesPerTexel;
      size = finalDesc.Buffer.NumElements * format.BytesPerTexel;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    }

    if (!ppView) {
      return S_FALSE;
    }

    auto viewId = buffer_->createView({.format = view_format});

    auto srv = ref(new TBufferSRV(
        &finalDesc, this, m_parent,
        {
            .viewKey = viewId,
            .viewElementOffset = viewElementOffset,
            .viewElementWidth = viewElementWidth,
            .byteOffset = offset,
            .byteWidth = size,
        }
    ));
    *ppView = srv;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc, ID3D11UnorderedAccessView1 **ppView)
      override {
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc, &finalDesc))) {
      return E_INVALIDARG;
    }
    if (finalDesc.ViewDimension != D3D11_UAV_DIMENSION_BUFFER) {
      return E_FAIL;
    }
    WMTPixelFormat view_format = WMTPixelFormatInvalid;
    uint32_t offset, size, viewElementOffset, viewElementWidth;
    Rc<Counter> counter = {};
    if (structured) {
      if (finalDesc.Format != DXGI_FORMAT_UNKNOWN) {
        return E_INVALIDARG;
      }
      // StructuredBuffer
      CalculateBufferViewOffsetAndSize(
          this->desc, desc.StructureByteStride, finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset,
          size
      );
      view_format = WMTPixelFormatR32Uint;
      // when structured buffer is interpreted as typed buffer for any reason
      viewElementOffset = finalDesc.Buffer.FirstElement * (desc.StructureByteStride >> 2);
      viewElementWidth = finalDesc.Buffer.NumElements * (desc.StructureByteStride >> 2);
      if (finalDesc.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) {
        counter = new Counter();
      }
    } else if (finalDesc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW) {
      if (!allow_raw_view)
        return E_INVALIDARG;
      if (finalDesc.Format != DXGI_FORMAT_R32_TYPELESS)
        return E_INVALIDARG;
      CalculateBufferViewOffsetAndSize(
          this->desc, sizeof(uint32_t), finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset, size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    } else {
      MTL_DXGI_FORMAT_DESC format;
      if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format))) {
        return E_FAIL;
      }
      if (!format.BytesPerTexel) {
        ERR("D3D11Buffer::CreateUnorderedAccessView: not an ordinary or packed format: ", finalDesc.Format);
        return E_FAIL;
      }

      view_format = format.PixelFormat;
      offset = finalDesc.Buffer.FirstElement * format.BytesPerTexel;
      size = finalDesc.Buffer.NumElements * format.BytesPerTexel;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    }

    if (!ppView) {
      return S_FALSE;
    }

    auto viewId = buffer_->createView({.format = view_format});

    auto srv = ref(new UAVWithCounter(
        &finalDesc, this, m_parent,
        {
            .viewKey = viewId,
            .viewElementOffset = viewElementOffset,
            .viewElementWidth = viewElementWidth,
            .byteOffset = offset,
            .byteWidth = size,
        },
        std::move(counter)
    ));
    *ppView = srv;
    return S_OK;
  };

  void
  OnSetDebugObjectName(LPCSTR Name) override {
    if (!Name) {
      return;
    }
#ifdef DXMT_DEBUG
    debug_name = std::string(Name);
#endif
  }
};

constexpr UINT kSharedResourceMiscFlags =
    D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

static Flags<BufferAllocationFlag>
SharedBufferAllocationFlags(const D3D11_BUFFER_DESC *pDesc) {
  Flags<BufferAllocationFlag> flags;
  if (pDesc->Usage == D3D11_USAGE_IMMUTABLE)
    flags.set(BufferAllocationFlag::GpuReadonly);
  return flags;
}

static HRESULT
ValidateSharedBufferDesc(const D3D11_BUFFER_DESC *pDesc) {
  if (!(pDesc->MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) {
    ERR("CreateBuffer: SHARED_NTHANDLE requires SHARED or SHARED_KEYEDMUTEX");
    return E_INVALIDARG;
  }
  if ((pDesc->MiscFlags & D3D11_RESOURCE_MISC_SHARED) && (pDesc->MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) {
    ERR("CreateBuffer: SHARED and SHARED_KEYEDMUTEX are mutually exclusive");
    return E_INVALIDARG;
  }
  if (pDesc->Usage != D3D11_USAGE_DEFAULT && pDesc->Usage != D3D11_USAGE_DYNAMIC &&
      pDesc->Usage != D3D11_USAGE_IMMUTABLE) {
    ERR("CreateBuffer: shared buffer with usage ", pDesc->Usage, " is not supported");
    return E_INVALIDARG;
  }
  return S_OK;
}

static HRESULT
CreateSharedBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Buffer **ppBuffer
) {
  if (HRESULT hr = ValidateSharedBufferDesc(pDesc); FAILED(hr))
    return hr;
  if (!(pDevice->GetLocalD3DKMT() & 0xc0000000)) {
    ERR("CreateBuffer: Invalid device handle, can't create shared buffer");
    return E_FAIL;
  }
  if (!ppBuffer)
    return S_FALSE;

  auto flags = SharedBufferAllocationFlags(pDesc);
  Rc<Buffer> buffer = new Buffer(pDesc->ByteWidth, pDevice->GetMTLDevice());
  mach_port_t mach_port = 0;
  auto allocation = buffer->allocateShared(flags, mach_port);
  if (!allocation) {
    ERR("CreateBuffer: Failed to allocate shared memory for a ", pDesc->ByteWidth, " bytes shared buffer");
    return E_OUTOFMEMORY;
  }
  if (pDesc->Usage == D3D11_USAGE_DYNAMIC && !allocation->mappedMemory(0)) {
    ERR("CreateBuffer: shared dynamic buffer is not addressable by this process");
    return E_INVALIDARG;
  }
  if (pInitialData)
    allocation->updateContents(0, pInitialData->pSysMem, pDesc->ByteWidth);

  SharedResourceData runtimeData = {};
  MakeUniqueSharedName(runtimeData.mach_port_name);
  if (!WMTBootstrapRegister(runtimeData.mach_port_name, mach_port)) {
    ERR("CreateBuffer: Failed to register mach port for shared buffer");
    return E_FAIL;
  }
  runtimeData.dimension = D3D11_RESOURCE_DIMENSION_BUFFER;
  runtimeData.desc.buffer = *pDesc;

  Rc<KeyedMutex> keyed_mutex = {};
  if (pDesc->MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
    keyed_mutex = KeyedMutex::create(pDevice->GetMTLDevice());
  runtimeData.mutex_handle = keyed_mutex ? keyed_mutex->globalHandle() : 0;

  D3DKMT_HANDLE local_kmt, global_kmt;
  if (FAILED(CreateSharedResourceAllocation(
          pDevice, runtimeData, pDesc->MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE, &local_kmt, &global_kmt
      ))) {
    ERR("CreateBuffer: Failed to create D3DKMT for shared buffer");
    return E_FAIL;
  }

  auto _ = buffer->rename(std::move(allocation));
  *ppBuffer = reinterpret_cast<ID3D11Buffer *>(ref(new D3D11Buffer(
      pDesc, std::move(buffer), flags, local_kmt, global_kmt, std::move(keyed_mutex), pDevice
  )));
  return S_OK;
}

HRESULT
ImportSharedBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc, mach_port_t MachPort, D3DKMT_HANDLE hSharedKeyedMutex,
    REFIID riid, void **ppBuffer
) {
  if (pDesc->ByteWidth == 0 || FAILED(ValidateSharedBufferDesc(pDesc))) {
    ERR("ImportSharedBuffer: Invalid shared buffer description");
    return E_INVALIDARG;
  }

  auto flags = SharedBufferAllocationFlags(pDesc);
  Rc<Buffer> buffer = new Buffer(pDesc->ByteWidth, pDevice->GetMTLDevice());
  auto allocation = buffer->importShared(flags, MachPort);
  if (!allocation) {
    ERR("ImportSharedBuffer: Failed to map shared buffer memory");
    return E_FAIL;
  }
  if (pDesc->Usage == D3D11_USAGE_DYNAMIC && !allocation->mappedMemory(0)) {
    ERR("ImportSharedBuffer: shared dynamic buffer is not addressable by this process");
    return E_INVALIDARG;
  }
  auto _ = buffer->rename(std::move(allocation));

  Rc<KeyedMutex> keyed_mutex;
  if (hSharedKeyedMutex & 0xc0000000)
    keyed_mutex = KeyedMutex::import(pDevice->GetMTLDevice(), hSharedKeyedMutex);

  Com<D3D11Buffer> shared_buffer =
      ref(new D3D11Buffer(pDesc, std::move(buffer), flags, 0, 0, std::move(keyed_mutex), pDevice));
  return shared_buffer->QueryInterface(riid, ppBuffer);
}

HRESULT
CreateBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Buffer **ppBuffer
) {
  if (pDesc->MiscFlags & kSharedResourceMiscFlags)
    return CreateSharedBuffer(pDevice, pDesc, pInitialData, ppBuffer);
  *ppBuffer = reinterpret_cast<ID3D11Buffer *>(ref(new D3D11Buffer(pDesc, pInitialData, pDevice)));
  return S_OK;
}

} // namespace dxmt
