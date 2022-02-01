#pragma once
#include "pch.h"
#include "Utils.h"


class NewRenderer {
public:
    ~NewRenderer();

	bool Initialize();

    void Render();

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

    winrt::com_ptr<IDXGIFactory4> _dxgiFactory;
    Utils::ScopedHandle _frameLatencyWaitableObject = NULL;

    // Pipeline objects.
    CD3DX12_VIEWPORT m_viewport;
    CD3DX12_RECT m_scissorRect;
    winrt::com_ptr<IDXGISwapChain3> _dxgiSwapChain;
    winrt::com_ptr<ID3D12Device> _d3dDevice;
    std::array<winrt::com_ptr<ID3D12Resource>, _FRAME_COUNT> _renderTargets;
    winrt::com_ptr<ID3D12CommandAllocator> _cmdAllocator;
    winrt::com_ptr<ID3D12CommandQueue> _cmdQueue;
    winrt::com_ptr<ID3D12RootSignature> _rootSignature;
    winrt::com_ptr<ID3D12DescriptorHeap> _rtvHeap;
    winrt::com_ptr<ID3D12PipelineState> m_pipelineState;
    winrt::com_ptr<ID3D12GraphicsCommandList> m_commandList;
    UINT _rtvDescriptorSize;

    // App resources.
    winrt::com_ptr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;

    // Synchronization objects.
    UINT m_frameIndex;
    Utils::ScopedHandle m_fenceEvent;
    winrt::com_ptr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue;
};

