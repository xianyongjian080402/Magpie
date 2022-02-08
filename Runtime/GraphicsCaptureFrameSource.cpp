#include "pch.h"
#include "GraphicsCaptureFrameSource.h"
#include "App.h"
#include "StrUtils.h"
#include <Windows.Graphics.DirectX.Direct3D11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include "Utils.h"
#include "DeviceResources.h"


namespace winrt {
using namespace Windows::Foundation::Metadata;
}


extern std::shared_ptr<spdlog::logger> logger;

bool GraphicsCaptureFrameSource::Initialize() {
	App::GetInstance().SetErrorMsg(ErrorMessages::FAILED_TO_CAPTURE);

	// 只在 Win10 1903 及更新版本中可用
	const RTL_OSVERSIONINFOW& version = Utils::GetOSVersion();
	if (Utils::CompareVersion(version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber, 10, 0, 18362) < 0) {
		SPDLOG_LOGGER_ERROR(logger, "当前操作系统无法使用 Graphics Capture");
		return false;
	}

	//HRESULT hr;
	
	winrt::impl::com_ref<IGraphicsCaptureItemInterop> interop;
	try {
		if (!winrt::ApiInformation::IsTypePresent(L"Windows.Graphics.Capture.GraphicsCaptureSession")) {
			SPDLOG_LOGGER_ERROR(logger, "不存在 GraphicsCaptureSession API");
			return false;
		}
		if (!winrt::GraphicsCaptureSession::IsSupported()) {
			SPDLOG_LOGGER_ERROR(logger, "当前不支持 WinRT 捕获");
			return false;
		}

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

		device.try_as(_wgcD3dDevice);
		dc.try_as(_wgcD3dDC);

		winrt::com_ptr<IDXGIDevice> dxgiDevice = device.try_as<IDXGIDevice>();

		hr = CreateDirect3D11DeviceFromDXGIDevice(
			dxgiDevice.get(),
			reinterpret_cast<::IInspectable**>(winrt::put_abi(_wrappedD3DDevice))
		);
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 IDirect3DDevice 失败", hr));
			return false;
		}

		// 从窗口句柄获取 GraphicsCaptureItem
		interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
		if (!interop) {
			SPDLOG_LOGGER_ERROR(logger, "获取 IGraphicsCaptureItemInterop 失败");
			return false;
		}
	} catch (const winrt::hresult_error& e) {
		SPDLOG_LOGGER_ERROR(logger, fmt::format("初始化 WinRT 失败：{}", StrUtils::UTF16ToUTF8(e.message())));
		return false;
	}

	// 有两层回落：
	// 1. 首先使用常规的窗口捕获
	// 2. 如果失败，尝试设置源窗口样式，因为 WGC 只能捕获位于 Alt+Tab 列表中的窗口
	// 3. 如果再次失败，改为使用屏幕捕获
	if (!_CaptureFromWindow(interop)) {
		SPDLOG_LOGGER_INFO(logger, "窗口捕获失败，尝试设置源窗口样式");

		if (!_CaptureFromStyledWindow(interop)) {
			SPDLOG_LOGGER_INFO(logger, "窗口捕获失败，尝试使用屏幕捕获");

			if (!_CaptureFromMonitor(interop)) {
				SPDLOG_LOGGER_ERROR(logger, "屏幕捕获失败");
				return false;
			} else {
				_isScreenCapture = true;
			}
		}
	}

	auto d3dDevice = App::GetInstance().GetDeviceResources().GetD3DDevice();

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
		&desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(_sharedTex.put()));

	desc.Flags = D3D12_RESOURCE_FLAG_NONE;
	hr = d3dDevice->CreateCommittedResource(&heapDesc, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(_output.put()));

	d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(_fence.put()));

	HANDLE hShared = NULL;
	d3dDevice->CreateSharedHandle(_sharedTex.get(), nullptr, GENERIC_ALL, nullptr, &hShared);
	_wgcD3dDevice->OpenSharedResource1(hShared, IID_PPV_ARGS(_wgcSharedTex.put()));

	d3dDevice->CreateSharedHandle(_fence.get(), nullptr, GENERIC_ALL, nullptr, &hShared);
	_wgcD3dDevice->OpenSharedFence(hShared, IID_PPV_ARGS(_wgcFence.put()));

	InitializeConditionVariable(&_cv);
	InitializeCriticalSection(&_cs);

	try {
		// 创建帧缓冲池
		// 帧的尺寸和 _captureItem.Size() 不同
		_captureFramePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
			_wrappedD3DDevice,
			winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
			1,	// 帧的缓存数量
			{ (int)_frameBox.right, (int)_frameBox.bottom } // 帧的尺寸为包含源窗口的最小尺寸
		);

		_frameArrived = _captureFramePool.FrameArrived(
			winrt::auto_revoke,
			{ this, &GraphicsCaptureFrameSource::_OnFrameArrived }
		);

		_captureSession = _captureFramePool.CreateCaptureSession(_captureItem);

		// 不捕获光标
		if (winrt::ApiInformation::IsPropertyPresent(
			L"Windows.Graphics.Capture.GraphicsCaptureSession",
			L"IsCursorCaptureEnabled"
		)) {
			// 从 v2004 开始提供
			_captureSession.IsCursorCaptureEnabled(false);
		} else {
			SPDLOG_LOGGER_INFO(logger, "当前系统无 IsCursorCaptureEnabled API");
		}

		// 不显示黄色边框
		if (winrt::ApiInformation::IsPropertyPresent(
			L"Windows.Graphics.Capture.GraphicsCaptureSession",
			L"IsBorderRequired"
		)) {
			// 从 Win10 v2104 开始提供
			// 先请求权限
			auto status = winrt::GraphicsCaptureAccess::RequestAccessAsync(winrt::GraphicsCaptureAccessKind::Borderless).get();
			if (status == decltype(status)::Allowed) {
				_captureSession.IsBorderRequired(false);
			} else {
				SPDLOG_LOGGER_INFO(logger, "IsCursorCaptureEnabled 失败");
			}
		} else {
			SPDLOG_LOGGER_INFO(logger, "当前系统无 IsBorderRequired API");
		}

		// 开始捕获
		_captureSession.StartCapture();
	} catch (const winrt::hresult_error& e) {
		SPDLOG_LOGGER_INFO(logger, fmt::format("Graphics Capture 失败：", StrUtils::UTF16ToUTF8(e.message())));
		return false;
	}

	App::GetInstance().SetErrorMsg(ErrorMessages::GENERIC);
	SPDLOG_LOGGER_INFO(logger, "GraphicsCaptureFrameSource 初始化完成");
	return true;
}

