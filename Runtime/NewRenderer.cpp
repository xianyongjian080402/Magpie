#include "pch.h"
#include "NewRenderer.h"
#include "StrUtils.h"
#include "App.h"


extern std::shared_ptr<spdlog::logger> logger;

bool NewRenderer::Initialize() {
	const RECT& hostWndRect = App::GetInstance().GetHostWndRect();
	SIZE hostSize = { hostWndRect.right - hostWndRect.left, hostWndRect.bottom - hostWndRect.top };

	m_viewport = CD3DX12_VIEWPORT(
		0.0f, 0.0f,
		static_cast<float>(hostSize.cx),
		static_cast<float>(hostSize.cy)
	);
	m_scissorRect = CD3DX12_RECT(0, 0, hostSize.cx, hostSize.cy);

	if (!_LoadPipeline()) {
		return false;
	}

	if (!_LoadAssets()) {
		return false;
	}

	return true;
}

void NewRenderer::Render() {
	_PopulateCommandList();

	ID3D12CommandList* ppCommandLists[] = { m_commandList.get() };
	_cmdQueue->ExecuteCommandLists((UINT)std::size(ppCommandLists), ppCommandLists);

	_dxgiSwapChain->Present(1, 0);
	_WaitForPreviousFrame();
}

NewRenderer::~NewRenderer() {
	_WaitForPreviousFrame();
}

static inline void LogAdapter(const DXGI_ADAPTER_DESC1& adapterDesc) {
	SPDLOG_LOGGER_INFO(logger, fmt::format("当前图形适配器：\n\tVendorId：{:#x}\n\tDeviceId：{:#x}\n\t描述：{}",
		adapterDesc.VendorId, adapterDesc.DeviceId, StrUtils::UTF16ToUTF8(adapterDesc.Description)));
}

static winrt::com_ptr<IDXGIAdapter1> ObtainGraphicsAdapter(winrt::com_ptr<IDXGIFactory4> dxgiFactory, int adapterIdx) {
	winrt::com_ptr<IDXGIAdapter1> adapter;
	
	if (adapterIdx >= 0) {
		HRESULT hr = dxgiFactory->EnumAdapters1(adapterIdx, adapter.put());
		if (SUCCEEDED(hr)) {
			DXGI_ADAPTER_DESC1 desc;
			HRESULT hr = adapter->GetDesc1(&desc);
			if (SUCCEEDED(hr)) {
				// 测试此适配器是否支持 D3D12
				hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr);
				if (SUCCEEDED(hr)) {
					LogAdapter(desc);
					return adapter;
				}
			}
		}
	}

	// 指定 GPU 失败，枚举查找第一个支持 D3D12 的图形适配器

	for (UINT adapterIndex = 0;
			SUCCEEDED(dxgiFactory->EnumAdapters1(adapterIndex,
				adapter.put()));
			adapterIndex++
	) {
		DXGI_ADAPTER_DESC1 desc;
		HRESULT hr = adapter->GetDesc1(&desc);
		if (FAILED(hr)) {
			continue;
		}

		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
			continue;
		}
		
		// 测试此适配器是否支持 D3D12
		hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr);
		if (SUCCEEDED(hr)) {
			LogAdapter(desc);
			return adapter;
		}
	}

	SPDLOG_LOGGER_INFO(logger, "未找到可用图形适配器");

	// 回落到 Basic Render Driver Adapter（WARP）
	// https://docs.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp
	HRESULT hr = dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put()));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 WARP 设备失败", hr));
		return nullptr;
	}

	return adapter;
}

