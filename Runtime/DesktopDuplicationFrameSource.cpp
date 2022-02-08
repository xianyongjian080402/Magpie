#include "pch.h"
#include "DesktopDuplicationFrameSource.h"
#include "App.h"
#include "Utils.h"
#include "DeviceResources.h"


extern std::shared_ptr<spdlog::logger> logger;

static winrt::com_ptr<IDXGIOutput1> FindMonitor(winrt::com_ptr<IDXGIAdapter1> adapter, HMONITOR hMonitor) {
	winrt::com_ptr<IDXGIOutput> output;

	for (UINT adapterIndex = 0;
			SUCCEEDED(adapter->EnumOutputs(adapterIndex, output.put()));
			adapterIndex++
	) {
		DXGI_OUTPUT_DESC desc;
		HRESULT hr = output->GetDesc(&desc);
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("GetDesc 失败", hr));
			continue;
		}

		if (desc.Monitor == hMonitor) {
			winrt::com_ptr<IDXGIOutput1> output1;
			if (!output.try_as(output1)) {
				SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("从 IDXGIOutput 获取 IDXGIOutput1 失败", hr));
				return nullptr;
			}

			return output1;
		}
	}

	return nullptr;
}

// 根据显示器句柄查找 IDXGIOutput1
static winrt::com_ptr<IDXGIOutput1> GetDXGIOutput(HMONITOR hMonitor) {
	DeviceResources& dr = App::GetInstance().GetDeviceResources();
	winrt::com_ptr<IDXGIAdapter1> curAdapter = dr.GetGraphicsAdapter();

	// 首先在当前使用的图形适配器上查询显示器
	winrt::com_ptr<IDXGIOutput1> output = FindMonitor(curAdapter, hMonitor);
	if (output) {
		return output;
	}

	// 未找到则在所有图形适配器上查找
	winrt::com_ptr<IDXGIAdapter1> adapter;
	winrt::com_ptr<IDXGIFactory2> dxgiFactory = dr.GetDXGIFactory();
	for (UINT adapterIndex = 0;
			SUCCEEDED(dxgiFactory->EnumAdapters1(adapterIndex, adapter.put()));
			adapterIndex++
	) {
		if (adapter == curAdapter) {
			continue;
		}

		output = FindMonitor(adapter, hMonitor);
		if (output) {
			return output;
		}
	}

	return nullptr;
}

DesktopDuplicationFrameSource::~DesktopDuplicationFrameSource() {
	_exiting = true;
	WaitForSingleObject(_hDDPThread, 1000);
}

