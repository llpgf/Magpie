#include "pch.h"
#include "AdaptiveHelper.h"
#include "Logger.h"
#include "Win32Helper.h"
#include <rapidjson/document.h>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace Magpie {

// 讀取 RPG Maker MV/MZ 的 data\System.json，取得遊戲真正的渲染解析度
static bool ReadRPGMakerNativeSize(
	const std::filesystem::path& exeDir, uint32_t& w, uint32_t& h
) noexcept {
	const std::filesystem::path candidates[] = {
		exeDir / L"data" / L"System.json",
		exeDir / L"www" / L"data" / L"System.json",
	};

	for (const std::filesystem::path& path : candidates) {
		std::error_code ec;
		if (!std::filesystem::exists(path, ec)) {
			continue;
		}

		std::ifstream ifs(path, std::ios::binary);
		if (!ifs) {
			continue;
		}
		std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
		if (text.empty()) {
			continue;
		}

		rapidjson::Document doc;
		doc.ParseInsitu(text.data());
		if (doc.HasParseError() || !doc.IsObject()) {
			continue;
		}

		auto advancedIt = doc.FindMember("advanced");
		if (advancedIt == doc.MemberEnd() || !advancedIt->value.IsObject()) {
			continue;
		}
		const auto& advanced = advancedIt->value;

		auto wIt = advanced.FindMember("screenWidth");
		auto hIt = advanced.FindMember("screenHeight");
		if (wIt == advanced.MemberEnd() || hIt == advanced.MemberEnd()
			|| !wIt->value.IsInt() || !hIt->value.IsInt()) {
			continue;
		}

		const int sw = wIt->value.GetInt();
		const int sh = hIt->value.GetInt();
		if (sw < 64 || sh < 64 || sw > 16384 || sh > 16384) {
			continue;
		}

		w = (uint32_t)sw;
		h = (uint32_t)sh;
		return true;
	}

	return false;
}

// 遊戲資料夾是否安裝了 PixelPerfect1to1 插件（表示畫面是 1:1 像素完美顯示）
static bool HasPixelPerfectPlugin(const std::filesystem::path& exeDir) noexcept {
	std::error_code ec;
	return std::filesystem::exists(exeDir / L"js" / L"plugins" / L"PixelPerfect1to1.js", ec)
		|| std::filesystem::exists(exeDir / L"www" / L"js" / L"plugins" / L"PixelPerfect1to1.js", ec);
}

