#include "pch.h"
#include "Renderer.h"
#include "StrUtils.h"
#include "App.h"
#include "DeviceResources.h"
#include "FrameSourceBase.h"


extern std::shared_ptr<spdlog::logger> logger;


bool Renderer::Initialize() {
	DeviceResources& dr = App::GetInstance().GetDeviceResources();
	auto d3dDevice = dr.GetD3DDevice();

	HRESULT hr{};

	const RECT& outputRect = App::GetInstance().GetHostWndRect();
	SIZE outputSize = { outputRect.right - outputRect.left,outputRect.bottom - outputRect.top };

	const RECT& frameRect = App::GetInstance().GetFrameSource().GetSrcFrameRect();
	SIZE frameSize = { frameRect.right - frameRect.left,frameRect.bottom - frameRect.top };

	if ((double)outputSize.cx / outputSize.cy > (double)frameSize.cx / frameSize.cy) {
		outputSize.cx = std::lround(outputSize.cy * frameSize.cx / (double)frameSize.cy);
	} else {
		outputSize.cy = std::lround(outputSize.cx * frameSize.cy / (double)frameSize.cx);
	}

	{
		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_B8G8R8A8_UNORM,
			outputSize.cx,
			outputSize.cy,
			1,
			1,
			1,
			0,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
		);
		hr = d3dDevice->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&resDesc,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(_intermediateTex.put())
		);

		hr = d3dDevice->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&resDesc,
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			nullptr,
			IID_PPV_ARGS(_outputTex.put())
		);
	}
	

	D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData{};
	featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

	hr = d3dDevice->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData));
	if (FAILED(hr)) {
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}

	CD3DX12_DESCRIPTOR_RANGE1 ranges[3]{};
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
	ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

	CD3DX12_ROOT_PARAMETER1 rootParameters[2]{};
	rootParameters[0].InitAsDescriptorTable(3, ranges);
	
	D3D12_STATIC_SAMPLER_DESC samplerDesc{};
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	samplerDesc.ShaderRegister = 0;
	samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc{};
	computeRootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &samplerDesc);

	winrt::com_ptr<ID3DBlob> signature;
	winrt::com_ptr<ID3DBlob> error;
	hr = D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc,
		featureData.HighestVersion, signature.put(), error.put());
	hr = d3dDevice->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(_rootSignature.put()));
	
	winrt::com_ptr<ID3DBlob> computeShader;

#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = 0;
#endif

	hr = D3DCompileFromFile(L"effects\\FSR_EASU.hlsl", nullptr, nullptr, "main",
		"cs_5_1", compileFlags, 0, computeShader.put(), error.put());
	if (FAILED(hr)) {
		auto s = (const char*)error->GetBufferPointer();
		SPDLOG_LOGGER_ERROR(logger, fmt::format("编译着色器失败{}", (const char*)error->GetBufferPointer()));
		return false;
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc{};
	computePsoDesc.pRootSignature = _rootSignature.get();
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.get());

	hr = d3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(_easuState.put()));

	hr = D3DCompileFromFile(L"effects\\FSR_RCAS.hlsl", nullptr, nullptr, "main",
		"cs_5_1", compileFlags, 0, computeShader.put(), error.put());
	if (FAILED(hr)) {
		auto s = (const char*)error->GetBufferPointer();
		SPDLOG_LOGGER_ERROR(logger, fmt::format("编译着色器失败{}", (const char*)error->GetBufferPointer()));
		return false;
	}

	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.get());
	hr = d3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(_rcasState.put()));

	
	{
		winrt::com_ptr<ID3D12Resource> constantBufferCSUpload;

		const UINT bufferSize = 256;

		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
		hr = d3dDevice->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&resDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(_easuCB.put()));

		heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		hr = d3dDevice->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(constantBufferCSUpload.put()));

		dr.SafeReleaseFrameResource(constantBufferCSUpload);

		const RECT& frameRect = App::GetInstance().GetFrameSource().GetSrcFrameRect();
		
		FLOAT constantBufferCS[64]{
			(FLOAT)frameRect.right - frameRect.left,
			(FLOAT)frameRect.bottom - frameRect.top,
			(FLOAT)outputSize.cx,
			(FLOAT)outputSize.cy
		};

		D3D12_SUBRESOURCE_DATA computeCBData = {};
		computeCBData.pData = &constantBufferCS;
		computeCBData.RowPitch = bufferSize;
		computeCBData.SlicePitch = computeCBData.RowPitch;

		winrt::com_ptr<ID3D12GraphicsCommandList> commandList = dr.GetCommandList();
		UpdateSubresources<1>(commandList.get(), _easuCB.get(), constantBufferCSUpload.get(), 0, 0, 1, &computeCBData);

		heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		hr = d3dDevice->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&resDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(_rcasCB.put()));

		heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		hr = d3dDevice->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(constantBufferCSUpload.put()));

		dr.SafeReleaseFrameResource(constantBufferCSUpload);

		constantBufferCS[2] = 0.87f;
		UpdateSubresources<1>(commandList.get(), _rcasCB.get(), constantBufferCSUpload.get(), 0, 0, 1, &computeCBData);

		CD3DX12_RESOURCE_BARRIER barrier[2] = {
			CD3DX12_RESOURCE_BARRIER::Transition(_easuCB.get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
			CD3DX12_RESOURCE_BARRIER::Transition(_rcasCB.get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
		};
		commandList->ResourceBarrier(2, barrier);
	}

	{
		// CBV - SRV - UAV

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.NumDescriptors = 6;
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		hr = d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(_descHeap.put()));

		_descriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(_descHeap->GetCPUDescriptorHandleForHeapStart());

		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
		cbvDesc.BufferLocation = _easuCB->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = 256;
		d3dDevice->CreateConstantBufferView(&cbvDesc, rtvHandle);
		rtvHandle.Offset(_descriptorSize);

		auto frame = App::GetInstance().GetFrameSource().GetOutput();
		d3dDevice->CreateShaderResourceView(frame.get(), nullptr, rtvHandle);
		rtvHandle.Offset(_descriptorSize);

		d3dDevice->CreateUnorderedAccessView(_intermediateTex.get(), nullptr, nullptr, rtvHandle);
		rtvHandle.Offset(_descriptorSize);

		cbvDesc.BufferLocation = _rcasCB->GetGPUVirtualAddress();
		d3dDevice->CreateConstantBufferView(&cbvDesc, rtvHandle);
		rtvHandle.Offset(_descriptorSize);

		d3dDevice->CreateShaderResourceView(_intermediateTex.get(), nullptr, rtvHandle);
		rtvHandle.Offset(_descriptorSize);

		d3dDevice->CreateUnorderedAccessView(_outputTex.get(), nullptr, nullptr, rtvHandle);
	}
	
	return true;
}

