#include "pch.h"
#include "DeviceResources.h"
#include "App.h"
#include "StrUtils.h"
#include <fmt/format.h>


extern std::shared_ptr<spdlog::logger> logger;

DeviceResources::~DeviceResources() {
	WaitForGPU();
}

static inline void LogAdapter(const DXGI_ADAPTER_DESC1& adapterDesc) {
	SPDLOG_LOGGER_INFO(logger, fmt::format("当前图形适配器：\n\tVendorId：{:#x}\n\tDeviceId：{:#x}\n\t描述：{}",
		adapterDesc.VendorId, adapterDesc.DeviceId, StrUtils::UTF16ToUTF8(adapterDesc.Description)));
}

static void LogFeatureLevel(D3D_FEATURE_LEVEL featureLevel) {
	std::string_view str;
	switch (featureLevel) {
	case D3D_FEATURE_LEVEL_12_2:
		str = "12.2";
		break;
	case D3D_FEATURE_LEVEL_12_1:
		str = "12.1";
		break;
	case D3D_FEATURE_LEVEL_12_0:
		str = "12.0";
		break;
	case D3D_FEATURE_LEVEL_11_1:
		str = "11.1";
		break;
	case D3D_FEATURE_LEVEL_11_0:
		str = "11.0";
		break;
	default:
		str = "未知";
		break;
	}

	SPDLOG_LOGGER_INFO(logger, fmt::format("D3D 设备功能级别：{}", str));
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

bool DeviceResources::Initialize(D3D12_COMMAND_LIST_TYPE commandListType) {
	App& app = App::GetInstance();

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

	_graphicsAdapter = ObtainGraphicsAdapter(_dxgiFactory, app.GetAdapterIdx());
	if (!_graphicsAdapter) {
		SPDLOG_LOGGER_ERROR(logger, "没有可用的图形适配器");
		return false;
	}

	// 创建 D3D12 设备
	hr = D3D12CreateDevice(_graphicsAdapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(_d3dDevice.put()));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 D3D12 设备失败", hr));
		return false;
	}
	
	// 获取功能级别
	{
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

		HRESULT hr = _d3dDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &queryData, sizeof(queryData));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("检查功能级别失败", hr));
			return false;
		}

		_d3dFeatureLevel = queryData.MaxSupportedFeatureLevel;
		LogFeatureLevel(_d3dFeatureLevel);
	}

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
		queueDesc.Type = commandListType;
		hr = _d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(_commandQueue.put()));
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

	UINT frameCount = App::GetInstance().IsDisableLowLatency() ? 2 : 1;

	// 创建命令分配器
	_commandAllocators.resize(frameCount);
	for (UINT i = 0; i < frameCount; ++i) {
		hr = _d3dDevice->CreateCommandAllocator(commandListType, IID_PPV_ARGS(_commandAllocators[i].put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建命令分配器失败", hr));
			return false;
		}
	}

	// 创建命令列表
	hr = _d3dDevice->CreateCommandList(0, commandListType, _commandAllocators[_curFrameIndex].get(),
		nullptr, IID_PPV_ARGS(_commandList.put()));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建命令列表失败", hr));
		return false;
	}

	// 创建同步对象
	{
		_fenceValues.resize(frameCount);
		hr = _d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(_fence.put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateFence 失败", hr));
			return false;
		}

		_fenceEvent.reset(CreateEvent(nullptr, FALSE, FALSE, nullptr));
		if (!_fenceEvent) {
			SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("CreateEvent 失败"));
			return false;
		}
	}

	_frameResources.resize(frameCount);

	return true;
}

void DeviceResources::SafeReleaseFrameResource(winrt::com_ptr<IUnknown> resource) {
	_frameResources[_curFrameIndex].push_back(std::move(resource));
}

void DeviceResources::BeginFrame() {
	// 等待此帧缓冲区
	_WaitForFence(_fenceValues[_curFrameIndex]);
	_frameResources[_curFrameIndex].clear();

	WaitForSingleObject(_frameLatencyWaitableObject.get(), 1000);

	_backBufferIndex = _swapChain->GetCurrentBackBufferIndex();

	if (_firstFrame) {
		_firstFrame = false;
		return;
	}

	HRESULT hr = _commandAllocators[_curFrameIndex]->Reset();
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("重置命令分配器失败", hr));
		return;
	}

	hr = _commandList->Reset(_commandAllocators[_curFrameIndex].get(), nullptr);
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("重置命令列表失败", hr));
		return;
	}
}

