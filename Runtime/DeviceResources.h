#pragma once
#include "pch.h"


class DeviceResources {
public:
	DeviceResources() = default;
	DeviceResources(const DeviceResources&) = delete;
	DeviceResources(DeviceResources&&) = default;

	~DeviceResources();

	bool Initialize();
};
