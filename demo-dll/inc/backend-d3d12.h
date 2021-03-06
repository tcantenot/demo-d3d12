#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <DirectXTex.h>
#include <winrt/base.h>
#include <pix3.h>
#include <DXProgrammableCapture.h>
#include <vector>
#include <string>
#include <functional>
#include <optional>
#include <concurrent_vector.h>

// Aliased types
using DXGIFactory_t = IDXGIFactory4;
using DXGIAdapter_t = IDXGIAdapter;
using DXGISwapChain_t = IDXGISwapChain3;
using D3DDebug_t = ID3D12Debug;
using D3DDevice_t = ID3D12Device5;
using D3DCommandQueue_t = ID3D12CommandQueue;
using D3DCommandAllocator_t = ID3D12CommandAllocator;
using D3DDescriptorHeap_t = ID3D12DescriptorHeap;
using D3DHeap_t = ID3D12Heap;
using D3DResource_t = ID3D12Resource;
using D3DCommandList_t = ID3D12GraphicsCommandList4;
using D3DFence_t = ID3D12Fence1;
using D3DPipelineState_t = ID3D12PipelineState;
using D3DRootSignature_t = ID3D12RootSignature;
using PhysicalAlloc_t = std::vector<uint32_t>;

struct IDxcBlob;
struct FResource;

enum class BindlessResourceType
{
	Buffer,
	Texture2D,
	TextureCube,
	RWTexture2D,
	RWTexture2DArray,
	Count
};

enum class BindlessDescriptorType
{
	Buffer,
	Texture2D,
	TextureCube,
	RWTexture2D,
	RWTexture2DArray
};

enum class BindlessDescriptorRange : uint32_t
{
	BufferBegin,
	BufferEnd = BufferBegin + 999,
	Texture2DBegin,
	Texture2DEnd = Texture2DBegin + 999,
	TextureCubeBegin,
	TextureCubeEnd = TextureCubeBegin + 999,
	RWTexture2DBegin,
	RWTexture2DEnd = RWTexture2DBegin + 999,
	RWTexture2DArrayBegin,
	RWTexture2DArrayEnd = RWTexture2DArrayBegin + 999,
	TotalCount
};

struct FCommandList
{
	D3D12_COMMAND_LIST_TYPE m_type;
	std::wstring m_name;
	size_t m_fenceValue;
	winrt::com_ptr<D3DCommandList_t> m_d3dCmdList;
	winrt::com_ptr<D3DCommandAllocator_t> m_cmdAllocator;
	winrt::com_ptr<D3DFence_t> m_fence;
	std::unordered_map<FResource*, std::function<void(void)>> m_pendingTransitions;
	std::vector<std::function<void(void)>> m_postExecuteCallbacks;

	FCommandList() = default;
	FCommandList(const D3D12_COMMAND_LIST_TYPE type, const size_t  fenceValue);
	void SetName(const std::wstring& name);
};

struct FShaderDesc
{
	std::wstring m_filename;
	std::wstring m_entrypoint;
	std::wstring m_defines;
};

struct FRootsigDesc
{
	std::wstring m_filename;
	std::wstring m_entrypoint;
};

struct FResource
{
	D3DResource_t* m_d3dResource;
	std::wstring m_name;
	concurrency::concurrent_vector<D3D12_RESOURCE_STATES> m_subresourceStates;

	~FResource();
	void SetName(const std::wstring& name);
	HRESULT InitCommittedResource(const std::wstring& name, const D3D12_HEAP_PROPERTIES& heapProperties, const D3D12_RESOURCE_DESC& resourceDesc, const D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue = nullptr);
	HRESULT InitReservedResource(const std::wstring& name, const D3D12_RESOURCE_DESC& resourceDesc, const D3D12_RESOURCE_STATES initialState);
	void Transition(FCommandList* cmdList, const uint32_t subresourceIndex, const D3D12_RESOURCE_STATES destState);
	void UavBarrier(FCommandList* cmdList);
};

struct FBindlessShaderResource
{
	FResource* m_resource;
	uint32_t m_srvIndex = ~0u;

	~FBindlessShaderResource();
};

struct FBindlessUav
{
	FResource* m_resource;
	std::vector<uint32_t> m_uavIndices; // one for each mip level
	D3D12_SHADER_RESOURCE_VIEW_DESC m_srvDesc;
	uint32_t m_srvIndex = ~0u;

	~FBindlessUav();
	void Transition(FCommandList* cmdList, const uint32_t subresourceIndex, const D3D12_RESOURCE_STATES destState);
};

struct FTransientBuffer
{
	FResource* m_resource;
	const FCommandList* m_dependentCmdlist;

	~FTransientBuffer();
};