bool DesktopDuplicationFrameSource::Initialize() {
	// WDA_EXCLUDEFROMCAPTURE 只在 Win10 v2004 及更新版本中可用
	const RTL_OSVERSIONINFOW& version = Utils::GetOSVersion();
	if (Utils::CompareVersion(version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber, 10, 0, 19041) < 0) {
		SPDLOG_LOGGER_ERROR(logger, "当前操作系统无法使用 Desktop Duplication");
		return false;
	}
	
	HWND hwndSrc = App::GetInstance().GetHwndSrc();

	HMONITOR hMonitor = MonitorFromWindow(hwndSrc, MONITOR_DEFAULTTONEAREST);
	if (!hMonitor) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("MonitorFromWindow 失败"));
		return false;
	}

	MONITORINFO mi{};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfo(hMonitor, &mi)) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("GetMonitorInfo 失败"));
		return false;
	}

	if (!_CenterWindowIfNecessary(hwndSrc, mi.rcWork)) {
		SPDLOG_LOGGER_ERROR(logger, "居中源窗口失败");
		return false;
	}

	if (!_UpdateSrcFrameRect()) {
		SPDLOG_LOGGER_ERROR(logger, "UpdateSrcFrameRect 失败");
		return false;
	}

	DeviceResources& dr = App::GetInstance().GetDeviceResources();
	winrt::com_ptr<ID3D12Device> d3dDevice = dr.GetD3DDevice();

	CD3DX12_HEAP_PROPERTIES heapDesc(D3D12_HEAP_TYPE_DEFAULT);
	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_B8G8R8A8_UNORM,
		static_cast<UINT64>(_srcFrameRect.right) - _srcFrameRect.left,
		static_cast<UINT64>(_srcFrameRect.bottom) - _srcFrameRect.top,
		1,
		1,
		1,
		0,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS
	);
	HRESULT hr = d3dDevice->CreateCommittedResource(&heapDesc, D3D12_HEAP_FLAG_SHARED,
		&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(_sharedTex.put()));

	desc.Flags = D3D12_RESOURCE_FLAG_NONE;
	hr = d3dDevice->CreateCommittedResource(&heapDesc, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(_output.put()));
	
	d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(_fence.put()));

	if (!_InitializeDdpD3D()) {
		SPDLOG_LOGGER_ERROR(logger, "初始化 D3D 失败");
		return false;
	}
	
	HANDLE hShared = NULL;
	d3dDevice->CreateSharedHandle(_sharedTex.get(), nullptr, GENERIC_ALL, nullptr, &hShared);
	_ddpD3dDevice->OpenSharedResource1(hShared, IID_PPV_ARGS(_ddpSharedTex.put()));

	d3dDevice->CreateSharedHandle(_fence.get(), nullptr, GENERIC_ALL, nullptr, &hShared);
	_ddpD3dDevice->OpenSharedFence(hShared, IID_PPV_ARGS(_ddpFence.put()));
	
	winrt::com_ptr<IDXGIOutput1> output = GetDXGIOutput(hMonitor);
	if (!output) {
		SPDLOG_LOGGER_ERROR(logger, "无法找到 IDXGIOutput");
		return false;
	}

	hr = output->DuplicateOutput(_ddpD3dDevice.get(), _outputDup.put());
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("DuplicateOutput 失败", hr));
		return false;
	}

	// 计算源窗口客户区在该屏幕上的位置，用于计算新帧是否有更新
	_srcClientInMonitor = {
		_srcFrameRect.left - mi.rcMonitor.left,
		_srcFrameRect.top - mi.rcMonitor.top,
		_srcFrameRect.right - mi.rcMonitor.left,
		_srcFrameRect.bottom - mi.rcMonitor.top
	};

	_frameInMonitor = {
		(UINT)_srcClientInMonitor.left,
		(UINT)_srcClientInMonitor.top,
		0,
		(UINT)_srcClientInMonitor.right,
		(UINT)_srcClientInMonitor.bottom,
		1
	};

	// 使全屏窗口无法被捕获到
	if (!SetWindowDisplayAffinity(App::GetInstance().GetHwndHost(), WDA_EXCLUDEFROMCAPTURE)) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("SetWindowDisplayAffinity 失败"));
		return false;
	}
	

	_hDDPThread = CreateThread(nullptr, 0, _DDPThreadProc, this, 0, nullptr);
	if (!_hDDPThread) {
		return false;
	}

	SPDLOG_LOGGER_INFO(logger, "DesktopDuplicationFrameSource 初始化完成");
	return true;
}


FrameSourceBase::UpdateState DesktopDuplicationFrameSource::CaptureFrame() {
	UINT64 ddpFenceValue = _ddpFenceValue.load();

	if (ddpFenceValue == 0) {
		// 第一帧之前不渲染
		return UpdateState::Waiting;
	} else if (_fenceValue == ddpFenceValue) {
		return UpdateState::NoUpdate;
	}

	_fenceValue = ddpFenceValue;

	DeviceResources& dr = App::GetInstance().GetDeviceResources();
	dr.GetCommandQueue()->Wait(_fence.get(), ddpFenceValue);

	auto commandList = dr.GetCommandList();

	CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			_output.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, 0);
	commandList->ResourceBarrier(1, &barrier);
	dr.GetCommandList()->CopyResource(_output.get(), _sharedTex.get());
	barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			_output.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
	commandList->ResourceBarrier(1, &barrier);

	return UpdateState::NewFrame;
}

