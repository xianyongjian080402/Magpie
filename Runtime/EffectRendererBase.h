#pragma once
#include "pch.h"
#include "Renderable.h"
#include "D2DContext.h"
#include "AdaptiveSharpenEffect.h"
#include "Anime4KEffect.h"
#include "Anime4KDarkLinesEffect.h"
#include "Anime4KThinLinesEffect.h"
#include "Jinc2ScaleEffect.h"
#include "MitchellNetravaliScaleEffect.h"
#include "Lanczos6ScaleEffect.h"
#include "PixelScaleEffect.h"
#include "nlohmann/json.hpp"
#include <unordered_set>


// ȡ���ڲ�ͬ�Ĳ���ʽ�����в�ͬ��������룬�����������ͨ�õĲ���
// �̳д�����Ҫʵ�� SetInput��_PushAsOutputEffect��_GetOutputImg
// ���ڹ��캯���е��� _Init
class EffectRendererBase : public Renderable {
public:
	EffectRendererBase(
		D2DContext& d2dContext,
		const RECT& hostClient
	): Renderable(d2dContext), _hostClient(hostClient) {
	}

	virtual ~EffectRendererBase() {}

	// ���ɸ��ƣ������ƶ�
	EffectRendererBase(const EffectRendererBase&) = delete;
	EffectRendererBase(EffectRendererBase&&) = delete;

	const D2D1_RECT_F& GetOutputRect() const {
		return _outputRect;
	}

	virtual void SetInput(ComPtr<IUnknown> inputImg) = 0;

	void Render() {
		ComPtr<ID2D1Image> outputImg = _GetOutputImg();

		_d2dContext.GetD2DDC()->DrawImage(
			outputImg.Get(),
			Point2F(_outputRect.left, _outputRect.top)
		);
	}

	
protected:
	void _Init(const std::string_view& scaleModel,  const SIZE& srcSize) {
		_SetDestSize(srcSize);
		_ReadEffectsJson(scaleModel);

		// �������λ�ã�x �� y ����Ϊ�����������ʹ����ģ��
		float x = float((_hostClient.right - _hostClient.left - _outputSize.cx) / 2);
		float y = float((_hostClient.bottom - _hostClient.top - _outputSize.cy) / 2);
		_outputRect = RectF(x, y, x + _outputSize.cx, y + _outputSize.cy);
	}

	// �� effect ���ӵ� effect ����Ϊ���
	virtual void _PushAsOutputEffect(ComPtr<ID2D1Effect> effect) = 0;

	virtual ComPtr<ID2D1Image> _GetOutputImg() = 0;

private:
	void _ReadEffectsJson(const std::string_view& scaleModel) {
		const auto& models = nlohmann::json::parse(scaleModel);
		Debug::Assert(models.is_array(), L"json ��ʽ����");

		for (const auto &model : models) {
			Debug::Assert(model.is_object(), L"json ��ʽ����");

			const auto &effectType = model.value("effect", "");
			
			if (effectType == "scale") {
				const auto& subType = model.value("type", "");

				if (subType == "Anime4K") {
					_AddAnime4KEffect(model);
				} else if (subType == "Anime4KDarkLines") {
					_AddAnime4KDarkLinesEffect(model);
				} else if (subType == "Anime4KThinLines") {
					_AddAnime4KThinLinesEffect(model);
				} else if (subType == "jinc2") {
					_AddJinc2ScaleEffect(model);
				} else if (subType == "mitchell") {
					_AddMitchellNetravaliScaleEffect(model);
				} else if (subType == "HQBicubic") {
					_AddHQBicubicScaleEffect(model);
				} else if (subType == "lanczos6") {
					_AddLanczos6ScaleEffect(model);
				} else if (subType == "pixel") {
					_AddPixelScaleEffect(model);
				} else {
					Debug::Assert(false, L"δ֪�� scale effect");
				}
			} else if (effectType == "sharpen") {
				const auto& subType = model.value("type", "");

				if (subType == "adaptive") {
					_AddAdaptiveSharpenEffect(model);
				} else if (subType == "builtIn") {
					_AddBuiltInSharpenEffect(model);
				} else {
					Debug::Assert(false, L"δ֪�� sharpen effect");
				}
			} else {
				Debug::Assert(false, L"δ֪�� effect");
			}
		}
	}