void Renderer::Render() {
	DeviceResources& dr = App::GetInstance().GetDeviceResources();

	if (!_waitingForNextFrame) {
		dr.BeginFrame();
	}

	winrt::com_ptr<ID3D12GraphicsCommandList> commandList = dr.GetCommandList();

	App::GetInstance().GetFrameSource().CaptureFrame();

	auto backBuffer = dr.GetCurrentBackBuffer();

	CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		_intermediateTex.get(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS
	);
	commandList->ResourceBarrier(1, &barrier);

	ID3D12DescriptorHeap* descHeap = _descHeap.get();
	commandList->SetDescriptorHeaps(1, &descHeap);

	commandList->SetPipelineState(_easuState.get());
	commandList->SetComputeRootSignature(_rootSignature.get());
	commandList->SetComputeRootDescriptorTable(0, _descHeap->GetGPUDescriptorHandleForHeapStart());

	const RECT& outputRect = App::GetInstance().GetHostWndRect();
	SIZE outputSize = { outputRect.right - outputRect.left,outputRect.bottom - outputRect.top };

	static const int threadGroupWorkRegionDim = 16;
	int dispatchX = (outputSize.cx + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	int dispatchY = (outputSize.cy + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	commandList->Dispatch(dispatchX, dispatchY, 1);

	CD3DX12_RESOURCE_BARRIER barrier1[2]{
		CD3DX12_RESOURCE_BARRIER::Transition(
			_intermediateTex.get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
		),
		CD3DX12_RESOURCE_BARRIER::Transition(
			_outputTex.get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS
		)
	};
	commandList->ResourceBarrier(2, barrier1);

	commandList->SetPipelineState(_rcasState.get());
	commandList->SetComputeRootSignature(_rootSignature.get());
	commandList->SetComputeRootDescriptorTable(0, CD3DX12_GPU_DESCRIPTOR_HANDLE(_descHeap->GetGPUDescriptorHandleForHeapStart(), 3, _descriptorSize));

	commandList->Dispatch(dispatchX, dispatchY, 1);

	barrier1[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		backBuffer.get(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_COPY_DEST
	);
	barrier1[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		_outputTex.get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_SOURCE
	);
	commandList->ResourceBarrier(2, barrier1);

	D3D12_RESOURCE_DESC desc = _outputTex->GetDesc();

	CD3DX12_TEXTURE_COPY_LOCATION dest(backBuffer.get(), 0);
	CD3DX12_TEXTURE_COPY_LOCATION src(_outputTex.get(), 0);
	CD3DX12_BOX box(0, 0, desc.Width, desc.Height);
	commandList->CopyTextureRegion(
		&dest,
		(outputSize.cx - desc.Width) / 2,
		(outputSize.cy - desc.Height) / 2,
		0,
		&src,
		&box
	);

	dr.EndFrame(D3D12_RESOURCE_STATE_COPY_DEST);
}
