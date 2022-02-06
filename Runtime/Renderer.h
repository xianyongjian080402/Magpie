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

    bool _waitingForNextFrame = false;
};