	void _SetDestSize(SIZE value) {
		// �ƺ�������Ҫ���� tile
		/*if (value.cx > _outputSize.cx || value.cy > _outputSize.cy) {
			// ���� tile �Ĵ�С������ͼ��
			D2D1_RENDERING_CONTROLS rc{};
			_d2dContext.GetD2DDC()->GetRenderingControls(&rc);

			rc.tileSize.width = max(value.cx, _outputSize.cx);
			rc.tileSize.height = max(value.cy, _outputSize.cy);
			_d2dContext.GetD2DDC()->SetRenderingControls(rc);
		}*/

		_outputSize = value;
	}

	void _AddAdaptiveSharpenEffect(const nlohmann::json& props) {
		_CheckAndRegisterEffect(
			CLSID_MAGPIE_ADAPTIVE_SHARPEN_EFFECT,
			&AdaptiveSharpenEffect::Register
		);

		ComPtr<ID2D1Effect> adaptiveSharpenEffect = nullptr;
		Debug::ThrowIfComFailed(
			_d2dContext.GetD2DDC()->CreateEffect(CLSID_MAGPIE_ADAPTIVE_SHARPEN_EFFECT, &adaptiveSharpenEffect),
			L"���� Adaptive sharpen effect ʧ��"
		);

		// curveHeight ����
		auto it = props.find("curveHeight");
		if (it != props.end()) {
			const auto& value = *it;
			Debug::Assert(value.is_number(), L"�Ƿ��� curveHeight ����ֵ");

			float curveHeight = value.get<float>();
			Debug::Assert(
				curveHeight > 0,
				L"�Ƿ��� curveHeight ����ֵ"
			);

			Debug::ThrowIfComFailed(
				adaptiveSharpenEffect->SetValue(AdaptiveSharpenEffect::PROP_CURVE_HEIGHT, curveHeight),
				L"���� curveHeight ����ʧ��"
			);
		}

		// �滻 output effect
		_PushAsOutputEffect(adaptiveSharpenEffect);
	}

	void _AddBuiltInSharpenEffect(const nlohmann::json& props) {
		ComPtr<ID2D1Effect> d2dSharpenEffect = nullptr;
		Debug::ThrowIfComFailed(
			_d2dContext.GetD2DDC()->CreateEffect(CLSID_D2D1Sharpen, &d2dSharpenEffect),
			L"���� sharpen effect ʧ��"
		);

		// sharpness ����
		auto it = props.find("sharpness");
		if (it != props.end()) {
			const auto& value = *it;
			Debug::Assert(value.is_number(), L"�Ƿ��� sharpness ����ֵ");

			float sharpness = value.get<float>();
			Debug::Assert(
				sharpness >= 0 && sharpness <= 10,
				L"�Ƿ��� sharpness ����ֵ"
			);

			Debug::ThrowIfComFailed(
				d2dSharpenEffect->SetValue(D2D1_SHARPEN_PROP_SHARPNESS, sharpness),
				L"���� sharpness ����ʧ��"
			);
		}

		// threshold ����
		it = props.find("threshold");
		if (it != props.end()) {
			const auto& value = *it;
			Debug::Assert(value.is_number(), L"�Ƿ��� threshold ����ֵ");

			float threshold = value.get<float>();
			Debug::Assert(
				threshold >= 0 && threshold <= 1,
				L"�Ƿ��� threshold ����ֵ"
			);

			Debug::ThrowIfComFailed(
				d2dSharpenEffect->SetValue(D2D1_SHARPEN_PROP_THRESHOLD, threshold),
				L"���� threshold ����ʧ��"
			);
		}

		// �滻 output effect
		_PushAsOutputEffect(d2dSharpenEffect);
	}

	void _AddAnime4KEffect(const nlohmann::json& props) {
		_CheckAndRegisterEffect(
			CLSID_MAGPIE_ANIME4K_EFFECT,
			&Anime4KEffect::Register
		);

		ComPtr<ID2D1Effect> anime4KEffect = nullptr;
		Debug::ThrowIfComFailed(
			_d2dContext.GetD2DDC()->CreateEffect(CLSID_MAGPIE_ANIME4K_EFFECT, &anime4KEffect),
			L"���� Anime4K Effect ʧ��"
		);

		// curveHeight ����
		auto it = props.find("curveHeight");
		if (it != props.end()) {
			const auto& value = *it;
			Debug::Assert(value.is_number(), L"�Ƿ��� curveHeight ����ֵ");

			float curveHeight = value.get<float>();
			Debug::Assert(
				curveHeight >= 0,
				L"�Ƿ��� curveHeight ����ֵ"
			);

			Debug::ThrowIfComFailed(
				anime4KEffect->SetValue(Anime4KEffect::PROP_CURVE_HEIGHT, curveHeight),
				L"���� curveHeight ����ʧ��"
			);
		}

		// useDenoiseVersion ����
		it = props.find("useDenoiseVersion");
		if (it != props.end()) {
			const auto& val = *it;
			Debug::Assert(val.is_boolean(), L"�Ƿ��� useSharperVersion ����ֵ");

			Debug::ThrowIfComFailed(
				anime4KEffect->SetValue(Anime4KEffect::PROP_USE_DENOISE_VERSION, (BOOL)val.get<bool>()),
				L"���� useSharperVersion ����ʧ��"
			);
		}

		// ���ͼ��ĳ��Ϳ���Ϊ 2 ��
		_SetDestSize(SIZE{ _outputSize.cx * 2, _outputSize.cy * 2 });

		_PushAsOutputEffect(anime4KEffect);
	}

