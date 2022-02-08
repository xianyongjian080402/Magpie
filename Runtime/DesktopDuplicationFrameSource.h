#pragma once
#include "FrameSourceBase.h"


// 使用 Desktop Duplication API 捕获窗口
// 在单独的线程中接收屏幕帧以避免丢帧
class DesktopDuplicationFrameSource : public FrameSourceBase {
public:
	DesktopDuplicationFrameSource() {};
	virtual ~DesktopDuplicationFrameSource();

	bool Initialize() override;

	winrt::com_ptr<ID3D12Resource> GetOutput() override {
		return _output;
	}

	UpdateState CaptureFrame() override;

	void ReleaseFrame() override {}

	bool HasRoundCornerInWin11() override {
		return true;
	}

	bool IsScreenCapture() override {
		return true;
	}

private:
	bool _InitializeDdpD3D();

	static DWORD WINAPI _DDPThreadProc(LPVOID lpThreadParameter);

	winrt::com_ptr<ID3D12Resource> _output;
	winrt::com_ptr<IDXGIOutputDuplication> _outputDup;
	std::vector<BYTE> _dupMetaData;

	HANDLE _hDDPThread = NULL;
	std::atomic<bool> _exiting = false;

	// DDP 线程使用的 D3D 设备
	winrt::com_ptr<ID3D11Device5> _ddpD3dDevice;
	winrt::com_ptr<ID3D11DeviceContext4> _ddpD3dDC;

	// d3d12 设备和 d3d11 设备的共享纹理
	winrt::com_ptr<ID3D12Resource> _sharedTex;
	winrt::com_ptr<ID3D11Resource> _ddpSharedTex;

	// 用于同步对共享纹理的访问
	winrt::com_ptr<ID3D12Fence> _fence;
	winrt::com_ptr<ID3D11Fence> _ddpFence;
	UINT64 _fenceValue = 0;
	std::atomic<UINT64> _ddpFenceValue = 0;

	RECT _srcClientInMonitor{};
	D3D11_BOX _frameInMonitor{};
};

