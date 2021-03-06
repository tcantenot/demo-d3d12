#include <demo.h>
#include <backend-d3d12.h>
#include <profiling.h>
#include <common.h>
#include <renderer.h>
#include <ppltasks.h>
#include <sstream>
#include <imgui.h>
#include <dxcapi.h>
#include <microprofile.h>

namespace RenderJob
{
	struct BasePassDesc
	{
		FRenderTexture* colorTarget;
		FRenderTexture* depthStencilTarget;
		uint32_t resX;
		uint32_t resY;
		uint32_t sampleCount;
		const FScene* scene;
		const FView* view;
	};

	struct PostprocessPassDesc
	{
		FRenderTexture* colorSource;
		FRenderTexture* colorTarget;
		uint32_t resX;
		uint32_t resY;
		const FView* view;
	};

	concurrency::task<FCommandList*> BasePass(const BasePassDesc& passDesc)
	{
		return concurrency::create_task([passDesc]
		{
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"base_pass_job");

			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			SCOPED_GPU_EVENT(cmdList, L"base_pass", 0);

			passDesc.colorTarget->Transition(cmdList, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.depthStencilTarget->Transition(cmdList, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"base-pass.hlsl", L"rootsig" });
			d3dCmdList->SetGraphicsRootSignature(rootsig.get());

			// Frame constant buffer
			struct FrameCbLayout
			{
				Matrix sceneRotation;
				uint32_t sceneIndexBufferBindlessIndex;
				uint32_t scenePositionBufferBindlessIndex;
				uint32_t sceneNormalBufferBindlessIndex;
				uint32_t sceneUvBufferBindlessIndex;
				FLightProbe sceneProbeData;
			};

			std::unique_ptr<FTransientBuffer> frameCb = RenderBackend12::CreateTransientBuffer(
				L"frame_cb",
				sizeof(FrameCbLayout),
				cmdList,
				[scene = passDesc.scene](uint8_t* pDest)
				{
					auto cbDest = reinterpret_cast<FrameCbLayout*>(pDest);
					cbDest->sceneRotation = scene->m_rootTransform;
					cbDest->sceneIndexBufferBindlessIndex = scene->m_meshIndexBuffer->m_srvIndex;
					cbDest->scenePositionBufferBindlessIndex = scene->m_meshPositionBuffer->m_srvIndex;
					cbDest->sceneNormalBufferBindlessIndex = scene->m_meshNormalBuffer->m_srvIndex;
					cbDest->sceneUvBufferBindlessIndex = scene->m_meshUvBuffer->m_srvIndex;
					cbDest->sceneProbeData = scene->m_globalLightProbe;
				});

			d3dCmdList->SetGraphicsRootConstantBufferView(3, frameCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// View constant buffer
			struct ViewCbLayout
			{
				Matrix viewTransform;
				Matrix projectionTransform;
			};

			std::unique_ptr<FTransientBuffer> viewCb = RenderBackend12::CreateTransientBuffer(
				L"view_cb",
				sizeof(ViewCbLayout),
				cmdList,
				[view = passDesc.view](uint8_t* pDest)
				{
					auto cbDest = reinterpret_cast<ViewCbLayout*>(pDest);
					cbDest->viewTransform = view->m_viewTransform;
					cbDest->projectionTransform = view->m_projectionTransform;
				});

			d3dCmdList->SetGraphicsRootConstantBufferView(2, viewCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetBindlessShaderResourceHeap() };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);
			d3dCmdList->SetGraphicsRootDescriptorTable(4, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::Texture2DBegin));
			d3dCmdList->SetGraphicsRootDescriptorTable(5, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::BufferBegin));
			d3dCmdList->SetGraphicsRootDescriptorTable(6, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::TextureCubeBegin));

			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.pRootSignature = rootsig.get();
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = Settings::k_backBufferFormat;
			psoDesc.SampleDesc.Count = passDesc.sampleCount;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"base-pass.hlsl", L"vs_main", L"" }, L"vs_6_4");
				IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"base-pass.hlsl", L"ps_main", L"" }, L"ps_6_4");

				vs.pShaderBytecode = vsBlob->GetBufferPointer();
				vs.BytecodeLength = vsBlob->GetBufferSize();
				ps.pShaderBytecode = psBlob->GetBufferPointer();
				ps.BytecodeLength = psBlob->GetBufferSize();
			}

			// PSO - Rasterizer State
			{
				D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
				desc.FillMode = D3D12_FILL_MODE_SOLID;
				desc.CullMode = D3D12_CULL_MODE_BACK;
				desc.FrontCounterClockwise = TRUE;
				desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
				desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
				desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
				desc.DepthClipEnable = TRUE;
				desc.MultisampleEnable = FALSE;
				desc.AntialiasedLineEnable = FALSE;
				desc.ForcedSampleCount = 0;
				desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
			}

			// PSO - Blend State
			{
				D3D12_BLEND_DESC& desc = psoDesc.BlendState;
				desc.AlphaToCoverageEnable = FALSE;
				desc.IndependentBlendEnable = FALSE;
				desc.RenderTarget[0].BlendEnable = FALSE;
				desc.RenderTarget[0].LogicOpEnable = FALSE;
				desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			}

			// PSO - Depth Stencil State
			{
				D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
				desc.DepthEnable = TRUE;
				desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
				desc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
				desc.StencilEnable = FALSE;
			}

			D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			D3D12_VIEWPORT viewport{ 0.f, 0.f, (float)passDesc.resX, (float)passDesc.resY, 0.f, 1.f };
			D3D12_RECT screenRect{ 0, 0, (LONG)passDesc.resX, (LONG)passDesc.resY };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.colorTarget->m_renderTextureIndices[0]) };
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, passDesc.depthStencilTarget->m_renderTextureIndices[0]);
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, &dsv);

			float clearColor[] = { .8f, .8f, 1.f, 0.f };
			d3dCmdList->ClearRenderTargetView(rtvs[0], clearColor, 0, nullptr);
			d3dCmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.f, 0, 0, nullptr);

			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			
			// Issue scene draws
			for (int meshIndex = 0; meshIndex < passDesc.scene->m_meshGeo.size(); ++meshIndex)
			{
				const FRenderMesh& mesh = passDesc.scene->m_meshGeo[meshIndex];

				// Geometry constants
				struct MeshCbLayout
				{
					Matrix localToWorldTransform;
					uint32_t indexOffset;
					uint32_t positionOffset;
					uint32_t normalOffset;
					uint32_t uvOffset;
				} meshCb =
				{
					passDesc.scene->m_meshTransforms[meshIndex],
					passDesc.scene->m_meshGeo[meshIndex].m_indexOffset,
					passDesc.scene->m_meshGeo[meshIndex].m_positionOffset,
					passDesc.scene->m_meshGeo[meshIndex].m_normalOffset,
					passDesc.scene->m_meshGeo[meshIndex].m_uvOffset
				};	

				d3dCmdList->SetGraphicsRoot32BitConstants(0, sizeof(MeshCbLayout)/4, &meshCb, 0);

				// Material constants
				struct MaterialCbLayout
				{
					Vector3 emissiveFactor;
					float metallicFactor;
					Vector3 baseColorFactor;
					float roughnessFactor;
					int baseColorTextureIndex;
					int metallicRoughnessTextureIndex;
					int normalTextureIndex;
					int baseColorSamplerIndex;
					int metallicRoughnessSamplerIndex;
					int normalSamplerIndex;
				};

				std::unique_ptr<FTransientBuffer> materialCb = RenderBackend12::CreateTransientBuffer(
					L"material_cb",
					sizeof(MaterialCbLayout),
					cmdList,
					[&mesh](uint8_t* pDest)
					{
						auto cbDest = reinterpret_cast<MaterialCbLayout*>(pDest);
						cbDest->emissiveFactor = mesh.m_emissiveFactor;
						cbDest->metallicFactor = mesh.m_metallicFactor;
						cbDest->baseColorFactor = mesh.m_baseColorFactor;
						cbDest->roughnessFactor = mesh.m_roughnessFactor;
						cbDest->baseColorTextureIndex = mesh.m_baseColorTextureIndex;
						cbDest->metallicRoughnessTextureIndex = mesh.m_metallicRoughnessTextureIndex;
						cbDest->normalTextureIndex = mesh.m_normalTextureIndex;
						cbDest->baseColorSamplerIndex = mesh.m_baseColorSamplerIndex;
						cbDest->metallicRoughnessSamplerIndex = mesh.m_metallicRoughnessSamplerIndex;
						cbDest->normalSamplerIndex = mesh.m_normalSamplerIndex;
					});

				d3dCmdList->SetGraphicsRootConstantBufferView(1, materialCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

				d3dCmdList->DrawInstanced(mesh.m_indexCount, 1, 0, 0);
			}
	
			return cmdList;
		});
	}

	concurrency::task<FCommandList*> BackgroundPass(const BasePassDesc& passDesc)
	{
		return concurrency::create_task([passDesc]
		{
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"background_pass_job");

			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			SCOPED_GPU_EVENT(cmdList, L"background_pass", 0);

			passDesc.colorTarget->Transition(cmdList, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.depthStencilTarget->Transition(cmdList, 0, D3D12_RESOURCE_STATE_DEPTH_READ);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"cubemap-bg.hlsl", L"rootsig" });
			d3dCmdList->SetGraphicsRootSignature(rootsig.get());

			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetBindlessShaderResourceHeap() };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);
			d3dCmdList->SetGraphicsRootDescriptorTable(1, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::TextureCubeBegin));

			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.pRootSignature = rootsig.get();
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = Settings::k_backBufferFormat;
			psoDesc.SampleDesc.Count = passDesc.sampleCount;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"cubemap-bg.hlsl", L"vs_main", L"" }, L"vs_6_4");
				IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"cubemap-bg.hlsl", L"ps_main", L"" }, L"ps_6_4");

				vs.pShaderBytecode = vsBlob->GetBufferPointer();
				vs.BytecodeLength = vsBlob->GetBufferSize();
				ps.pShaderBytecode = psBlob->GetBufferPointer();
				ps.BytecodeLength = psBlob->GetBufferSize();
			}

			// PSO - Rasterizer State
			{
				D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
				desc.FillMode = D3D12_FILL_MODE_SOLID;
				desc.CullMode = D3D12_CULL_MODE_BACK;
				desc.FrontCounterClockwise = FALSE;
				desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
				desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
				desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
				desc.DepthClipEnable = TRUE;
				desc.MultisampleEnable = FALSE;
				desc.AntialiasedLineEnable = FALSE;
				desc.ForcedSampleCount = 0;
				desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
			}

			// PSO - Blend State
			{
				D3D12_BLEND_DESC& desc = psoDesc.BlendState;
				desc.AlphaToCoverageEnable = FALSE;
				desc.IndependentBlendEnable = FALSE;
				desc.RenderTarget[0].BlendEnable = FALSE;
				desc.RenderTarget[0].LogicOpEnable = FALSE;
				desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			}

			// PSO - Depth Stencil State
			{
				D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
				desc.DepthEnable = TRUE;
				desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				desc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
				desc.StencilEnable = FALSE;
			}

			D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			D3D12_VIEWPORT viewport{ 0.f, 0.f, (float)passDesc.resX, (float)passDesc.resY, 0.f, 1.f };
			D3D12_RECT screenRect{ 0, 0, (LONG)passDesc.resX, (LONG)passDesc.resY };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.colorTarget->m_renderTextureIndices[0]) };
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, passDesc.depthStencilTarget->m_renderTextureIndices[0]);
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, &dsv);

			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// Constant buffer
			struct CbLayout
			{
				Matrix invParallaxViewProjMatrix;
				int envmapTextureIndex;
			};

			Matrix parallaxViewMatrix = passDesc.view->m_viewTransform;
			parallaxViewMatrix.Translation(Vector3::Zero);

			CbLayout constants{};
			constants.envmapTextureIndex = passDesc.scene->m_globalLightProbe.m_envmapTextureIndex;
			constants.invParallaxViewProjMatrix = (parallaxViewMatrix * passDesc.view->m_projectionTransform).Invert();
			d3dCmdList->SetGraphicsRoot32BitConstants(0, sizeof(CbLayout) / 4, &constants, 0);

			d3dCmdList->DrawInstanced(3, 1, 0, 0);

			return cmdList;
		});
	}

	concurrency::task<FCommandList*> Postprocess(const PostprocessPassDesc& passDesc)
	{
		return concurrency::create_task([passDesc]
		{
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"postprocess_job");

			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			SCOPED_GPU_EVENT(cmdList, L"post_process", 0);

			// MSAA resolve
			passDesc.colorSource->Transition(cmdList, 0, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
			passDesc.colorTarget->Transition(cmdList, 0, D3D12_RESOURCE_STATE_RESOLVE_DEST);
			d3dCmdList->ResolveSubresource(
				passDesc.colorTarget->m_resource->m_d3dResource,
				0,
				passDesc.colorSource->m_resource->m_d3dResource,
				0,
				Settings::k_backBufferFormat);

			return cmdList;
		});
	}

	concurrency::task<FCommandList*> UI()
	{
		return concurrency::create_task([]
		{
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"imgui_job");

			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_GPU_EVENT(cmdList, L"imgui_commands", 0);

			ImDrawData* drawData = ImGui::GetDrawData();
			size_t vtxBufferSize = 0;
			size_t idxBufferSize = 0;
			for (int i = 0; i < drawData->CmdListsCount; ++i)
			{
				const ImDrawList* imguiCL = drawData->CmdLists[i];
				vtxBufferSize += imguiCL->VtxBuffer.Size * sizeof(ImDrawVert);
				idxBufferSize += imguiCL->IdxBuffer.Size * sizeof(ImDrawIdx);
			}

			// Vertex Buffer
			{
				std::unique_ptr<FTransientBuffer> vtxBuffer = RenderBackend12::CreateTransientBuffer(
					L"imgui_vb",
					vtxBufferSize,
					cmdList,
					[drawData](uint8_t* pDest)
					{
						ImDrawVert* vbDest = reinterpret_cast<ImDrawVert*>(pDest);
						for (int i = 0; i < drawData->CmdListsCount; ++i)
						{
							const ImDrawList* imguiCL = drawData->CmdLists[i];
							memcpy(vbDest, imguiCL->VtxBuffer.Data, imguiCL->VtxBuffer.Size * sizeof(ImDrawVert));
							vbDest += imguiCL->VtxBuffer.Size;
						}
					});

				D3D12_VERTEX_BUFFER_VIEW vbDescriptor = {};
				vbDescriptor.BufferLocation = vtxBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress();
				vbDescriptor.SizeInBytes = vtxBufferSize;
				vbDescriptor.StrideInBytes = sizeof(ImDrawVert);
				d3dCmdList->IASetVertexBuffers(0, 1, &vbDescriptor);
			}

			// Index Buffer
			{
				std::unique_ptr<FTransientBuffer> idxBuffer = RenderBackend12::CreateTransientBuffer(
					L"imgui_ib",
					idxBufferSize,
					cmdList,
					[drawData](uint8_t* pDest)
					{
						ImDrawIdx* ibDest = reinterpret_cast<ImDrawIdx*>(pDest);
						for (int i = 0; i < drawData->CmdListsCount; ++i)
						{
							const ImDrawList* imguiCL = drawData->CmdLists[i];
							memcpy(ibDest, imguiCL->IdxBuffer.Data, imguiCL->IdxBuffer.Size * sizeof(ImDrawIdx));
							ibDest += imguiCL->IdxBuffer.Size;
						}
					});

				D3D12_INDEX_BUFFER_VIEW ibDescriptor = {};
				ibDescriptor.BufferLocation = idxBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress();
				ibDescriptor.SizeInBytes = idxBufferSize;
				ibDescriptor.Format = sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
				d3dCmdList->IASetIndexBuffer(&ibDescriptor);
			}

			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"imgui.hlsl", L"rootsig" });
			d3dCmdList->SetGraphicsRootSignature(rootsig.get());
			rootsig->SetName(L"imgui_rootsig");

			// Vertex Constant Buffer
			{
				struct _cb
				{
					float   mvp[4][4];
				} vtxConstantBuffer;

				float L = drawData->DisplayPos.x;
				float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
				float T = drawData->DisplayPos.y;
				float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
				float mvp[4][4] =
				{
					{ 2.0f / (R - L),   0.0f,				0.0f,       0.0f },
					{ 0.0f,				2.0f / (T - B),     0.0f,       0.0f },
					{ 0.0f,				0.0f,				0.5f,       0.0f },
					{ (R + L) / (L - R),(T + B) / (B - T),  0.5f,       1.0f },
				};
				memcpy(&vtxConstantBuffer.mvp, mvp, sizeof(mvp));
				d3dCmdList->SetGraphicsRoot32BitConstants(0, 16, &vtxConstantBuffer, 0);
			}

			// Pixel Constant Buffer
			{
				uint32_t fontSrvIndex = (uint32_t)ImGui::GetIO().Fonts->TexID;
				d3dCmdList->SetGraphicsRoot32BitConstants(1, 1, &fontSrvIndex, 0);
			}


			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.pRootSignature = rootsig.get();
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = Settings::k_backBufferFormat;
			psoDesc.SampleDesc.Count = 1;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"imgui.hlsl", L"vs_main", L"" }, L"vs_6_4");
				IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"imgui.hlsl", L"ps_main", L"" }, L"ps_6_4");

				vs.pShaderBytecode = vsBlob->GetBufferPointer();
				vs.BytecodeLength = vsBlob->GetBufferSize();
				ps.pShaderBytecode = psBlob->GetBufferPointer();
				ps.BytecodeLength = psBlob->GetBufferSize();
			}

			// PSO - Input Layout
			{
				static D3D12_INPUT_ELEMENT_DESC inputLayout[] =
				{
					{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (UINT)IM_OFFSETOF(ImDrawVert, pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
					{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (UINT)IM_OFFSETOF(ImDrawVert, uv),  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
					{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)IM_OFFSETOF(ImDrawVert, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				};
				psoDesc.InputLayout = { inputLayout, 3 };
			}

			// PSO - Rasterizer State
			{
				D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
				desc.FillMode = D3D12_FILL_MODE_SOLID;
				desc.CullMode = D3D12_CULL_MODE_NONE;
				desc.FrontCounterClockwise = FALSE;
				desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
				desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
				desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
				desc.DepthClipEnable = TRUE;
				desc.MultisampleEnable = FALSE;
				desc.AntialiasedLineEnable = FALSE;
				desc.ForcedSampleCount = 0;
				desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
			}

			// PSO - Blend State
			{
				D3D12_BLEND_DESC& desc = psoDesc.BlendState;
				desc.AlphaToCoverageEnable = FALSE;
				desc.IndependentBlendEnable = FALSE;
				desc.RenderTarget[0].BlendEnable = TRUE;
				desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
				desc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
				desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
				desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
				desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			}

			// PSO - Depth Stencil State
			{
				D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
				desc.DepthEnable = FALSE;
				desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				desc.StencilEnable = FALSE;
			}

			D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Viewport
			D3D12_VIEWPORT vp = {};
			vp.Width = drawData->DisplaySize.x;
			vp.Height = drawData->DisplaySize.y;
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			vp.TopLeftX = vp.TopLeftY = 0.0f;
			d3dCmdList->RSSetViewports(1, &vp);

			// Blend Factor
			const float blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
			d3dCmdList->OMSetBlendFactor(blendFactor);

			RenderBackend12::GetBackBuffer()->Transition(cmdList, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RenderBackend12::GetBackBuffer()->m_renderTextureIndices[0]) };
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, nullptr);

			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetBindlessShaderResourceHeap() };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);
			d3dCmdList->SetGraphicsRootDescriptorTable(2, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::Texture2DBegin));
			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// Render commands
			int vertexOffset = 0;
			int indexOffset = 0;
			ImVec2 clipOffset = drawData->DisplayPos;
			for (int n = 0; n < drawData->CmdListsCount; n++)
			{
				const ImDrawList* imguiCmdList = drawData->CmdLists[n];
				for (int cmdIndex = 0; cmdIndex < imguiCmdList->CmdBuffer.Size; ++cmdIndex)
				{
					const ImDrawCmd* pcmd = &imguiCmdList->CmdBuffer[cmdIndex];
					if (pcmd->UserCallback != NULL)
					{
						// User callback, registered via ImDrawList::AddCallback()
						// (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
						if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
						{
							DebugAssert(false);// ImGui_ImplDX12_SetupRenderState(drawData, d3dCmdList, fr);
						}
						else
						{
							pcmd->UserCallback(imguiCmdList, pcmd);
						}
					}
					else
					{
						// Apply Scissor, Bind texture, Draw
						const D3D12_RECT r = 
						{ 
							(LONG)(pcmd->ClipRect.x - clipOffset.x),
							(LONG)(pcmd->ClipRect.y - clipOffset.y),
							(LONG)(pcmd->ClipRect.z - clipOffset.x),
							(LONG)(pcmd->ClipRect.w - clipOffset.y)
						};

						if (r.right > r.left && r.bottom > r.top)
						{
							d3dCmdList->RSSetScissorRects(1, &r);
							d3dCmdList->DrawIndexedInstanced(pcmd->ElemCount, 1, pcmd->IdxOffset + indexOffset, pcmd->VtxOffset + vertexOffset, 0);
						}
					}
				}

				vertexOffset += imguiCmdList->IdxBuffer.Size;
				indexOffset += imguiCmdList->VtxBuffer.Size;
			}

			return cmdList;
		});
	}

	concurrency::task<FCommandList*> Present()
	{
		return concurrency::create_task([]
		{
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"present_job");

			RenderBackend12::GetBackBuffer()->Transition(cmdList, 0, D3D12_RESOURCE_STATE_PRESENT);

			return cmdList;
		});
	}
}

