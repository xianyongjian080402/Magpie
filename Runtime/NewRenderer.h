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

    int _GetFrameCount();

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
    // 交换链中最多有 3 个帧缓冲区
    ComPtr<ID3D12Resource> m_renderTargets[3];
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12CommandQueue> _d3dCmdQueue;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12DescriptorHeap> _rtvHeap;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    UINT m_rtvDescriptorSize;

    // App resources.
    ComPtr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;

    // Synchronization objects.
    UINT m_frameIndex;
    HANDLE m_fenceEvent;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue;
};

