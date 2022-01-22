#include "pch.h"
#include "NewRenderer.h"
#include "StrUtils.h"
#include "App.h"


extern std::shared_ptr<spdlog::logger> logger;

bool NewRenderer::Initialize() {
	if (!_LoadPipeline()) {
		return false;
	}

	if (!_LoadAssets()) {
		return false;
	}

	return true;
}

static inline void LogAdapter(const DXGI_ADAPTER_DESC1& adapterDesc) {
	SPDLOG_LOGGER_INFO(logger, fmt::format("当前图形适配器：\n\tVendorId：{:#x}\n\tDeviceId：{:#x}\n\t描述：{}",
		adapterDesc.VendorId, adapterDesc.DeviceId, StrUtils::UTF16ToUTF8(adapterDesc.Description)));
}

static ComPtr<IDXGIAdapter1> ObtainGraphicsAdapter(IDXGIFactory4* dxgiFactory, int adapterIdx) {
	ComPtr<IDXGIAdapter1> adapter;
	
	if (adapterIdx >= 0) {
		HRESULT hr = dxgiFactory->EnumAdapters1(adapterIdx, adapter.ReleaseAndGetAddressOf());
		if (SUCCEEDED(hr)) {
			DXGI_ADAPTER_DESC1 desc;
			HRESULT hr = adapter->GetDesc1(&desc);
			if (SUCCEEDED(hr)) {
				// 测试此适配器是否支持 D3D12
				hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr);
				if (SUCCEEDED(hr)) {
					LogAdapter(desc);
					return adapter;
				}
			}
		}
	}

	// 指定 GPU 失败，枚举查找第一个支持 D3D12 的图形适配器

	for (UINT adapterIndex = 0;
			SUCCEEDED(dxgiFactory->EnumAdapters1(adapterIndex,
				adapter.ReleaseAndGetAddressOf()));
			adapterIndex++
	) {
		DXGI_ADAPTER_DESC1 desc;
		HRESULT hr = adapter->GetDesc1(&desc);
		if (FAILED(hr)) {
			continue;
		}

		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
			continue;
		}
		
		// 测试此适配器是否支持 D3D12
		hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr);
		if (SUCCEEDED(hr)) {
			LogAdapter(desc);
			return adapter;
		}
	}

	SPDLOG_LOGGER_INFO(logger, "未找到可用图形适配器");

	// 回落到 Basic Render Driver Adapter（WARP）
	// https://docs.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp
	HRESULT hr = dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 WARP 设备失败", hr));
		return nullptr;
	}

	return adapter;
}

bool NewRenderer::_LoadPipeline() {
#ifdef _DEBUG
	// 启用 D3D12 调试层
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
			SPDLOG_LOGGER_INFO(logger, "已启用 D3D12 调试层");
			debugController->EnableDebugLayer();
		}
	}
#endif // _DEBUG

	ComPtr<IDXGIFactory4> factory;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateDXGIFactory1 失败", hr));
		return false;
	}

	ComPtr<IDXGIAdapter1> adapter = ObtainGraphicsAdapter(factory.Get(), App::GetInstance().GetAdapterIdx());
	if (!adapter) {
		SPDLOG_LOGGER_ERROR(logger, "没有可用的图形适配器");
		return false;
	}

	hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 D3D12 设备失败", hr));
		return false;
	}

	return true;
}

bool NewRenderer::_LoadAssets() {
	return false;
}

bool NewRenderer::_PopulateCommandList() {
	return false;
}

bool NewRenderer::_WaitForPreviousFrame() {
	return false;
}
