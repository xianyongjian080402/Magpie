#pragma once
#include "pch.h"
#include <CommonEffectDefines.h>


// {F606A124-CF11-4533-9C7E-62A0B89F0282}
DEFINE_GUID(GUID_MAGPIE_RAVU_LITE_R3_PASS1_SHADER,
	0xf606a124, 0xcf11, 0x4533, 0x9c, 0x7e, 0x62, 0xa0, 0xb8, 0x9f, 0x2, 0x82);

// {F270D6BC-DDA8-458D-8CAC-1B472DD2F7D2}
DEFINE_GUID(GUID_MAGPIE_RAVU_LITE_R3_PASS2_SHADER,
	0xf270d6bc, 0xdda8, 0x458d, 0x8c, 0xac, 0x1b, 0x47, 0x2d, 0xd2, 0xf7, 0xd2);

// {F39FDAAA-B830-4692-A80F-5929B25C3C6F}
DEFINE_GUID(GUID_MAGPIE_RAVU_ZOOM_R3_SHADER,
	0xf39fdaaa, 0xb830, 0x4692, 0xa8, 0xf, 0x59, 0x29, 0xb2, 0x5c, 0x3c, 0x6f);

// {07431047-1DCC-4EBC-84D5-6734D52DD774}
DEFINE_GUID(GUID_MAGPIE_RAVU_ZOOM_R3_WEIGHTS_SHADER,
	0x7431047, 0x1dcc, 0x4ebc, 0x84, 0xd5, 0x67, 0x34, 0xd5, 0x2d, 0xd7, 0x74);


// {59F45D37-88F0-454A-B32E-116E5889CD86}
DEFINE_GUID(CLSID_MAGPIE_RAVU_LITE_EFFECT,
	0x59f45d37, 0x88f0, 0x454a, 0xb3, 0x2e, 0x11, 0x6e, 0x58, 0x89, 0xcd, 0x86);

// {10FEB1FE-2AC5-4C8D-BF88-F6031FBB311D}
DEFINE_GUID(CLSID_MAGPIE_RAVU_ZOOM_EFFECT,
	0x10feb1fe, 0x2ac5, 0x4c8d, 0xbf, 0x88, 0xf6, 0x3, 0x1f, 0xbb, 0x31, 0x1d);


constexpr auto MAGPIE_RAVU_LITE_R3_PASS1_SHADER = L"shaders/RavuLiteR3Pass1Shader.cso";
constexpr auto MAGPIE_RAVU_LITE_R3_PASS2_SHADER = L"shaders/RavuLiteR3Pass2Shader.cso";
constexpr auto MAGPIE_RAVU_ZOOM_R3_SHADER = L"shaders/RavuZoomR3Shader.cso";
constexpr auto MAGPIE_RAVU_ZOOM_R3_WEIGHTS_SHADER = L"shaders/RavuZoomR3WeightsShader.cso";