	void _AddAnime4KDarkLinesEffect(const nlohmann::json& props) {
		_CheckAndRegisterEffect(
			CLSID_MAGPIE_ANIME4K_DARKLINES_EFFECT,
			&Anime4KDarkLinesEffect::Register
		);

		ComPtr<ID2D1Effect> effect = nullptr;
		Debug::ThrowIfComFailed(
			_d2dContext.GetD2DDC()->CreateEffect(CLSID_MAGPIE_ANIME4K_DARKLINES_EFFECT, &effect),
			L"���� Anime4K Effect ʧ��"
		);


		_SetDestSize(SIZE{ _outputSize.cx, _outputSize.cy });

		_PushAsOutputEffect(effect);
	}

	void _AddAnime4KThinLinesEffect(const nlohmann::json& props) {
		_CheckAndRegisterEffect(
			CLSID_MAGPIE_ANIME4K_THINLINES_EFFECT,
			&Anime4KThinLinesEffect::Register
		);

		ComPtr<ID2D1Effect> effect = nullptr;
		Debug::ThrowIfComFailed(
			_d2dContext.GetD2DDC()->CreateEffect(CLSID_MAGPIE_ANIME4K_THINLINES_EFFECT, &effect),
			L"���� Anime4K Effect ʧ��"
		);

		// strength ����
		auto it = props.find("strength");
		if (it != props.end()) {
			const auto& value = *it;
			Debug::Assert(value.is_number(), L"�Ƿ��� strength ����ֵ");

			float strength = value.get<float>();
			Debug::Assert(
				strength > 0,
				L"�Ƿ��� strength ����ֵ"
			);

			Debug::ThrowIfComFailed(
				effect->SetValue(Anime4KThinLinesEffect::PROP_STRENGTH, strength),
				L"���� strength ����ʧ��"
			);
		}

		_SetDestSize(SIZE{ _outputSize.cx, _outputSize.cy });

		_PushAsOutputEffect(effect);
	}

	
	void _AddJinc2ScaleEffect(const nlohmann::json& props) {
		_CheckAndRegisterEffect(
			CLSID_MAGPIE_JINC2_SCALE_EFFECT,
			&Jinc2ScaleEffect::Register
		);

		ComPtr<ID2D1Effect> effect = nullptr;
		Debug::ThrowIfComFailed(
			_d2dContext.GetD2DDC()->CreateEffect(CLSID_MAGPIE_JINC2_SCALE_EFFECT, &effect),
			L"���� Anime4K Effect ʧ��"
		);
		

		// scale ����
		auto it = props.find("scale");
		if (it != props.end()) {
			const auto& scale = _ReadScaleProp(*it);
			
			Debug::ThrowIfComFailed(
				effect->SetValue(Jinc2ScaleEffect::PROP_SCALE, scale),
				L"���� scale ����ʧ��"
			);

			// ���� scale �����ͼ��ߴ�ı�
			_SetDestSize(SIZE{ lroundf(_outputSize.cx * scale.x), lroundf(_outputSize.cy * scale.y) });
		}

		// windowSinc ����
		it = props.find("windowSinc");
		if (it != props.end()) {
			const auto& value = *it;
			Debug::Assert(value.is_number(), L"�Ƿ��� windowSinc ����ֵ");

			float windowSinc = value.get<float>();
			Debug::Assert(
				windowSinc > 0,
				L"�Ƿ��� windowSinc ����ֵ"
			);

			Debug::ThrowIfComFailed(
				effect->SetValue(Jinc2ScaleEffect::PROP_WINDOW_SINC, windowSinc),
				L"���� windowSinc ����ʧ��"
			);
		}

		// sinc ����
		it = props.find("sinc");
		if (it != props.end()) {
			const auto& value = *it;
			Debug::Assert(value.is_number(), L"�Ƿ��� sinc ����ֵ");

			float sinc = value.get<float>();
			Debug::Assert(
				sinc > 0,
				L"�Ƿ��� sinc ����ֵ"
			);

			Debug::ThrowIfComFailed(
				effect->SetValue(Jinc2ScaleEffect::PROP_SINC, sinc),
				L"���� sinc ����ʧ��"
			);
		}

		// ARStrength ����
		it = props.find("ARStrength");
		if (it != props.end()) {
			const auto& value = *it;
			Debug::Assert(value.is_number(), L"�Ƿ��� ARStrength ����ֵ");

			float ARStrength = value.get<float>();
			Debug::Assert(
				ARStrength >= 0 && ARStrength <= 1,
				L"�Ƿ��� ARStrength ����ֵ"
			);

			Debug::ThrowIfComFailed(
				effect->SetValue(Jinc2ScaleEffect::PROP_AR_STRENGTH, ARStrength),
				L"���� ARStrength ����ʧ��"
			);
		}
		
		// �滻 output effect
		_PushAsOutputEffect(effect);
	}