bool NewRenderer::_CreateSwapChain() {
	HRESULT hr{};
	const RECT& hostWndRect = App::GetInstance().GetHostWndRect();

	// 检查可变帧率支持
	BOOL allowTearing = FALSE;
	{
		winrt::com_ptr<IDXGIFactory5> dxgiFactory5 = _dxgiFactory.try_as<IDXGIFactory5>();
		if (!dxgiFactory5) {
			SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("获取 IDXGIFactory5 失败", hr));
		} else {
			hr = dxgiFactory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
			if (FAILED(hr)) {
				SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("CheckFeatureSupport 失败", hr));
			}
		}

		SPDLOG_LOGGER_INFO(logger, fmt::format("可变刷新率支持：{}", allowTearing ? "是" : "否"));

		if (App::GetInstance().GetFrameRate() != 0 && !allowTearing) {
			SPDLOG_LOGGER_ERROR(logger, "当前显示器不支持可变刷新率");
			App::GetInstance().SetErrorMsg(ErrorMessages::VSYNC_OFF_NOT_SUPPORTED);
			return false;
		}
	}

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = hostWndRect.right - hostWndRect.left;
	sd.Height = hostWndRect.bottom - hostWndRect.top;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
	sd.BufferCount = _FRAME_COUNT;
	// 使用 DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL 而不是 DXGI_SWAP_EFFECT_FLIP_DISCARD
	// 不渲染四周（可能存在的）黑边，因此必须保证交换链缓冲区不被改变
	// 否则将不得不在每帧渲染前清空后缓冲区，这个操作在一些显卡上比较耗时
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	// 只要显卡支持始终启用 DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
	sd.Flags = (allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
		| (App::GetInstance().GetFrameRate() == 0 ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);

	winrt::com_ptr<IDXGISwapChain1> dxgiSwapChain = nullptr;
	hr = _dxgiFactory->CreateSwapChainForHwnd(
		_cmdQueue.get(),
		App::GetInstance().GetHwndHost(),
		&sd,
		nullptr,
		nullptr,
		dxgiSwapChain.put()
	);
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建交换链失败", hr));
		return false;
	}

	if (!dxgiSwapChain.try_as(_dxgiSwapChain)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("获取 IDXGISwapChain2 失败", hr));
		return false;
	}

	if (App::GetInstance().GetFrameRate() == 0) {
		// 关闭低延迟模式时将最大延迟设为 2 以使 CPU 和 GPU 并行执行
		_dxgiSwapChain->SetMaximumFrameLatency(App::GetInstance().IsDisableLowLatency() ? 2 : 1);

		_frameLatencyWaitableObject.reset(_dxgiSwapChain->GetFrameLatencyWaitableObject());
		if (!_frameLatencyWaitableObject) {
			SPDLOG_LOGGER_ERROR(logger, "GetFrameLatencyWaitableObject 失败");
			return false;
		}
	}

	hr = _dxgiFactory->MakeWindowAssociation(App::GetInstance().GetHwndHost(), DXGI_MWA_NO_ALT_ENTER);
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("MakeWindowAssociation 失败", hr));
	}

	// 检查 Multiplane Overlay 和 Hardware Composition 支持
	BOOL supportMPO = FALSE;
	BOOL supportHardwareComposition = FALSE;
	winrt::com_ptr<IDXGIOutput> output;
	hr = _dxgiSwapChain->GetContainingOutput(output.put());
	if (FAILED(hr)) {
		SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("获取 IDXGIOutput 失败", hr));
	} else {
		winrt::com_ptr<IDXGIOutput2> output2 = output.try_as<IDXGIOutput2>();
		if (!output2) {
			SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("获取 IDXGIOutput2 失败", hr));
		} else {
			supportMPO = output2->SupportsOverlays();
		}

		winrt::com_ptr<IDXGIOutput6> output6 = output.try_as<IDXGIOutput6>();
		if (!output6) {
			SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("获取 IDXGIOutput6 失败", hr));
		} else {
			UINT flags;
			hr = output6->CheckHardwareCompositionSupport(&flags);
			if (FAILED(hr)) {
				SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("CheckHardwareCompositionSupport 失败", hr));
			} else {
				supportHardwareComposition = flags & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED;
			}
		}
	}

	SPDLOG_LOGGER_INFO(logger, fmt::format("Hardware Composition 支持：{}", supportHardwareComposition ? "是" : "否"));
	SPDLOG_LOGGER_INFO(logger, fmt::format("Multiplane Overlay 支持：{}", supportMPO ? "是" : "否"));

	return true;
}

static void LogFeatureLevel(winrt::com_ptr<ID3D12Device> d3dDevice) {
	D3D_FEATURE_LEVEL requestedLevels[] = {
		D3D_FEATURE_LEVEL_12_2,
		D3D_FEATURE_LEVEL_12_1,
		D3D_FEATURE_LEVEL_12_0,
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0
	};

	D3D12_FEATURE_DATA_FEATURE_LEVELS queryData{};
	queryData.pFeatureLevelsRequested = requestedLevels;
	queryData.NumFeatureLevels = ARRAYSIZE(requestedLevels);

	HRESULT hr = d3dDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &queryData, sizeof(queryData));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("检查功能级别失败", hr));
		return;
	}

	std::string_view fl;
	switch (queryData.MaxSupportedFeatureLevel) {
	case D3D_FEATURE_LEVEL_12_2:
		fl = "12.2";
		break;
	case D3D_FEATURE_LEVEL_12_1:
		fl = "12.1";
		break;
	case D3D_FEATURE_LEVEL_12_0:
		fl = "12.0";
		break;
	case D3D_FEATURE_LEVEL_11_1:
		fl = "11.1";
		break;
	case D3D_FEATURE_LEVEL_11_0:
		fl = "11.0";
		break;
	default:
		fl = "未知";
		break;
	}

	SPDLOG_LOGGER_INFO(logger, fmt::format("D3D 设备功能级别：{}", fl));
}

bool NewRenderer::_LoadPipeline() {
#ifdef _DEBUG
	// 调试模式下启用 D3D 调试层
	{
		winrt::com_ptr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.put())))) {
			debugController->EnableDebugLayer();
			SPDLOG_LOGGER_INFO(logger, "已启用 D3D12 调试层");
		}
	}

	// 启用 DXGI 调试可以捕获到枚举图形适配器和创建 D3D 设备时发生的错误
	UINT dxgiFlag = DXGI_CREATE_FACTORY_DEBUG;
#else
	UINT dxgiFlag = 0;
