#include "pch.h"
#include "Renderer.h"
#include "StrUtils.h"
#include "App.h"
#include "DeviceResources.h"
#include "FrameSourceBase.h"


extern std::shared_ptr<spdlog::logger> logger;


bool Renderer::Initialize() {
	
	return true;
}

void Renderer::Render() {
	DeviceResources& dr = App::GetInstance().GetDeviceResources();

	if (!_waitingForNextFrame) {
		dr.WaitForSwapChain();
	}

	dr.PrepareForCurrentFrame();
	App::GetInstance().GetFrameSource().CaptureFrame();

	_PopulateCommandList();
	dr.Present(D3D12_RESOURCE_STATE_COPY_DEST);
}

bool Renderer::_PopulateCommandList() {
	DeviceResources& dr = App::GetInstance().GetDeviceResources();
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList = dr.GetCommandList();

	auto frame = App::GetInstance().GetFrameSource().GetOutput();
	const RECT& frameRect = App::GetInstance().GetFrameSource().GetSrcFrameRect();
	SIZE frameSize{ frameRect.right - frameRect.left,frameRect.bottom - frameRect.top };

	auto backBuffer = dr.GetCurrentBackBuffer();
	const RECT& hostRect = App::GetInstance().GetHostWndRect();
	SIZE backBufferSize{ hostRect.right - hostRect.left,hostRect.bottom - hostRect.top };

	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		backBuffer.get(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_COPY_DEST
	);
	commandList->ResourceBarrier(1, &barrier);

	CD3DX12_TEXTURE_COPY_LOCATION src(frame.get(), 0);
	CD3DX12_TEXTURE_COPY_LOCATION dest(backBuffer.get(), 0);
	CD3DX12_BOX box(0, 0, std::min(backBufferSize.cx, frameSize.cx), std::min(backBufferSize.cy, frameSize.cy));
	commandList->CopyTextureRegion(
		&dest,
		std::max(0L, (backBufferSize.cx - frameSize.cx) / 2),
		std::max(0L, (backBufferSize.cy - frameSize.cy) / 2),
		0,
		&src,
		&box
	);

	return true;
}