	void _AddMitchellNetravaliScaleEffect(const nlohmann::json& props) {
		_CheckAndRegisterEffect(
			CLSID_MAGPIE_MITCHELL_NETRAVALI_SCALE_EFFECT, 
			&MitchellNetravaliScaleEffect::Register
		);

		ComPtr<ID2D1Effect> effect = nullptr;
		Debug::ThrowIfComFailed(
			_d2dContext.GetD2DDC()->CreateEffect(CLSID_MAGPIE_MITCHELL_NETRAVALI_SCALE_EFFECT, &effect),
			L"���� Mitchell-Netraval Scale Effect ʧ��"
		);

		// scale ����
		auto it = props.find("scale");
		if (it != props.end()) {
			const auto& scale = _ReadScaleProp(*it);

			Debug::ThrowIfComFailed(
				effect->SetValue(MitchellNetravaliScaleEffect::PROP_SCALE, scale),
				L"���� scale ����ʧ��"
			);

			// ���� scale �����ͼ��ߴ�ı�
			_SetDestSize(SIZE{ lroundf(_outputSize.cx * scale.x), lroundf(_outputSize.cy * scale.y) });
		}

		// useSharperVersion ����
		it = props.find("useSharperVersion");
		if (it != props.end()) {
			const auto& val = *it;
			Debug::Assert(val.is_boolean(), L"�Ƿ��� useSharperVersion ����ֵ");

			Debug::ThrowIfComFailed(
				effect->SetValue(MitchellNetravaliScaleEffect::PROP_USE_SHARPER_VERSION, (BOOL)val.get<bool>()),
				L"���� useSharperVersion ����ʧ��"
			);
		}

		// �滻 output effect
		_PushAsOutputEffect(effect);
	}

	// ���õ� HIGH_QUALITY_CUBIC �����㷨
	void _AddHQBicubicScaleEffect(const nlohmann::json& props) {
		ComPtr<ID2D1Effect> effect = nullptr;
		Debug::ThrowIfComFailed(
			_d2dContext.GetD2DDC()->CreateEffect(CLSID_D2D1Scale, &effect),
			L"���� Anime4K Effect ʧ��"
		);

		effect->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
		effect->SetValue(D2D1_SCALE_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);

		// scale ����
		auto it = props.find("scale");
		if (it != props.end()) {
			const auto& scale = _ReadScaleProp(*it);
			Debug::ThrowIfComFailed(
				effect->SetValue(D2D1_SCALE_PROP_SCALE, scale),
				L"���� scale ����ʧ��"
			);

			// ���� scale �����ͼ��ߴ�ı�
			_SetDestSize(SIZE{ lroundf(_outputSize.cx * scale.x), lroundf(_outputSize.cy * scale.y) });
		}

		// sharpness ����
		it = props.find("sharpness");
		if (it != props.end()) {
			const auto& value = *it;
			Debug::Assert(value.is_number(), L"�Ƿ��� sharpness ����ֵ");

			float sharpness = value.get<float>();
			Debug::Assert(
				sharpness >= 0 && sharpness <= 1,
				L"�Ƿ��� sharpness ����ֵ"
			);

			Debug::ThrowIfComFailed(
				effect->SetValue(D2D1_SCALE_PROP_SHARPNESS, sharpness),
				L"���� sharpness ����ʧ��"
			);
		}

		_PushAsOutputEffect(effect);
	}

