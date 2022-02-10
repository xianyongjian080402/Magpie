#pragma once
#include "pch.h"
#include "Utils.h"
#include "FrameStatistics.h"


class DeviceResources {
public:
	DeviceResources() = default;
	DeviceResources(const DeviceResources&) = delete;
	DeviceResources(DeviceResources&&) = default;

	~DeviceResources();

	bool Initialize(D3D12_COMMAND_LIST_TYPE commandListType);

    winrt::com_ptr<ID3D12Device> GetD3DDevice() const noexcept {return _d3dDevice;}
    winrt::com_ptr<IDXGISwapChain3> GetSwapChain() const noexcept { return _swapChain; }
    winrt::com_ptr<IDXGIFactory4> GetDXGIFactory() const noexcept { return _dxgiFactory; }
    winrt::com_ptr<IDXGIAdapter1> GetGraphicsAdapter() const noexcept { return _graphicsAdapter; }
    D3D_FEATURE_LEVEL GetDeviceFeatureLevel() const noexcept { return _d3dFeatureLevel; }
    UINT GetBackBufferCount() const noexcept { return _backBufferCount; }
    const std::vector<winrt::com_ptr<ID3D12Resource>>& GetBackBuffers() const noexcept { return _backBuffers; }
    winrt::com_ptr<ID3D12Resource> GetCurrentBackBuffer() const noexcept { return _backBuffers[_backBufferIndex]; }
    UINT GetBackBufferIndex() const noexcept { return _backBufferIndex; }
    winrt::com_ptr<ID3D12CommandQueue> GetCommandQueue() const noexcept { return _commandQueue; }
    winrt::com_ptr<ID3D12CommandAllocator> GetCommandAllocator() const noexcept { return _commandAllocators[_backBufferIndex]; }
    winrt::com_ptr<ID3D12GraphicsCommandList> GetCommandList() const noexcept { return _commandList; }

    const FrameStatistics& GetFrameStatics() const noexcept { return _frameStatistics; }

    void BeginFrame();

    void EndFrame(D3D12_RESOURCE_STATES currentBackBufferState);

    bool WaitForGPU();

private:
    bool _CreateSwapChain();

    void _WaitForFence(UINT64 waitValue);

    winrt::com_ptr<IDXGIFactory4> _dxgiFactory;
    winrt::com_ptr<ID3D12Device> _d3dDevice;
    D3D_FEATURE_LEVEL _d3dFeatureLevel{};
    winrt::com_ptr<IDXGIAdapter1> _graphicsAdapter;

    winrt::com_ptr<IDXGISwapChain3> _swapChain;
    Utils::ScopedHandle _frameLatencyWaitableObject;
    std::vector<winrt::com_ptr<ID3D12Resource>> _backBuffers;

    std::vector<winrt::com_ptr<ID3D12CommandAllocator>> _commandAllocators;
    winrt::com_ptr<ID3D12CommandQueue> _commandQueue;
    winrt::com_ptr<ID3D12GraphicsCommandList> _commandList;

    // Synchronization objects.
    UINT _backBufferIndex = 0;
    UINT _backBufferCount = 0;
    Utils::ScopedHandle _fenceEvent;
    winrt::com_ptr<ID3D12Fence> _fence;
    std::vector<UINT64> _fenceValues;
    UINT64 _nextFenceValue = 1;

    UINT _curFrameIndex = 0;

    FrameStatistics _frameStatistics;

    bool _firstFrame = true;
};