FrameSourceBase::UpdateState GraphicsCaptureFrameSource::CaptureFrame() {
	UINT64 wgcFenceValue = _wgcFenceValue.load();

	// 每次睡眠 1 毫秒等待新帧到达，防止 CPU 占用过高
	EnterCriticalSection(&_cs);

	if (wgcFenceValue == _fenceValue) {
		SleepConditionVariableCS(&_cv, &_cs, 1);
	}

	LeaveCriticalSection(&_cs);

	wgcFenceValue = _wgcFenceValue.load();
	if (wgcFenceValue == _fenceValue) {
		return UpdateState::Waiting;
	}

	_fenceValue = wgcFenceValue;

	DeviceResources& dr = App::GetInstance().GetDeviceResources();
	dr.GetCommandQueue()->Wait(_fence.get(), wgcFenceValue);

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

bool GraphicsCaptureFrameSource::_CaptureFromWindow(winrt::impl::com_ref<IGraphicsCaptureItemInterop> interop) {
	// DwmGetWindowAttribute 和 Graphics.Capture 无法应用于子窗口
	HWND hwndSrc = App::GetInstance().GetHwndSrc();

	// 包含边框的窗口尺寸
	RECT srcRect{};
	if (!Utils::GetWindowFrameRect(hwndSrc, srcRect)) {
		SPDLOG_LOGGER_ERROR(logger, "GetWindowFrameRect 失败");
		return false;
	}

	if (!_UpdateSrcFrameRect()) {
		SPDLOG_LOGGER_ERROR(logger, "_UpdateSrcFrameRect 失败");
		return false;
	}

	// 在源窗口存在 DPI 缩放时有时会有一像素的偏移（取决于窗口在屏幕上的位置）
	// 可能是 DwmGetWindowAttribute 的 bug
	_frameBox = {
		UINT(_srcFrameRect.left - srcRect.left),
		UINT(_srcFrameRect.top - srcRect.top),
		0,
		UINT(_srcFrameRect.right - srcRect.left),
		UINT(_srcFrameRect.bottom - srcRect.top),
		1
	};

	try {
		HRESULT hr = interop->CreateForWindow(
			hwndSrc,
			winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
			winrt::put_abi(_captureItem)
		);
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 GraphicsCaptureItem 失败", hr));
			return false;
		}
	} catch (const winrt::hresult_error& e) {
		SPDLOG_LOGGER_INFO(logger, fmt::format("源窗口无法使用窗口捕获：", StrUtils::UTF16ToUTF8(e.message())));
		return false;
	}

	return true;
}