	void _AddLanczos6ScaleEffect(const nlohmann::json& props) {
		_CheckAndRegisterEffect(
			CLSID_MAGPIE_LANCZOS6_SCALE_EFFECT,
			&Lanczos6ScaleEffect::Register
		);

		ComPtr<ID2D1Effect> effect = nullptr;
		Debug::ThrowIfComFailed(
			_d2dContext.GetD2DDC()->CreateEffect(CLSID_MAGPIE_LANCZOS6_SCALE_EFFECT, &effect),
			L"���� Lanczos6 Effect ʧ��"
		);

		// scale ����
		auto it = props.find("scale");
		if (it != props.end()) {
			const auto& scale = _ReadScaleProp(*it);

			Debug::ThrowIfComFailed(
				effect->SetValue(Lanczos6ScaleEffect::PROP_SCALE, scale),
				L"���� scale ����ʧ��"
			);

			// ���� scale �����ͼ��ߴ�ı�
			_SetDestSize(SIZE{ lroundf(_outputSize.cx * scale.x), lroundf(_outputSize.cy * scale.y) });
		}

		// ARStrength ����
		it = props.find("ARStrength");
		if (it != props.end()) {
			const auto& value = *it;
			Debug::Assert(value.is_number(), L"�Ƿ��� ARStrength ����ֵ");

			float ARStrength = value.get<float>();
			Debug::Assert(
				ARStrength >= 0 && ARStrength <= 1,
				L"�Ƿ��� ARStrength ����ֵ"
			);

			Debug::ThrowIfComFailed(
				effect->SetValue(Lanczos6ScaleEffect::PROP_AR_STRENGTH, ARStrength),
				L"���� ARStrengthc ����ʧ��"
			);
		}

		// �滻 output effect
		_PushAsOutputEffect(effect);
	}

	void _AddPixelScaleEffect(const nlohmann::json& props) {
		_CheckAndRegisterEffect(
			CLSID_MAGPIE_PIXEL_SCALE_EFFECT,
			&PixelScaleEffect::Register
		);

		ComPtr<ID2D1Effect> effect = nullptr;
		Debug::ThrowIfComFailed(
			_d2dContext.GetD2DDC()->CreateEffect(CLSID_MAGPIE_PIXEL_SCALE_EFFECT, &effect),
			L"���� Pixel Scale Effect ʧ��"
		);

		// scale ����
		auto it = props.find("scale");
		if (it != props.end()) {
			Debug::Assert(it->is_number_integer(), L"�Ƿ���Scale����ֵ");
			int scale = *it;

			Debug::Assert(scale > 0, L"�Ƿ���Scale����ֵ");
			Debug::ThrowIfComFailed(
				effect->SetValue(PixelScaleEffect::PROP_SCALE, scale),
				L"���� scale ����ʧ��"
			);

			// ���� scale �����ͼ��ߴ�ı�
			_SetDestSize(SIZE{ _outputSize.cx * scale, _outputSize.cy * scale });
		}

		// �滻 output effect
		_PushAsOutputEffect(effect);
	}

	D2D1_VECTOR_2F _ReadScaleProp(const nlohmann::json& prop) {
		Debug::Assert(
			prop.is_array() && prop.size() == 2
			&& prop[0].is_number() && prop[1].is_number(),
			L"��ȡ scale ����ʧ��"
		);

		D2D1_VECTOR_2F scale{ prop[0], prop[1] };
		Debug::Assert(
			scale.x >= 0 && scale.y >= 0,
			L"scale ���Ե�ֵ�Ƿ�"
		);

		if (scale.x == 0 || scale.y == 0) {
			// ���ͼ�������Ļ
			scale.x = min(
				float(_hostClient.right - _hostClient.left) / _outputSize.cx,
				float(_hostClient.bottom - _hostClient.top) / _outputSize.cy
			);
			scale.y = scale.x;
		}

		return scale;
	}
	

	// ��Ҫʱע�� effect
	void _CheckAndRegisterEffect(const GUID& effectID, std::function<HRESULT(ID2D1Factory1*)> registerFunc) {
		if (_registeredEffects.find(effectID) == _registeredEffects.end()) {
			// δע��
			Debug::ThrowIfComFailed(
				registerFunc(_d2dContext.GetD2DFactory()),
				L"ע�� Effect ʧ��"
			);
			
			_registeredEffects.insert(effectID);
		}
	}

private:
	// ���ͼ��ߴ�
	SIZE _outputSize{};
	D2D1_RECT_F _outputRect{};

	const RECT& _hostClient;

	// �洢��ע��� effect �� GUID
	std::unordered_set<GUID> _registeredEffects;
};