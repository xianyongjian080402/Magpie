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
		signature->GetBufferSize(), IID_PPV_ARGS(_fsrRasuRootSignature.put()));
	
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
	computePsoDesc.pRootSignature = _fsrRasuRootSignature.get();
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.get());

	hr = d3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(_fsrRasuState.put()));
	
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
			IID_PPV_ARGS(_constantBufferCS.put()));

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
		UpdateSubresources<1>(commandList.get(), _constantBufferCS.get(), constantBufferCSUpload.get(), 0, 0, 1, &computeCBData);

		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			_constantBufferCS.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		commandList->ResourceBarrier(1, &barrier);
	}

	{
		// CBV - SRV - UAV
		UINT descCount = 3;

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.NumDescriptors = 3;
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		hr = d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(_descHeap.put()));

		_descriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(_descHeap->GetCPUDescriptorHandleForHeapStart());

		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
		cbvDesc.BufferLocation = _constantBufferCS->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = 256;
		d3dDevice->CreateConstantBufferView(&cbvDesc, rtvHandle);
		rtvHandle.Offset(_descriptorSize);

		auto frame = App::GetInstance().GetFrameSource().GetOutput();
		d3dDevice->CreateShaderResourceView(frame.get(), nullptr, rtvHandle);
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
		_outputTex.get(),
		D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS
	);
	commandList->ResourceBarrier(1, &barrier);

	commandList->SetPipelineState(_fsrRasuState.get());
	commandList->SetComputeRootSignature(_fsrRasuRootSignature.get());

	ID3D12DescriptorHeap* descHeap = _descHeap.get();
	commandList->SetDescriptorHeaps(1, &descHeap);

	commandList->SetComputeRootDescriptorTable(0, _descHeap->GetGPUDescriptorHandleForHeapStart());

	const RECT& outputRect = App::GetInstance().GetHostWndRect();
	static const int threadGroupWorkRegionDim = 16;
	int dispatchX = (outputRect.right - outputRect.left + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	int dispatchY = (outputRect.bottom - outputRect.top + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	commandList->Dispatch(dispatchX, dispatchY, 1);

	CD3DX12_RESOURCE_BARRIER barrier1[2]{
		CD3DX12_RESOURCE_BARRIER::Transition(
			backBuffer.get(),
			D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_COPY_DEST
		),
		CD3DX12_RESOURCE_BARRIER::Transition(
			_outputTex.get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_SOURCE
		)
	};
	commandList->ResourceBarrier(2, barrier1);

	commandList->CopyResource(backBuffer.get(), _outputTex.get());

	dr.EndFrame(D3D12_RESOURCE_STATE_COPY_DEST);
}
