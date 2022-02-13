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
    bool _waitingForNextFrame = false;

    winrt::com_ptr<ID3D12RootSignature> _rootSignature;
    winrt::com_ptr<ID3D12PipelineState> _easuState;
    winrt::com_ptr<ID3D12Resource> _easuCB;
    winrt::com_ptr<ID3D12PipelineState> _rcasState;
    winrt::com_ptr<ID3D12Resource> _rcasCB;

    winrt::com_ptr<ID3D12Resource> _intermediateTex;
    winrt::com_ptr<ID3D12Resource> _outputTex;

    winrt::com_ptr<ID3D12DescriptorHeap> _descHeap;
    UINT _descriptorSize = 0;
};
