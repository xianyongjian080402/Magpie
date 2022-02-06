#pragma once
#include "pch.h"
#include "FrameSourceBase.h"


class GDIFrameSource : public FrameSourceBase {
public:
	GDIFrameSource() {};
	virtual ~GDIFrameSource() {}

	bool Initialize() override;

	winrt::com_ptr<ID3D12Resource> GetOutput() override {
		return nullptr;
	}

	UpdateState CaptureFrame() override;

	void ReleaseFrame() override {}

	bool HasRoundCornerInWin11() override {
		return false;
	}

	bool IsScreenCapture() override {
		return false;
	}

private:
	RECT _frameRect{};
	ComPtr<IDXGISurface1> _dxgiSurface;
	ComPtr<ID3D11Texture2D> _output;
};