#endif // _DEBUG

	HRESULT hr = CreateDXGIFactory2(dxgiFlag, IID_PPV_ARGS(_dxgiFactory.put()));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateDXGIFactory1 失败", hr));
		return false;
	}

	winrt::com_ptr<IDXGIAdapter1> adapter = ObtainGraphicsAdapter(_dxgiFactory, App::GetInstance().GetAdapterIdx());
	if (!adapter) {
		SPDLOG_LOGGER_ERROR(logger, "没有可用的图形适配器");
		return false;
	}

	// 创建 D3D12 设备
	hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(_d3dDevice.put()));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 D3D12 设备失败", hr));
		return false;
	}

	LogFeatureLevel(_d3dDevice);

#ifdef _DEBUG
	/*
	// 调试层报告错误时中断程序
	{
		winrt::com_ptr<ID3D12InfoQueue> d3dInfoQueue = _d3dDevice.try_as<ID3D12InfoQueue>();
		if (d3dInfoQueue) {
			d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
			d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		} else {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("获取 ID3D12InfoQueue 失败", hr));
		}
	}*/
#endif

	// 创建命令队列
	{
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		hr = _d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(_cmdQueue.put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建命令队列失败", hr));
			return false;
		}
	}

	// 创建交换链
	if (!_CreateSwapChain()) {
		SPDLOG_LOGGER_ERROR(logger, "创建交换链失败");
		return false;
	}

	// 为 RTV 创建描述符堆
	{
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = _FRAME_COUNT;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hr = _d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(_rtvHeap.put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建描述符堆失败", hr));
			return false;
		}

		_rtvDescriptorSize = _d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}
	
	// 为每个帧缓冲区创建 RTV
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(_rtvHeap->GetCPUDescriptorHandleForHeapStart());

		for (UINT n = 0; n < _FRAME_COUNT; n++) {
			hr = _dxgiSwapChain->GetBuffer(n, IID_PPV_ARGS(_renderTargets[n].put()));
			if (FAILED(hr)) {
				SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("GetBuffer 失败", hr));
				return false;
			}
			_d3dDevice->CreateRenderTargetView(_renderTargets[n].get(), nullptr, rtvHandle);
			rtvHandle.Offset(_rtvDescriptorSize);
		}
	}
	
	// 创建命令分配器
	hr = _d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(_cmdAllocator.put()));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建命令分配器 失败", hr));
		return false;
	}

	return true;
}

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

bool NewRenderer::_LoadAssets() {
	HRESULT hr;

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

		hr = _d3dDevice->CreateRootSignature(0, signature->GetBufferPointer(),
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

		hr = _d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_pipelineState.put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateGraphicsPipelineState 失败", hr));
			return false;
		}
	}

	// 创建命令列表
	hr = _d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _cmdAllocator.get(), m_pipelineState.get(), IID_PPV_ARGS(m_commandList.put()));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateCommandList 失败", hr));
		return false;
	}

	hr = m_commandList->Close();
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("关闭命令列表失败", hr));
		return false;
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
		hr = _d3dDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_vertexBuffer.put()));
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

	// 创建同步对象
	{
		hr = _d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateFence 失败", hr));
			return false;
		}

		m_fenceValue = 1;

		m_fenceEvent.reset(CreateEvent(nullptr, FALSE, FALSE, nullptr));
		if (!m_fenceEvent) {
			SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("CreateEvent 失败"));
			return false;
		}

		_WaitForPreviousFrame();
	}

	return true;
}

bool NewRenderer::_PopulateCommandList() {
	_cmdAllocator->Reset();

	m_commandList->Reset(_cmdAllocator.get(), m_pipelineState.get());

	// Set necessary state.
	m_commandList->SetGraphicsRootSignature(_rootSignature.get());
	m_commandList->RSSetViewports(1, &m_viewport);
	m_commandList->RSSetScissorRects(1, &m_scissorRect);

	// Indicate that the back buffer will be used as a render target.
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(_renderTargets[m_frameIndex].get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_commandList->ResourceBarrier(1, &barrier);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, _rtvDescriptorSize);
	m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

	// Record commands.
	const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
	m_commandList->DrawInstanced(3, 1, 0, 0);

	// Indicate that the back buffer will now be used to present.
	barrier = CD3DX12_RESOURCE_BARRIER::Transition(_renderTargets[m_frameIndex].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	m_commandList->ResourceBarrier(1, &barrier);

	m_commandList->Close();
	return true;
}

bool NewRenderer::_WaitForPreviousFrame() {
	// Signal and increment the fence value.
	const UINT64 fence = m_fenceValue;
	HRESULT hr = _cmdQueue->Signal(m_fence.get(), fence);
	if (FAILED(hr)) {
		return false;
	}
	m_fenceValue++;

	// Wait until the previous frame is finished.
	if (m_fence->GetCompletedValue() < fence) {
		hr = m_fence->SetEventOnCompletion(fence, m_fenceEvent.get());
		if (FAILED(hr)) {
			return false;
		}
		WaitForSingleObject(m_fenceEvent.get(), INFINITE);
	}

	m_frameIndex = _dxgiSwapChain->GetCurrentBackBufferIndex();
	return true;
}