bool GraphicsCaptureFrameSource::_CaptureFromStyledWindow(winrt::impl::com_ref<IGraphicsCaptureItemInterop> interop) {
	HWND hwndSrc = App::GetInstance().GetHwndSrc();

	_srcWndStyle = GetWindowLongPtr(hwndSrc, GWL_EXSTYLE);
	if (_srcWndStyle == 0) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("GetWindowLongPtr 失败"));
		return false;
	}

	// 删除 WS_EX_TOOLWINDOW，添加 WS_EX_APPWINDOW 样式，确保源窗口可被 Alt+Tab 选中
	LONG_PTR newStyle = (_srcWndStyle & !WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW;

	if (_srcWndStyle == newStyle) {
		// 如果源窗口已经可被 Alt+Tab 选中，则回落到屏幕捕获
		SPDLOG_LOGGER_INFO(logger, "源窗口无需改变样式");
		return false;
	}

	if (!SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, newStyle)) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("SetWindowLongPtr 失败"));
		return false;
	}

	if (!_CaptureFromWindow(interop)) {
		SPDLOG_LOGGER_ERROR(logger, "改变样式后捕获窗口失败");
		// 还原源窗口样式
		SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, _srcWndStyle);
		_srcWndStyle = 0;
		return false;
	}

	return true;
}

bool GraphicsCaptureFrameSource::_CaptureFromMonitor(winrt::impl::com_ref<IGraphicsCaptureItemInterop> interop) {
	// WDA_EXCLUDEFROMCAPTURE 只在 Win10 v2004 及更新版本中可用
	const RTL_OSVERSIONINFOW& version = Utils::GetOSVersion();
	if (Utils::CompareVersion(version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber, 10, 0, 19041) < 0) {
		SPDLOG_LOGGER_ERROR(logger, "当前操作系统无法使用全屏捕获");
		return false;
	}

	// 使全屏窗口无法被捕获到
	if (!SetWindowDisplayAffinity(App::GetInstance().GetHwndHost(), WDA_EXCLUDEFROMCAPTURE)) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("SetWindowDisplayAffinity 失败"));
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

	// 放在屏幕左上角而不是中间可以提高帧率，这里是为了和 DesktopDuplication 保持一致
	if (!_CenterWindowIfNecessary(hwndSrc, mi.rcWork)) {
		SPDLOG_LOGGER_ERROR(logger, "居中源窗口失败");
		return false;
	}

	if (!_UpdateSrcFrameRect()) {
		SPDLOG_LOGGER_ERROR(logger, "UpdateSrcFrameRect 失败");
		return false;
	}

	_frameBox = {
		UINT(_srcFrameRect.left - mi.rcMonitor.left),
		UINT(_srcFrameRect.top - mi.rcMonitor.top),
		0,
		UINT(_srcFrameRect.right - mi.rcMonitor.left),
		UINT(_srcFrameRect.bottom - mi.rcMonitor.top),
		1
	};

	try {
		HRESULT hr = interop->CreateForMonitor(
			hMonitor,
			winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
			winrt::put_abi(_captureItem)
		);
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 GraphicsCaptureItem 失败", hr));
			return false;
		}
	} catch (const winrt::hresult_error& e) {
		SPDLOG_LOGGER_INFO(logger, fmt::format("捕获屏幕失败：", StrUtils::UTF16ToUTF8(e.message())));
		return false;
	}

	return true;
}

void GraphicsCaptureFrameSource::_OnFrameArrived(winrt::Direct3D11CaptureFramePool const&, winrt::IInspectable const&) {
	winrt::Direct3D11CaptureFrame frame = _captureFramePool.TryGetNextFrame();
	if (!frame) {
		// 缓冲池没有帧，不应发生此情况
		assert(false);
		return;
	}

	// 从帧获取 IDXGISurface
	winrt::IDirect3DSurface d3dSurface = frame.Surface();

	winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> dxgiInterfaceAccess(
		d3dSurface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()
	);

	winrt::com_ptr<ID3D11Texture2D> withFrame;
	HRESULT hr = dxgiInterfaceAccess->GetInterface(IID_PPV_ARGS(withFrame.put()));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("从获取 IDirect3DSurface 获取 ID3D11Texture2D 失败", hr));
		return;
	}

	// 更改标志，如果主线程正在等待，唤醒主线程
	UINT64 fenceValue = ++_wgcFenceValue;

	_wgcD3dDC->CopySubresourceRegion(_wgcSharedTex.get(), 0, 0, 0, 0, withFrame.get(), 0, &_frameBox);
	_wgcD3dDC->Signal(_wgcFence.get(), fenceValue);
	_wgcD3dDC->Flush();

	WakeConditionVariable(&_cv);
}

GraphicsCaptureFrameSource::~GraphicsCaptureFrameSource() {
	if (_frameArrived) {
		_frameArrived.revoke();
	}
	if (_captureSession) {
		_captureSession.Close();
	}
	if (_captureFramePool) {
		_captureFramePool.Close();
	}

	// 还原源窗口样式
	if (_srcWndStyle) {
		SetWindowLongPtr(App::GetInstance().GetHwndSrc(), GWL_EXSTYLE, _srcWndStyle);
	}
}
