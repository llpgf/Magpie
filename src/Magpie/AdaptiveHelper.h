#pragma once

#include <cstdint>

namespace Magpie {

// 自適應放大的判斷結果
struct AdaptiveResult {
	RECT srcRect{};              // 精確的來源矩形（螢幕座標）
	uint32_t nativeWidth = 0;    // 遊戲原生渲染寬度
	uint32_t nativeHeight = 0;   // 遊戲原生渲染高度
	float scale = 0.0f;          // 有效放大倍率（目標 / 來源）
	bool pixelPerfect = false;   // 是否為 1:1 像素完美模式
};

// 偵測 RPG Maker MV/MZ 遊戲的原生解析度並算出精確來源矩形。
// manualWidth/manualHeight 非 0 時優先採用（設定檔手動指定）。
// 回傳 false 表示無法判斷，呼叫端應維持原本行為。
bool EvaluateAdaptive(
	HWND hWnd,
	uint32_t manualWidth,
	uint32_t manualHeight,
	AdaptiveResult& result
) noexcept;

// 依有效放大倍率挑選縮放模式，回傳模式索引；-1 表示維持使用者選擇。
int PickScalingModeByScale(float scale, int fallbackMode) noexcept;

}