void DeviceResources::EndFrame(D3D12_RESOURCE_STATES currentBackBufferState) {
	if (currentBackBufferState != D3D12_RESOURCE_STATE_PRESENT) {
		D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			GetCurrentBackBuffer().get(), currentBackBufferState, D3D12_RESOURCE_STATE_PRESENT);
		_commandList->ResourceBarrier(1, &barrier);
	}

	// 将命令列表发送给 GPU
	HRESULT hr = _commandList->Close();
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("关闭命令列表失败", hr));
		return;
	}

	ID3D12CommandList* t[] = { _commandList.get() };
	_commandQueue->ExecuteCommandLists(1, t);

	if (App::GetInstance().IsDisableVSync()) {
		hr = _swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
	} else {
		hr = _swapChain->Present(1, 0);
	}

	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("Present 失败", hr));
		return;
	}

	const UINT64 currentFenceValue = _nextFenceValue++;
	_fenceValues[_curFrameIndex] = currentFenceValue;
	hr = _commandQueue->Signal(_fence.get(), currentFenceValue);
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("Signal 失败", hr));
		return;
	}
	
	_curFrameIndex = (_curFrameIndex + 1) % _fenceValues.size();

	_frameStatistics.Tick();
}

bool DeviceResources::WaitForGPU() {
	if (!_commandQueue || _fenceValues.empty()) {
		return true;
	}

	_WaitForFence(_fenceValues[_curFrameIndex]);
	return true;
}

bool DeviceResources::_CreateSwapChain() {
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

		if (App::GetInstance().IsDisableVSync() && !allowTearing) {
			SPDLOG_LOGGER_ERROR(logger, "无法关闭垂直同步");
			App::GetInstance().SetErrorMsg(ErrorMessages::VSYNC_OFF_NOT_SUPPORTED);
			return false;
		}
	}

	_backBufferCount = (!App::GetInstance().IsDisableVSync() && App::GetInstance().IsDisableLowLatency()) ? 3 : 2;

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = hostWndRect.right - hostWndRect.left;
	sd.Height = hostWndRect.bottom - hostWndRect.top;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = 0;
	sd.BufferCount = _backBufferCount;
	// 使用 DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL 而不是 DXGI_SWAP_EFFECT_FLIP_DISCARD
	// 不渲染四周（可能存在的）黑边，因此必须保证交换链缓冲区不被改变
	// 否则将不得不在每帧渲染前清空后缓冲区，这个操作在一些显卡上比较耗时
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	// 只要显卡支持始终启用 DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
	sd.Flags = (allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

	winrt::com_ptr<IDXGISwapChain1> dxgiSwapChain = nullptr;
	hr = _dxgiFactory->CreateSwapChainForHwnd(
		_commandQueue.get(),
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

	if (!dxgiSwapChain.try_as(_swapChain)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("获取 IDXGISwapChain2 失败", hr));
		return false;
	}

	// 关闭低延迟模式时将最大延迟设为 2 以使 CPU 和 GPU 并行执行
	_swapChain->SetMaximumFrameLatency(App::GetInstance().IsDisableLowLatency() ? 2 : 1);

	_frameLatencyWaitableObject.reset(_swapChain->GetFrameLatencyWaitableObject());
	if (!_frameLatencyWaitableObject) {
		SPDLOG_LOGGER_ERROR(logger, "GetFrameLatencyWaitableObject 失败");
		return false;
	}

	hr = _dxgiFactory->MakeWindowAssociation(App::GetInstance().GetHwndHost(), DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_PRINT_SCREEN);
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("MakeWindowAssociation 失败", hr));
	}

	// 检查 Multiplane Overlay 和 Hardware Composition 支持
	{
		BOOL supportMPO = FALSE;
		BOOL supportHardwareComposition = FALSE;
		winrt::com_ptr<IDXGIOutput> output;
		hr = _swapChain->GetContainingOutput(output.put());
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
	}

	_backBuffers.resize(_backBufferCount);
	for (UINT n = 0; n < _backBufferCount; n++) {
		hr = _swapChain->GetBuffer(n, IID_PPV_ARGS(_backBuffers[n].put()));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("GetBuffer 失败", hr));
			return false;
		}
	}

	_backBufferIndex = _swapChain->GetCurrentBackBufferIndex();

	return true;
}

void DeviceResources::_WaitForFence(UINT64 waitValue) {
	if (_fence->GetCompletedValue() < waitValue) {
		HRESULT hr = _fence->SetEventOnCompletion(waitValue, _fenceEvent.get());
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("SetEventOnCompletion 失败", hr));
			return;
		}

		WaitForSingleObject(_fenceEvent.get(), 1000);
	}
}
