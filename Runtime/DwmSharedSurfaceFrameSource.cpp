#include "pch.h"
#include "DwmSharedSurfaceFrameSource.h"
#include "App.h"
#include "DeviceResources.h"


extern std::shared_ptr<spdlog::logger> logger;


bool DwmSharedSurfaceFrameSource::Initialize() {
	_dwmGetDxSharedSurface = (_DwmGetDxSharedSurfaceFunc*)GetProcAddress(
		GetModuleHandle(L"user32.dll"), "DwmGetDxSharedSurface");

	if (!_dwmGetDxSharedSurface) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("获取函数 DwmGetDxSharedSurface 地址失败"));
		return false;
	}

	if (!_UpdateSrcFrameRect()) {
		SPDLOG_LOGGER_ERROR(logger, "UpdateSrcFrameRect 失败");
		return false;
	}
	
	HWND hwndSrc = App::GetInstance().GetHwndSrc();

	double a, bx, by;
	if (!_GetMapToOriginDPI(hwndSrc, a, bx, by)) {
		SPDLOG_LOGGER_ERROR(logger, "_GetMapToOriginDPI 失败");
		App::GetInstance().SetErrorMsg(ErrorMessages::FAILED_TO_CAPTURE);
		return false;
	}

	SPDLOG_LOGGER_INFO(logger, fmt::format("源窗口 DPI 缩放为 {}", 1 / a));

	RECT frameRect = {
		std::lround(_srcFrameRect.left * a + bx),
		std::lround(_srcFrameRect.top * a + by),
		std::lround(_srcFrameRect.right * a + bx),
		std::lround(_srcFrameRect.bottom * a + by)
	};
	if (frameRect.left < 0 || frameRect.top < 0 || frameRect.right < 0 
		|| frameRect.bottom < 0 || frameRect.right - frameRect.left <= 0
		|| frameRect.bottom - frameRect.top <= 0
	) {
		SPDLOG_LOGGER_ERROR(logger, "裁剪失败");
		App::GetInstance().SetErrorMsg(ErrorMessages::FAILED_TO_CROP);
		return false;
	}

	_frameInWnd = {
		(UINT)frameRect.left,
		(UINT)frameRect.top,
		0,
		(UINT)frameRect.right,
		(UINT)frameRect.bottom,
		1
	};

	CD3DX12_HEAP_PROPERTIES heapDesc(D3D12_HEAP_TYPE_DEFAULT);
	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_B8G8R8A8_UNORM,
		static_cast<UINT64>(frameRect.right) - frameRect.left,
		static_cast<UINT64>(frameRect.bottom) - frameRect.top
	);
	HRESULT hr = App::GetInstance().GetDeviceResources().GetD3DDevice()->CreateCommittedResource(
		&heapDesc, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(_output.put()));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_CRITICAL(logger, MakeComErrorMsg("创建 2D 纹理失败", hr));
		return false;
	}

	SPDLOG_LOGGER_INFO(logger, "DwmSharedSurfaceFrameSource 初始化完成");
	return true;
}

FrameSourceBase::UpdateState DwmSharedSurfaceFrameSource::CaptureFrame() {
	_sharedTexture = nullptr;

	HANDLE sharedTextureHandle = NULL;
	if (!_dwmGetDxSharedSurface(App::GetInstance().GetHwndSrc(),
		&sharedTextureHandle, nullptr, nullptr, nullptr, nullptr)
		|| !sharedTextureHandle
	) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("DwmGetDxSharedSurface 失败"));
		return UpdateState::Error;
	}

	const DeviceResources& dr = App::GetInstance().GetDeviceResources();

	HRESULT hr = dr.GetD3DDevice()->OpenSharedHandle(sharedTextureHandle, IID_PPV_ARGS(&_sharedTexture));

	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("OpenSharedHandle 失败", hr));
		return UpdateState::Error;
	}

	auto commandList = dr.GetCommandList();

	CD3DX12_RESOURCE_BARRIER barriers[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(
			_sharedTexture.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE, 0),
		CD3DX12_RESOURCE_BARRIER::Transition(
			_output.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, 0)
	};
	commandList->ResourceBarrier((UINT)std::size(barriers), barriers);

	CD3DX12_TEXTURE_COPY_LOCATION src(_sharedTexture.get(), 0);
	CD3DX12_TEXTURE_COPY_LOCATION dest(_output.get(), 0);
	commandList->CopyTextureRegion(&dest, 0, 0, 0, &src, &_frameInWnd);

	CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			_output.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
	commandList->ResourceBarrier(1, &barrier);
	
	return UpdateState::NewFrame;
}

void DwmSharedSurfaceFrameSource::ReleaseFrame() {
	_sharedTexture = nullptr;
}
