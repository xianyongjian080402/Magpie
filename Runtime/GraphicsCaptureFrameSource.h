#pragma once
#include "pch.h"
#include "FrameSourceBase.h"
#include <winrt/Windows.Graphics.Capture.h>
#include <Windows.Graphics.Capture.Interop.h>


namespace winrt {
using namespace Windows::Foundation;
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;
}


// 使用 Window Runtime 的 Windows.Graphics.Capture API 抓取窗口
// 见 https://docs.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture
class GraphicsCaptureFrameSource : public FrameSourceBase {
public:
	GraphicsCaptureFrameSource() {};
	virtual ~GraphicsCaptureFrameSource();

	bool Initialize() override;

	winrt::com_ptr<ID3D12Resource> GetOutput() override {
		return _output;
	}

	UpdateState CaptureFrame() override;

	bool HasRoundCornerInWin11() override {
		return true;
	}

	bool IsScreenCapture() override {
		return _isScreenCapture;
	}

private:
	bool _CaptureFromWindow(winrt::impl::com_ref<IGraphicsCaptureItemInterop> interop);

	bool _CaptureFromStyledWindow(winrt::impl::com_ref<IGraphicsCaptureItemInterop> interop);

	bool _CaptureFromMonitor(winrt::impl::com_ref<IGraphicsCaptureItemInterop> interop);

	void _OnFrameArrived(winrt::Direct3D11CaptureFramePool const&, winrt::IInspectable const&);

	LONG_PTR _srcWndStyle = 0;
	D3D11_BOX _frameBox{};

	bool _isScreenCapture = false;

	winrt::GraphicsCaptureItem _captureItem{ nullptr };
	winrt::Direct3D11CaptureFramePool _captureFramePool{ nullptr };
	winrt::GraphicsCaptureSession _captureSession{ nullptr };
	winrt::IDirect3DDevice _wrappedD3DDevice{ nullptr };
	winrt::Direct3D11CaptureFramePool::FrameArrived_revoker _frameArrived;

	// 用于线程同步
	CONDITION_VARIABLE _cv{};
	CRITICAL_SECTION _cs{};
	
	// DDP 线程使用的 D3D 设备
	winrt::com_ptr<ID3D11Device5> _wgcD3dDevice;
	winrt::com_ptr<ID3D11DeviceContext4> _wgcD3dDC;

	// d3d12 设备和 d3d11 设备的共享纹理
	winrt::com_ptr<ID3D12Resource> _sharedTex;
	winrt::com_ptr<ID3D11Resource> _wgcSharedTex;

	// 用于同步对共享纹理的访问
	winrt::com_ptr<ID3D12Fence> _fence;
	winrt::com_ptr<ID3D11Fence> _wgcFence;
	UINT64 _fenceValue = 0;
	std::atomic<UINT64> _wgcFenceValue = 0;

	winrt::com_ptr<ID3D12Resource> _output;
};
