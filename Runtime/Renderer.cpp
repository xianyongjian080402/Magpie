#include "pch.h"
#include "Renderer.h"
#include "StrUtils.h"
#include "App.h"
#include "DeviceResources.h"


extern std::shared_ptr<spdlog::logger> logger;


static const char* shaderHlsl = R"(
struct PSInput {
	float4 position : SV_POSITION;
	float4 color : COLOR;
};

PSInput VSMain(float4 position : POSITION, float4 color : COLOR) {
	PSInput result;
	result.position = position;
	result.color = color;
	return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
	return input.color;
})";

bool Renderer::Initialize() {
	const RECT& hostWndRect = App::GetInstance().GetHostWndRect();
	SIZE hostSize = { hostWndRect.right - hostWndRect.left, hostWndRect.bottom - hostWndRect.top };

	m_viewport = CD3DX12_VIEWPORT(
		0.0f, 0.0f,
		static_cast<float>(hostSize.cx),
		static_cast<float>(hostSize.cy)
	);
	m_scissorRect = CD3DX12_RECT(0, 0, hostSize.cx, hostSize.cy);

	HRESULT hr{};
	const DeviceResources& dr = App::GetInstance().GetDeviceResources();
	winrt::com_ptr d3dDevice = dr.GetD3DDevice();

	// 为 RTV 创建描述符堆
	{
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = dr.GetBackBufferCount();
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hr = d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(_rtvHeap.put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建描述符堆失败", hr));
			return false;
		}

		_rtvDescriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}

	// 为每个帧缓冲区创建 RTV
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(_rtvHeap->GetCPUDescriptorHandleForHeapStart());

		for (UINT n = 0; n < dr.GetBackBufferCount(); n++) {
			d3dDevice->CreateRenderTargetView(dr.GetBackBuffers()[n].get(), nullptr, rtvHandle);
			rtvHandle.Offset(_rtvDescriptorSize);
		}
	}

	// 创建着色器根签名
	{
		D3D12_ROOT_SIGNATURE_DESC rootSignDesc{};
		rootSignDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		winrt::com_ptr<ID3DBlob> signature;
		winrt::com_ptr<ID3DBlob> error;

		HRESULT hr = D3D12SerializeRootSignature(&rootSignDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), error.put());
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg(fmt::format("D3D12SerializeRootSignature 失败：{}", (const char*)error->GetBufferPointer()), hr));
			return false;
		}

		hr = d3dDevice->CreateRootSignature(0, signature->GetBufferPointer(),
			signature->GetBufferSize(), IID_PPV_ARGS(_rootSignature.put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateRootSignature 失败", hr));
			return false;
		}
	}

	// 编译和加载着色器
	{
		winrt::com_ptr<ID3DBlob> vertexShader;
		winrt::com_ptr<ID3DBlob> pixelShader;

#ifdef _DEBUG
		// Enable better shader debugging with the graphics debugging tools.
		UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
		UINT compileFlags = 0;
#endif

		hr = D3DCompile(shaderHlsl, StrUtils::StrLen(shaderHlsl), nullptr, nullptr,
			nullptr, "VSMain", "vs_5_1", compileFlags, 0, vertexShader.put(), nullptr);
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("编译顶点着色器失败", hr));
			return false;
		}

		hr = D3DCompile(shaderHlsl, StrUtils::StrLen(shaderHlsl), nullptr, nullptr,
			nullptr, "PSMain", "ps_5_1", compileFlags, 0, pixelShader.put(), nullptr);
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("编译像素着色器失败", hr));
			return false;
		}

		// 顶点输入布局
		D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
			{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
			{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
		};

		// 创建图形管道状态对象（PSO）
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.InputLayout = { inputElementDescs, (UINT)std::size(inputElementDescs) };
		psoDesc.pRootSignature = _rootSignature.get();
		psoDesc.VS = { (UINT8*)vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
		psoDesc.PS = { (UINT8*)pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
		psoDesc.SampleDesc.Count = 1;

		hr = d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_pipelineState.put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateGraphicsPipelineState 失败", hr));
			return false;
		}
	}

	// 创建顶点缓冲区
	{
		const RECT& hostRect = App::GetInstance().GetHostWndRect();
		float aspectRatio = float(hostRect.right - hostRect.left) / (hostRect.bottom - hostRect.top);
		Vertex triangleVertices[] = {
			{ { 0.0f, 0.25f * aspectRatio, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
			{ { 0.25f, -0.25f * aspectRatio, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
			{ { -0.25f, -0.25f * aspectRatio, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }
		};

		constexpr UINT vertexBufferSize = sizeof(triangleVertices);

		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
		hr = d3dDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_vertexBuffer.put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建顶点缓冲区失败", hr));
			return false;
		}

		UINT8* pVertexDataBegin = nullptr;
		CD3DX12_RANGE readRange(0, 0);
		hr = m_vertexBuffer->Map(0, &readRange, (void**)(&pVertexDataBegin));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("Map 失败", hr));
			return false;
		}

		std::memcpy(pVertexDataBegin, triangleVertices, vertexBufferSize);

		m_vertexBuffer->Unmap(0, nullptr);

		// 初始化顶点缓冲区视图
		m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
		m_vertexBufferView.StrideInBytes = sizeof(Vertex);
		m_vertexBufferView.SizeInBytes = vertexBufferSize;
	}

	return true;
}

void Renderer::Render() {
	DeviceResources& dr = App::GetInstance().GetDeviceResources();

	if (!_waitingForNextFrame) {
		dr.WaitForSwapChain();
	}

	_PopulateCommandList();

	dr.Present(D3D12_RESOURCE_STATE_RENDER_TARGET);
}

bool Renderer::_PopulateCommandList() {
	DeviceResources& dr = App::GetInstance().GetDeviceResources();
	dr.PrepareForCurrentFrame();
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList = dr.GetCommandList();

	// Set necessary state.
	commandList->SetPipelineState(m_pipelineState.get());
	commandList->SetGraphicsRootSignature(_rootSignature.get());
	commandList->RSSetViewports(1, &m_viewport);
	commandList->RSSetScissorRects(1, &m_scissorRect);

	// Indicate that the back buffer will be used as a render target.
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		dr.GetCurrentBackBuffer().get(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET
	);
	commandList->ResourceBarrier(1, &barrier);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
		_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
		dr.GetBackBufferIndex(),
		_rtvDescriptorSize
	);
	commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

	// Record commands.
	const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
	commandList->DrawInstanced(3, 1, 0, 0);

	return true;
}
