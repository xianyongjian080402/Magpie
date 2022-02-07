#pragma once
#include "pch.h"
#include "FrameSourceBase.h"


class GDIFrameSource : public FrameSourceBase {
public:
	GDIFrameSource() {};
	virtual ~GDIFrameSource() {}

	bool Initialize() override;

	winrt::com_ptr<ID3D12Resource> GetOutput() override {
		return _output;
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

	winrt::com_ptr<ID3D11Texture2D> _d3d11Tex;
	winrt::com_ptr<IDXGISurface1> _dxgiSurface;

	winrt::com_ptr<ID3D12Resource> _output;
	winrt::com_ptr<ID3D11Resource> _wrappedOutput;

	winrt::com_ptr<ID3D11Device> _d3d11Device;
	winrt::com_ptr<ID3D11On12Device> _d3d11On12Device;
	winrt::com_ptr<ID3D11DeviceContext> _d3d11DC;
};
