#include "pch.h"
#include "NewRenderer.h"
#include "StrUtils.h"
#include "App.h"


extern std::shared_ptr<spdlog::logger> logger;

bool NewRenderer::Initialize() {
	if (!_LoadPipeline()) {
		return false;
	}

	if (!_LoadAssets()) {
		return false;
	}

	return true;
}

static inline void LogAdapter(const DXGI_ADAPTER_DESC1& adapterDesc) {
	SPDLOG_LOGGER_INFO(logger, fmt::format("当前图形适配器：\n\tVendorId：{:#x}\n\tDeviceId：{:#x}\n\t描述：{}",
		adapterDesc.VendorId, adapterDesc.DeviceId, StrUtils::UTF16ToUTF8(adapterDesc.Description)));
}

static ComPtr<IDXGIAdapter1> ObtainGraphicsAdapter(IDXGIFactory4* dxgiFactory, int adapterIdx) {
	ComPtr<IDXGIAdapter1> adapter;
	
	if (adapterIdx >= 0) {
		HRESULT hr = dxgiFactory->EnumAdapters1(adapterIdx, adapter.ReleaseAndGetAddressOf());
		if (SUCCEEDED(hr)) {
			DXGI_ADAPTER_DESC1 desc;
			HRESULT hr = adapter->GetDesc1(&desc);
			if (SUCCEEDED(hr)) {
				// 测试此适配器是否支持 D3D12
				hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr);
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
				adapter.ReleaseAndGetAddressOf()));
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
		hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr);
		if (SUCCEEDED(hr)) {
			LogAdapter(desc);
			return adapter;
		}
	}

	SPDLOG_LOGGER_INFO(logger, "未找到可用图形适配器");

	// 回落到 Basic Render Driver Adapter（WARP）
	// https://docs.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp
	HRESULT hr = dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 WARP 设备失败", hr));
		return nullptr;
	}

	return adapter;
}

bool NewRenderer::_CreateSwapChain() {
	const RECT& hostWndRect = App::GetInstance().GetHostWndRect();

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = hostWndRect.right - hostWndRect.left;
	sd.Height = hostWndRect.bottom - hostWndRect.top;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
	sd.BufferCount = _GetFrameCount();
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = App::GetInstance().GetFrameRate() != 0 ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

	ComPtr<IDXGISwapChain1> dxgiSwapChain = nullptr;
	HRESULT hr = _dxgiFactory->CreateSwapChainForHwnd(
		_d3dCmdQueue.Get(),
		App::GetInstance().GetHwndHost(),
		&sd,
		nullptr,
		nullptr,
		&dxgiSwapChain
	);
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建交换链失败", hr));
		return false;
	}

	hr = dxgiSwapChain.As(&_dxgiSwapChain);
	if (FAILED(hr)) {
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
	ComPtr<IDXGIOutput> output;
	hr = _dxgiSwapChain->GetContainingOutput(&output);
	if (FAILED(hr)) {
		SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("获取 IDXGIOutput 失败", hr));
	} else {
		ComPtr<IDXGIOutput2> output2;
		hr = output.As(&output2);
		if (FAILED(hr)) {
			SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("获取 IDXGIOutput2 失败", hr));
		} else {
			supportMPO = output2->SupportsOverlays();
		}

		ComPtr<IDXGIOutput6> output6;
		hr = output.As(&output6);
		if (FAILED(hr)) {
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

static void LogFeatureLevel(ID3D12Device* d3dDevice) {
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
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
			SPDLOG_LOGGER_INFO(logger, "已启用 D3D12 调试层");
			debugController->EnableDebugLayer();
		}
	}
#endif // _DEBUG

	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&_dxgiFactory));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateDXGIFactory1 失败", hr));
		return false;
	}

	// 检查可变帧率支持
	{
		BOOL supportTearing = FALSE;
		ComPtr<IDXGIFactory5> dxgiFactory5;
		hr = _dxgiFactory.As(&dxgiFactory5);
		if (FAILED(hr)) {
			SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("获取 IDXGIFactory5 失败", hr));
		} else {
			hr = dxgiFactory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supportTearing, sizeof(supportTearing));
			if (FAILED(hr)) {
				SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("CheckFeatureSupport 失败", hr));
			}
		}

		SPDLOG_LOGGER_INFO(logger, fmt::format("可变刷新率支持：{}", supportTearing ? "是" : "否"));

		if (App::GetInstance().GetFrameRate() != 0 && !supportTearing) {
			SPDLOG_LOGGER_ERROR(logger, "当前显示器不支持可变刷新率");
			App::GetInstance().SetErrorMsg(ErrorMessages::VSYNC_OFF_NOT_SUPPORTED);
			return false;
		}
	}

	ComPtr<IDXGIAdapter1> adapter = ObtainGraphicsAdapter(_dxgiFactory.Get(), App::GetInstance().GetAdapterIdx());
	if (!adapter) {
		SPDLOG_LOGGER_ERROR(logger, "没有可用的图形适配器");
		return false;
	}

	// 创建 D3D12 设备
	hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_d3dDevice));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 D3D12 设备失败", hr));
		return false;
	}

	LogFeatureLevel(_d3dDevice.Get());

	// 创建命令队列
	{
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		hr = _d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_d3dCmdQueue));
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
		rtvHeapDesc.NumDescriptors = _GetFrameCount();
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hr = _d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&_rtvHeap));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建描述符堆失败", hr));
			return false;
		}

		m_rtvDescriptorSize = _d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}
	/*
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
		// Create a RTV for each frame.
		for (UINT n = 0; n < FrameCount; n++) {
			hr = _dxgiSwapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n]));
			_d3dDevice->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
			rtvHandle.ptr = SIZE_T((BYTE*)rtvHandle.ptr + m_rtvDescriptorSize);
		}
	}
	*/

	return true;
}

bool NewRenderer::_LoadAssets() {
	return false;
}

bool NewRenderer::_PopulateCommandList() {
	return false;
}

bool NewRenderer::_WaitForPreviousFrame() {
	return false;
}

int NewRenderer::_GetFrameCount() {
	return (App::GetInstance().IsDisableLowLatency() && App::GetInstance().GetFrameRate() == 0) ? 3 : 2;
}
