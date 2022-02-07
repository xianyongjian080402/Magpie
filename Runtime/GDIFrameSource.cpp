#include "pch.h"
#include "GDIFrameSource.h"
#include "App.h"
#include "DeviceResources.h"


extern std::shared_ptr<spdlog::logger> logger;

bool GDIFrameSource::Initialize() {
	if (!_UpdateSrcFrameRect()) {
		SPDLOG_LOGGER_ERROR(logger, "_UpdateSrcFrameRect 失败");
		return false;
	}

	HWND hwndSrc = App::GetInstance().GetHwndSrc();

	double a, bx, by;
	if (_GetMapToOriginDPI(hwndSrc, a, bx, by)) {
		SPDLOG_LOGGER_INFO(logger, fmt::format("源窗口 DPI 缩放为 {}", 1 / a));

		_frameRect = {
			std::lround(_srcFrameRect.left * a + bx),
			std::lround(_srcFrameRect.top * a + by),
			std::lround(_srcFrameRect.right * a + bx),
			std::lround(_srcFrameRect.bottom * a + by)
		};
	} else {
		SPDLOG_LOGGER_ERROR(logger, "_GetMapToOriginDPI 失败");

		// _GetMapToOriginDPI 失败则假设 DPI 缩放为 1
		RECT srcWindowRect;
		if (!GetWindowRect(hwndSrc, &srcWindowRect)) {
			SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("GetWindowRect 失败"));
			return false;
		}

		_frameRect = {
			_srcFrameRect.left - srcWindowRect.left,
			_srcFrameRect.top - srcWindowRect.top,
			_srcFrameRect.right - srcWindowRect.left,
			_srcFrameRect.bottom - srcWindowRect.top
		};
	}
	
	if (_frameRect.left < 0 || _frameRect.top < 0 || _frameRect.right < 0
		|| _frameRect.bottom < 0 || _frameRect.right - _frameRect.left <= 0
		|| _frameRect.bottom - _frameRect.top <= 0
	) {
		App::GetInstance().SetErrorMsg(ErrorMessages::FAILED_TO_CROP);
		SPDLOG_LOGGER_ERROR(logger, "裁剪失败");
		return false;
	}

	DeviceResources& dr = App::GetInstance().GetDeviceResources();
	UINT d3d11DeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
	d3d11DeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif // _DEBUG

	IUnknown* commandQueue = dr.GetCommandQueue().get();
	D3D11On12CreateDevice(dr.GetD3DDevice().get(), d3d11DeviceFlags, nullptr, 0, 
		&commandQueue, 1, 0, _d3d11Device.put(), _d3d11DC.put(), nullptr);

	_d3d11Device.try_as(_d3d11On12Device);

	CD3DX12_HEAP_PROPERTIES heapDesc(D3D12_HEAP_TYPE_DEFAULT);
	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_B8G8R8A8_UNORM,
		static_cast<UINT64>(_frameRect.right) - _frameRect.left,
		static_cast<UINT64>(_frameRect.bottom) - _frameRect.top,
		1,
		1
	);
	HRESULT hr = App::GetInstance().GetDeviceResources().GetD3DDevice()->CreateCommittedResource(
		&heapDesc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(_output.put()));

	D3D11_TEXTURE2D_DESC desc1{};
	desc1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc1.Width = _frameRect.right - _frameRect.left;
	desc1.Height = _frameRect.bottom - _frameRect.top;
	desc1.Usage = D3D11_USAGE_DEFAULT;
	desc1.MipLevels = 1;
	desc1.ArraySize = 1;
	desc1.SampleDesc.Count = 1;
	desc1.SampleDesc.Quality = 0;
	desc1.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc1.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
	hr = _d3d11Device->CreateTexture2D(&desc1, nullptr, _d3d11Tex.put());
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 Texture2D 失败", hr));
		return false;
	}

	_d3d11Tex.try_as(_dxgiSurface);

	D3D11_RESOURCE_FLAGS flags{};
	hr = _d3d11On12Device->CreateWrappedResource(_output.get(), &flags, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE, IID_PPV_ARGS(_wrappedOutput.put()));

	SPDLOG_LOGGER_INFO(logger, "GDIFrameSource 初始化完成");
	return true;
}

FrameSourceBase::UpdateState GDIFrameSource::CaptureFrame() {
	HWND hwndSrc = App::GetInstance().GetHwndSrc();

	ID3D11Resource* res = _wrappedOutput.get();
	_d3d11On12Device->AcquireWrappedResources(&res, 1);

	HDC hdcDest;
	HRESULT hr = _dxgiSurface->GetDC(TRUE, &hdcDest);
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("从 Texture2D 获取 IDXGISurface1 失败", hr));
		return UpdateState::Error;
	}

	HDC hdcSrc = GetDCEx(hwndSrc, NULL, DCX_LOCKWINDOWUPDATE | DCX_WINDOW);
	if (!hdcSrc) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("GetDC 失败"));
		_dxgiSurface->ReleaseDC(nullptr);
		return UpdateState::Error;
	}

	if (!BitBlt(hdcDest, 0, 0, _frameRect.right-_frameRect.left, _frameRect.bottom-_frameRect.top,
		hdcSrc, _frameRect.left, _frameRect.top, SRCCOPY)
	) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("BitBlt 失败"));
	}

	ReleaseDC(hwndSrc, hdcSrc);
	_dxgiSurface->ReleaseDC(nullptr);

	_d3d11DC->CopyResource(_wrappedOutput.get(), _d3d11Tex.get());
	
	_d3d11On12Device->ReleaseWrappedResources(&res, 1);

	_d3d11DC->Flush();

	return UpdateState::NewFrame;
}
