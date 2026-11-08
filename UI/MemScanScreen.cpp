#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "ppsspp_config.h"
#include "Common/UI/UI.h"
#include "Common/UI/PopupScreens.h"
#include "Common/Data/Text/I18n.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include "Common/System/System.h"
#include "Common/System/Request.h"
#include "Common/UI/ScreenManager.h"
#include "Core/System.h"
#include "Core/Config.h"
#include "Core/CwCheat.h"
#include "Core/MemScan.h"
#include "UI/MemScanScreen.h"

// Status dipertahankan selama aplikasi hidup, supaya hasil scan tidak hilang saat layar ditutup.
static MemScanner g_scan;
static ScanType g_type = ScanType::Float;
static ScanCmp g_cmp = ScanCmp::Changed;
static std::string g_valueText = "1.3333334";
static int g_selected = -1;
static std::map<uint32_t, uint32_t> g_orig;  // nilai asli sebelum dites
static std::string g_status = "Belum ada scan.";

static const char *const kTypeNames[] = { "8-bit", "16-bit", "32-bit (angka bulat)", "Float (pecahan)" };
static const char *const kCmpNames[] = { "Sama dengan nilai", "Berubah", "Tidak berubah", "Naik", "Turun" };

static bool ParseValue(const std::string &s, ScanType t, uint32_t *out) {
	if (s.empty())
		return false;
	char *end = nullptr;
	bool hex = s.find("0x") != std::string::npos || s.find("0X") != std::string::npos;
	if (t == ScanType::Float && !hex) {
		float f = strtof(s.c_str(), &end);
		if (end == s.c_str())
			return false;
		memcpy(out, &f, 4);
		return true;
	}
	if (s[0] == '-') {
		long v = strtol(s.c_str(), &end, 0);
		if (end == s.c_str())
			return false;
		*out = (uint32_t)v;
		return true;
	}
	unsigned long v = strtoul(s.c_str(), &end, 0);
	if (end == s.c_str())
		return false;
	*out = (uint32_t)v;
	return true;
}

// Untuk instruksi lui: boleh ketik angka hex (0x3FE3) atau float (1.7778 -> 16 bit atas).
static bool ParseLuiImm(const std::string &s, uint32_t *out) {
	if (s.empty())
		return false;
	if (s.find('.') != std::string::npos) {
		char *end = nullptr;
		float f = strtof(s.c_str(), &end);
		if (end == s.c_str())
			return false;
		uint32_t bits;
		memcpy(&bits, &f, 4);
		*out = bits >> 16;
		return true;
	}
	char *end = nullptr;
	unsigned long v = strtoul(s.c_str(), &end, 0);
	if (end == s.c_str())
		return false;
	*out = (uint32_t)(v & 0xFFFF);
	return true;
}

static std::string FormatHit(const ScanHit &h, ScanType type) {
	char buf[160];
	if (h.kind == HitKind::Lui) {
		snprintf(buf, sizeof(buf), "0x%08X  lui 0x%04X  [%s]", h.addr, h.value & 0xFFFF, h.tag ? h.tag : "");
	} else if (h.tag) {
		float f;
		memcpy(&f, &h.value, 4);
		snprintf(buf, sizeof(buf), "0x%08X  float %.5f  [%s]", h.addr, f, h.tag);
	} else if (type == ScanType::Float) {
		float f;
		memcpy(&f, &h.value, 4);
		snprintf(buf, sizeof(buf), "0x%08X  %.5f", h.addr, f);
	} else {
		snprintf(buf, sizeof(buf), "0x%08X  %u (0x%X)", h.addr, h.value, h.value);
	}
	return buf;
}

MemScanScreen::MemScanScreen(const Path &gamePath, const std::string &gameID)
	: UITwoPaneBaseDialogScreen(gamePath, TwoPaneFlags::Default), gameID_(gameID) {
}

