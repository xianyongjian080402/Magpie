#pragma once
#include "pch.h"
#include "FrameSourceBase.h"


class DwmSharedSurfaceFrameSource : public FrameSourceBase {
public:
	DwmSharedSurfaceFrameSource() {}
	virtual ~DwmSharedSurfaceFrameSource() {}

	bool Initialize() override;

	winrt::com_ptr<ID3D12Resource> GetOutput() override {
		return _output;
	}

	UpdateState CaptureFrame() override;

	void ReleaseFrame() override;

	bool HasRoundCornerInWin11() override {
		return false;
	}

	bool IsScreenCapture() override {
		return false;
	}

private:
	using _DwmGetDxSharedSurfaceFunc = bool(
		HWND hWnd,
		HANDLE* phSurface,
		LUID* pAdapterLuid,
		ULONG* pFmtWindow,
		ULONG* pPresentFlags,
		ULONGLONG* pWin32KUpdateId
	);
	_DwmGetDxSharedSurfaceFunc *_dwmGetDxSharedSurface = nullptr;

	D3D12_BOX _frameInWnd{};
	winrt::com_ptr<ID3D12Resource> _sharedTexture;
	winrt::com_ptr<ID3D12Resource> _output;
};