struct FRenderTexture
{
	FResource* m_resource;
	std::vector<uint32_t> m_renderTextureIndices; // one for each mip level
	D3D12_SHADER_RESOURCE_VIEW_DESC m_srvDesc;
	uint32_t m_srvIndex = ~0u;
	bool m_isDepthStencil;
	bool m_isSwapChainBuffer;

	~FRenderTexture();
	void Transition(FCommandList* cmdList, const uint32_t subresourceIndex, const D3D12_RESOURCE_STATES destState);
};

class FResourceUploadContext
{
public:
	FResourceUploadContext() = delete;
	explicit FResourceUploadContext(const size_t uploadBufferSizeInBytes);

	void UpdateSubresources(
		D3DResource_t* destinationResource,
		const std::vector<D3D12_SUBRESOURCE_DATA>& srcData,
		std::function<void(FCommandList*)> transition);

	D3DFence_t* SubmitUploads(FCommandList* owningCL);

private:
	FResource* m_uploadBuffer;
	FCommandList* m_copyCommandlist;
	uint8_t* m_mappedPtr;
	size_t m_sizeInBytes;
	size_t m_currentOffset;
	std::vector<std::function<void(FCommandList*)>> m_pendingTransitions;
};

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														RenderBackend12
//-----------------------------------------------------------------------------------------------------------------------------------------------

namespace RenderBackend12
{
	bool Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY);
	void Teardown();
	void FlushGPU();

	// Command Lists
	FCommandList* FetchCommandlist(const D3D12_COMMAND_LIST_TYPE type);
	D3DFence_t* ExecuteCommandlists(const D3D12_COMMAND_LIST_TYPE commandQueueType, std::initializer_list<FCommandList*> commandLists);

	// Root Signatures
	winrt::com_ptr<D3DRootSignature_t> FetchRootSignature(const FRootsigDesc& rootsig);

	// Pipeline States
	D3DPipelineState_t* FetchGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
	D3DPipelineState_t* FetchComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC&  desc);

	// Swap chain and back buffers
	FRenderTexture* GetBackBuffer();
	void PresentDisplay();

	// Shaders
	IDxcBlob* CacheShader(const FShaderDesc& shaderDesc, const std::wstring& profile);
	IDxcBlob* CacheRootsignature(const FRootsigDesc& rootsigDesc, const std::wstring& profile);

	// Descriptor Management
	D3DDescriptorHeap_t* GetBindlessShaderResourceHeap();
	D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapType, uint32_t descriptorIndex);
	D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapType, uint32_t descriptorIndex);
	uint32_t GetDescriptorTableOffset(BindlessDescriptorType descriptorType, uint32_t descriptorIndex);

	// Feature Support
	uint32_t GetLaneCount();

	// Resource Management
	std::unique_ptr<FTransientBuffer> CreateTransientBuffer(
		const std::wstring& name,
		const size_t size,
		const FCommandList* dependentCL,
		std::function<void(uint8_t*)> uploadFunc = nullptr);

	std::unique_ptr<FRenderTexture> CreateRenderTexture(
		const std::wstring& name,
		const DXGI_FORMAT format,
		const size_t width,
		const size_t height,
		const size_t mipLevels,
		const size_t depth,
		const size_t sampleCount);

	std::unique_ptr<FRenderTexture> CreateDepthStencilTexture(
		const std::wstring& name,
		const DXGI_FORMAT format,
		const size_t width,
		const size_t height,
		const size_t mipLevels,
		const size_t sampleCount);

	std::unique_ptr<FBindlessShaderResource> CreateBindlessTexture(
		const std::wstring& name, 
		const BindlessResourceType type,
		const DXGI_FORMAT format,
		const size_t width,
		const size_t height,
		const size_t numMips,
		const size_t numSlices,
		D3D12_RESOURCE_STATES resourceState,
		const DirectX::Image* images = nullptr,
		FResourceUploadContext* uploadContext = nullptr);

	std::unique_ptr<FBindlessShaderResource> CreateBindlessBuffer(
		const std::wstring& name,
		const size_t size,
		D3D12_RESOURCE_STATES resourceState,
		const uint8_t* pData = nullptr,
		FResourceUploadContext* uploadContext = nullptr);

	std::unique_ptr<FBindlessUav> CreateBindlessUavTexture(
		const std::wstring& name,
		const DXGI_FORMAT format,
		const size_t width,
		const size_t height,
		const size_t mipLevels,
		const size_t arraySize);

	std::unique_ptr<FBindlessUav> CreateBindlessUavBuffer(
		const std::wstring& name,
		const size_t size);

	// Programmatic Captures
	void BeginCapture();
	void EndCapture();
}