bool MemScanScreen::SaveCheat(const std::string &name) {
	const auto &hits = g_scan.Hits();
	if (g_selected < 0 || g_selected >= (int)hits.size() || gameID_.empty())
		return false;
	const ScanHit &h = hits[g_selected];
	if (h.addr < 0x08800000)
		return false;

	uint32_t val = MemScanner::ReadAt(h.addr, h.size);
	int nibble = h.size == 1 ? 0 : (h.size == 2 ? 1 : 2);

	std::string clean = name.empty() ? "Cheat baru" : name;
	for (char &c : clean) {
		if (c == '\n' || c == '\r')
			c = ' ';
	}

	CWCheatEngine engine(gameID_);
	engine.CreateCheatFile();
	FILE *f = File::OpenCFile(engine.CheatFilename(), "at");
	if (!f)
		return false;
	fputc('\n', f);
	fprintf(f, "_C0 %s\n", clean.c_str());
	fprintf(f, "_L 0x%X%07X 0x%08X\n", nibble, (h.addr - 0x08800000) & 0x0FFFFFFF, val);
	fclose(f);
	g_Config.bReloadCheats = true;
	return true;
}

void MemScanScreen::CreateSettingsViews(UI::ViewGroup *left) {
	using namespace UI;

	left->Add(new ItemHeader("1. Cari nilai"));

	left->Add(new Choice(std::string("Tipe data: ") + kTypeNames[(int)g_type]))->OnClick.Add([this](UI::EventParams &) {
		g_type = (ScanType)(((int)g_type + 1) % 4);
		RecreateViews();
	});

	Choice *valueChoice = left->Add(new Choice("Nilai dicari: " + g_valueText));
	valueChoice->OnClick.Add([this, valueChoice, sm = screenManager(), token = GetRequesterToken()](UI::EventParams &) {
		AskForInput(sm, token, valueChoice, "Nilai dicari", [this](const std::string &text, bool success) {
			if (!success)
				return;
			g_valueText = text;
			RecreateViews();
		});
	});

	left->Add(new Choice("Scan pertama (seluruh RAM)"))->OnClick.Add([this](UI::EventParams &) {
		uint32_t raw = 0;
		if (!ParseValue(g_valueText, g_type, &raw)) {
			g_status = "Nilai tidak valid.";
		} else {
			g_scan.First(g_type, raw);
			g_selected = -1;
			g_orig.clear();
			g_status = StringFromFormat("Ditemukan %d alamat%s.", (int)g_scan.Hits().size(), g_scan.Truncated() ? " (dibatasi, terlalu banyak)" : "");
		}
		RecreateViews();
	});

	left->Add(new Choice(std::string("Saring lagi: ") + kCmpNames[(int)g_cmp]))->OnClick.Add([this](UI::EventParams &) {
		g_cmp = (ScanCmp)(((int)g_cmp + 1) % 5);
		RecreateViews();
	});

	left->Add(new Choice("Jalankan saring lagi"))->OnClick.Add([this](UI::EventParams &) {
		if (!g_scan.HasScan() || g_scan.IsWidescreenMode()) {
			g_status = "Lakukan scan pertama dulu (bukan hasil preset widescreen).";
		} else {
			uint32_t raw = 0;
			if (g_cmp == ScanCmp::Equal && !ParseValue(g_valueText, g_type, &raw)) {
				g_status = "Nilai tidak valid.";
			} else {
				g_scan.Next(g_cmp, raw);
				g_selected = -1;
				g_status = StringFromFormat("Tersisa %d alamat.", (int)g_scan.Hits().size());
			}
		}
		RecreateViews();
	});

	left->Add(new Choice("Hapus hasil scan"))->OnClick.Add([this](UI::EventParams &) {
		g_scan.Reset();
		g_selected = -1;
		g_orig.clear();
		g_status = "Hasil dihapus.";
		RecreateViews();
	});

	left->Add(new ItemHeader("2. Widescreen"));
	left->Add(new Choice("Cari konstanta layar 4:3 / 16:9"))->OnClick.Add([this](UI::EventParams &) {
		g_scan.ScanWidescreen();
		g_selected = -1;
		g_orig.clear();
		g_status = StringFromFormat("Ditemukan %d kandidat (float dan instruksi lui).", (int)g_scan.Hits().size());
		RecreateViews();
	});

	const auto &hits = g_scan.Hits();
	if (g_selected >= 0 && g_selected < (int)hits.size()) {
		const ScanHit &h = hits[g_selected];
		left->Add(new ItemHeader(StringFromFormat("3. Alamat terpilih 0x%08X", h.addr)));

		Choice *testChoice = left->Add(new Choice(h.kind == HitKind::Lui ? "Coba nilai baru (hex 0x3FE3 atau float 1.7778)" : "Coba nilai baru"));
		testChoice->OnClick.Add([this, testChoice, sm = screenManager(), token = GetRequesterToken()](UI::EventParams &) {
			AskForInput(sm, token, testChoice, "Nilai baru", [this](const std::string &text, bool success) {
				const auto &hits = g_scan.Hits();
				if (!success || g_selected < 0 || g_selected >= (int)hits.size())
					return;
				const ScanHit &h = hits[g_selected];
				uint32_t val = 0;
				bool ok = h.kind == HitKind::Lui ? ParseLuiImm(text, &val) : ParseValue(text, g_scan.Type(), &val);
				if (!ok) {
					g_status = "Nilai tidak valid.";
				} else {
					if (!g_orig.count(h.addr))
						g_orig[h.addr] = MemScanner::ReadAt(h.addr, h.size);
					g_status = MemScanner::WriteAt(h.addr, h.size, val) ? "Nilai ditulis. Lihat efeknya di game." : "Gagal menulis (alamat tidak valid).";
				}
				RecreateViews();
			});
		});

		left->Add(new Choice("Kembalikan nilai asli"))->OnClick.Add([this](UI::EventParams &) {
			const auto &hits = g_scan.Hits();
			if (g_selected >= 0 && g_selected < (int)hits.size()) {
				const ScanHit &h = hits[g_selected];
				auto it = g_orig.find(h.addr);
				if (it != g_orig.end()) {
					MemScanner::WriteAt(h.addr, h.size, it->second);
					g_status = "Nilai dikembalikan.";
				} else {
					g_status = "Belum ada nilai yang dites di alamat ini.";
				}
			}
			RecreateViews();
		});

		Choice *saveChoice = left->Add(new Choice("Simpan jadi cheat (nilai saat ini)"));
		saveChoice->OnClick.Add([this, saveChoice, sm = screenManager(), token = GetRequesterToken()](UI::EventParams &) {
			AskForInput(sm, token, saveChoice, "Nama cheat", [this](const std::string &text, bool success) {
				if (!success)
					return;
				g_status = SaveCheat(text) ? "Cheat disimpan (nonaktif). Aktifkan di daftar cheat." : "Gagal menyimpan cheat.";
				RecreateViews();
			});
		});
	}
}

void MemScanScreen::CreateContentViews(UI::ViewGroup *parent) {
	using namespace UI;

	LinearLayout *rightSide = parent->Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT, 0.5f)));
	rightSide->Add(new TextView(g_status, new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT, UI::Margins(8, 8, 8, 8))));

	ScrollView *scroll = rightSide->Add(new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT, 1.0f)));
	scroll->SetTag("MemScanResults");
	LinearLayout *column = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(200, FILL_PARENT));
	column->SetSpacing(0.0f);
	scroll->Add(column);

	const auto &hits = g_scan.Hits();
	const int limit = (int)std::min<size_t>(hits.size(), 300);
	for (int i = 0; i < limit; i++) {
		std::string label = FormatHit(hits[i], g_scan.Type());
		if (i == g_selected)
			label = "> " + label;
		column->Add(new Choice(label))->OnClick.Add([this, i](UI::EventParams &) {
			g_selected = i;
			RecreateViews();
		});
	}
	if ((int)hits.size() > limit) {
		column->Add(new TextView(StringFromFormat("... dan %d lainnya. Saring dulu supaya lebih sedikit.", (int)hits.size() - limit), new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT, UI::Margins(8, 8, 8, 8))));
	}
}
