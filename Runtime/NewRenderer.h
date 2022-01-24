#pragma once
#include "pch.h"
#include "Utils.h"


class NewRenderer {
public:
	bool Initialize();

private:
    bool _CreateSwapChain();

    bool _LoadPipeline();
    bool _LoadAssets();
    bool _PopulateCommandList();
    bool _WaitForPreviousFrame();

    static constexpr int _FRAME_COUNT = 2;

    struct Vertex {
        XMFLOAT3 position;
        XMFLOAT4 color;
    };

    ComPtr<IDXGIFactory4> _dxgiFactory;
    Utils::ScopedHandle _frameLatencyWaitableObject = NULL;

    // Pipeline objects.
    D3D12_VIEWPORT m_viewport;
    D3D12_RECT m_scissorRect;
    ComPtr<IDXGISwapChain3> _dxgiSwapChain;
    ComPtr<ID3D12Device> _d3dDevice;
    std::array<ComPtr<ID3D12Resource>, _FRAME_COUNT> _renderTargets;
    ComPtr<ID3D12CommandAllocator> _cmdAllocator;
    ComPtr<ID3D12CommandQueue> _d3dCmdQueue;
    ComPtr<ID3D12RootSignature> _rootSignature;
    ComPtr<ID3D12DescriptorHeap> _rtvHeap;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    UINT _rtvDescriptorSize;

    // App resources.
    ComPtr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;

    // Synchronization objects.
    UINT m_frameIndex;
    HANDLE m_fenceEvent;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue;
};