bool EvaluateAdaptive(
	HWND hWnd,
	uint32_t manualWidth,
	uint32_t manualHeight,
	AdaptiveResult& result
) noexcept {
	const std::filesystem::path exePath = Win32Helper::GetWindowExePath(hWnd);
	if (exePath.empty()) {
		Logger::Get().Info("自適應：無法取得執行檔路徑");
		return false;
	}
	const std::filesystem::path exeDir = exePath.parent_path();

	const bool pluginInstalled = HasPixelPerfectPlugin(exeDir);

	uint32_t W = manualWidth;
	uint32_t H = manualHeight;
	if (W == 0 || H == 0) {
		if (!ReadRPGMakerNativeSize(exeDir, W, H)) {
			Logger::Get().Info(fmt::format("自適應：{} 不是 RPG Maker MV/MZ 遊戲（找不到 data\\System.json），維持原行為",
				StrHelper::UTF16ToUTF8(exeDir.filename().native())));
			return false;
		}
	}

	RECT clientRect;
	if (!Win32Helper::GetClientScreenRect(hWnd, clientRect)) {
		Logger::Get().Info("自適應：GetClientScreenRect 失敗");
		return false;
	}

	// ------------------------------------------------------------------
	// 核心：把來源視窗外部調整成「原生解析度 1:1」
	//
	// 引擎的顯示倍率為 realScale = min(客戶區寬 / W, 客戶區高 / H)（MZ 的拉伸規則），
	// 只要讓客戶區的「實體像素」等於 W x H，realScale 就會等於 1/dpr，
	// 畫布剛好一個像素對一個螢幕像素。這是從外部用 SetWindowPos 完成的，
	// 不需要修改遊戲的任何檔案或設定。
	// ------------------------------------------------------------------
	{
		const LONG curW = clientRect.right - clientRect.left;
		const LONG curH = clientRect.bottom - clientRect.top;
		if (!pluginInstalled) {
			const double k = std::min((double)curW / W, (double)curH / H);
			const LONG canvasW = std::lround(W * k);
			const LONG canvasH = std::lround(H * k);
			if (canvasW != (LONG)W || canvasH != (LONG)H) {
				if (IsZoomed(hWnd)) {
					ShowWindow(hWnd, SW_RESTORE);
				}

				RECT wr{}, cr{};
				if (GetWindowRect(hWnd, &wr) && GetClientRect(hWnd, &cr)) {
					// 保留原本的視窗外框（標題欄與邊框）
					const LONG frameW = (wr.right - wr.left) - cr.right;
					const LONG frameH = (wr.bottom - wr.top) - cr.bottom;

					Logger::Get().Info(fmt::format(
						"自適應：把視窗由 {}x{} 調整為 {}x{} 實體像素，讓畫布回到 1:1",
						curW, curH, W, H));

					if (SetWindowPos(hWnd, nullptr, wr.left, wr.top,
						(LONG)W + frameW, (LONG)H + frameH,
						SWP_NOZORDER | SWP_NOACTIVATE)) {
						// 等引擎收到 WM_SIZE、重新計算 realScale 並重繪
						Sleep(150);
						Win32Helper::GetClientScreenRect(hWnd, clientRect);
					} else {
						Logger::Get().Win32Error("自適應：SetWindowPos 失敗");
					}
				}
			}
		}
	}

	const LONG cw = clientRect.right - clientRect.left;
	const LONG ch = clientRect.bottom - clientRect.top;
	if (cw < 64 || ch < 64) {
		return false;
	}

	// 畫布在裝置像素下的大小
	//   1:1（安裝了 PixelPerfect 插件，或視窗剛好等於原生解析度）-> 就是原生大小
	//   其他（引擎自動拉伸填滿視窗）-> min(寬比, 高比)，與引擎的 realScale 一致
	const bool pixelPerfect = pluginInstalled
		|| (std::abs(cw - (LONG)W) <= 2 && std::abs(ch - (LONG)H) <= 2);

	LONG canvasW = 0;
	LONG canvasH = 0;
	if (pixelPerfect) {
		canvasW = (LONG)W;
		canvasH = (LONG)H;
	} else {
		const double k = std::min((double)cw / W, (double)ch / H);
		canvasW = std::lround(W * k);
		canvasH = std::lround(H * k);
	}

	if (canvasW < 64 || canvasH < 64 || canvasW > cw || canvasH > ch) {
		Logger::Get().Info("自適應：算出的畫布尺寸不合理，跳過");
		return false;
	}

	// 引擎会把畫布置中
	const LONG left = clientRect.left + (cw - canvasW) / 2;
	const LONG top = clientRect.top + (ch - canvasH) / 2;

	result.srcRect = { left, top, left + canvasW, top + canvasH };
	result.nativeWidth = W;
	result.nativeHeight = H;
	result.pixelPerfect = pixelPerfect;

	// 有效放大倍率：以來源視窗所在監視器的工作區為目標
	HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ .cbSize = sizeof(mi) };
	if (GetMonitorInfo(hMon, &mi)) {
		const LONG tw = mi.rcWork.right - mi.rcWork.left;
		const LONG th = mi.rcWork.bottom - mi.rcWork.top;
		result.scale = (float)std::min((double)tw / canvasW, (double)th / canvasH);
	}

	Logger::Get().Info(fmt::format(
		"自適應判斷：原生 {}x{}，客戶區 {}x{}，{}，畫布 {}x{} 位於 ({},{})，放大倍率 {:.2f}x",
		W, H, cw, ch,
		pixelPerfect ? "1:1 像素完美" : "引擎拉伸",
		canvasW, canvasH, left, top, result.scale));

	return true;
}

}
