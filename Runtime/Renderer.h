#pragma once
#include "pch.h"
#include "Utils.h"


class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;

	bool Initialize();

    void Render();

private:
    bool _PopulateCommandList();

    struct Vertex {
        XMFLOAT3 position;
        XMFLOAT4 color;
    };

    CD3DX12_VIEWPORT m_viewport;
    CD3DX12_RECT m_scissorRect;
    winrt::com_ptr<ID3D12PipelineState> m_pipelineState;
    winrt::com_ptr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
    winrt::com_ptr<ID3D12RootSignature> _rootSignature;
    winrt::com_ptr<ID3D12DescriptorHeap> _rtvHeap;
    UINT _rtvDescriptorSize = 0;

    bool _waitingForNextFrame = false;
};