void Demo::Render(const uint32_t resX, const uint32_t resY)
{
	SCOPED_CPU_EVENT(L"Render", MP_YELLOW);

	const uint32_t sampleCount = 4;
	std::unique_ptr<FRenderTexture> colorBuffer = RenderBackend12::CreateRenderTexture(L"scene_color", Settings::k_backBufferFormat, resX, resY, 1, 1, sampleCount);
	std::unique_ptr<FRenderTexture> depthBuffer = RenderBackend12::CreateDepthStencilTexture(L"depth_buffer", DXGI_FORMAT_D32_FLOAT, resX, resY, 1, sampleCount);

	// Base pass
	RenderJob::BasePassDesc baseDesc
	{
		.colorTarget = colorBuffer.get(),
		.depthStencilTarget = depthBuffer.get(),
		.resX = resX,
		.resY = resY,
		.sampleCount = sampleCount,
		.scene = GetScene(),
		.view = GetView()
	};

	auto basePassCmdList = RenderJob::BasePass(baseDesc).get();
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { basePassCmdList });

	auto backgroundPassCmdList = RenderJob::BackgroundPass(baseDesc).get();
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { backgroundPassCmdList });

	// Post Process
	RenderJob::PostprocessPassDesc postDesc
	{
		.colorSource = colorBuffer.get(),
		.colorTarget = RenderBackend12::GetBackBuffer(),
		.resX = resX,
		.resY = resY,
		.view = GetView()
	};

	auto postCmdList = RenderJob::Postprocess(postDesc).get();
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { postCmdList });


	// UI
	ImDrawData* imguiDraws = ImGui::GetDrawData();
	if (imguiDraws && imguiDraws->CmdListsCount > 0)
	{
		auto uiCmdList = RenderJob::UI().get();
		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { uiCmdList });
	}

	// Present
	auto presentCmdList = RenderJob::Present().get();
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { presentCmdList });

	RenderBackend12::PresentDisplay();
	Profiling::Flip();
}