bool DesktopDuplicationFrameSource::_InitializeDdpD3D() {
	UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
		// 在 DEBUG 配置启用调试层
		createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0
	};
	UINT nFeatureLevels = ARRAYSIZE(featureLevels);

	// 使用和 Renderer 相同的图像适配器以避免 GPU 间的纹理拷贝
	winrt::com_ptr<ID3D11Device> device;
	winrt::com_ptr<ID3D11DeviceContext> dc;
	HRESULT hr = D3D11CreateDevice(
		App::GetInstance().GetDeviceResources().GetGraphicsAdapter().get(),
		D3D_DRIVER_TYPE_UNKNOWN,
		nullptr,
		createDeviceFlags,
		featureLevels,
		nFeatureLevels,
		D3D11_SDK_VERSION,
		device.put(),
		nullptr,
		dc.put()
	);

	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("D3D11CreateDevice 失败", hr));
		return false;
	}

	device.try_as(_ddpD3dDevice);
	dc.try_as(_ddpD3dDC);
	
	return true;
}

DWORD WINAPI DesktopDuplicationFrameSource::_DDPThreadProc(LPVOID lpThreadParameter) {
	DesktopDuplicationFrameSource& that = *(DesktopDuplicationFrameSource*)lpThreadParameter;

	DXGI_OUTDUPL_FRAME_INFO info{};
	winrt::com_ptr<IDXGIResource> dxgiRes;
	std::vector<BYTE> dupMetaData;

	while (!that._exiting.load()) {
		if (dxgiRes) {
			that._outputDup->ReleaseFrame();
		}
		HRESULT hr = that._outputDup->AcquireNextFrame(500, &info, dxgiRes.put());
		if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
			continue;
		}

		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("AcquireNextFrame 失败", hr));
			continue;
		}

		bool noUpdate = true;

		// 检索 move rects 和 dirty rects
		// 这些区域如果和窗口客户区有重叠则表明画面有变化
		if (info.TotalMetadataBufferSize) {
			if (info.TotalMetadataBufferSize > dupMetaData.size()) {
				dupMetaData.resize(info.TotalMetadataBufferSize);
			}

			UINT bufSize = info.TotalMetadataBufferSize;

			// move rects
			hr = that._outputDup->GetFrameMoveRects(bufSize, (DXGI_OUTDUPL_MOVE_RECT*)dupMetaData.data(), &bufSize);
			if (FAILED(hr)) {
				SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("GetFrameMoveRects 失败", hr));
				continue;
			}

			UINT nRect = bufSize / sizeof(DXGI_OUTDUPL_MOVE_RECT);
			for (UINT i = 0; i < nRect; ++i) {
				const DXGI_OUTDUPL_MOVE_RECT& rect = ((DXGI_OUTDUPL_MOVE_RECT*)dupMetaData.data())[i];
				if (Utils::CheckOverlap(that._srcClientInMonitor, rect.DestinationRect)) {
					noUpdate = false;
					break;
				}
			}

			if (noUpdate) {
				bufSize = info.TotalMetadataBufferSize;

				// dirty rects
				hr = that._outputDup->GetFrameDirtyRects(bufSize, (RECT*)dupMetaData.data(), &bufSize);
				if (FAILED(hr)) {
					SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("GetFrameDirtyRects 失败", hr));
					continue;
				}

				nRect = bufSize / sizeof(RECT);
				for (UINT i = 0; i < nRect; ++i) {
					const RECT& rect = ((RECT*)dupMetaData.data())[i];
					if (Utils::CheckOverlap(that._srcClientInMonitor, rect)) {
						noUpdate = false;
						break;
					}
				}
			}
		}

		if (noUpdate) {
			continue;
		}

		winrt::com_ptr<ID3D11Resource> d3dRes = dxgiRes.try_as<ID3D11Resource>();
		if (!d3dRes) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("从 IDXGIResource 检索 ID3D11Resource 失败", hr));
			continue;
		}

		UINT64 fenceValue = ++that._ddpFenceValue;
		that._ddpD3dDC->CopySubresourceRegion(that._ddpSharedTex.get(), 0, 0, 0, 0, d3dRes.get(), 0, &that._frameInMonitor);
		that._ddpD3dDC->Signal(that._ddpFence.get(), fenceValue);
		that._ddpD3dDC->Flush();
	}

	return 0;
}
