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

    winrt::com_ptr<ID3D12RootSignature> _fsrRasuRootSignature;
    winrt::com_ptr<ID3D12PipelineState> _fsrRasuState;
    winrt::com_ptr<ID3D12Resource> _constantBufferCS;

    winrt::com_ptr<ID3D12Resource> _outputTex;

    winrt::com_ptr<ID3D12DescriptorHeap> _descHeap;
    UINT _descriptorSize = 0;
};
