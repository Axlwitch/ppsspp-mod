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
static int g_freezeSel = -1;  // freeze yang dipilih di daftar
static bool g_showWatch = false;  // panel kanan: false = hasil scan, true = log akses CPU
static int g_recSel = -1;
static std::vector<AccessRecord> g_recs;

// Cheat pointer CWCheat: pointer disimpan di ptrAddr, nilai ditulis ke [pointer + offset].
static bool SavePointerCheat(const std::string &gameID, const std::string &name, uint32_t ptrAddr, int32_t offset, int size, uint32_t value) {
	if (gameID.empty() || ptrAddr < 0x08800000)
		return false;
	int type = size == 1 ? 0 : (size == 2 ? 1 : 2);  // 0/1/2 = tulis 8/16/32 bit di [pointer+offset]
	uint32_t off = (uint32_t)offset;
	if (offset < 0) {
		type += 3;  // 3/4/5 = varian [pointer-offset]
		off = (uint32_t)(-offset);
	}
	std::string clean = name.empty() ? "Cheat pointer" : name;
	for (char &c : clean) {
		if (c == '\n' || c == '\r')
			c = ' ';
	}
	CWCheatEngine engine(gameID);
	engine.CreateCheatFile();
	FILE *f = File::OpenCFile(engine.CheatFilename(), "at");
	if (!f)
		return false;
	fputc('\n', f);
	fprintf(f, "_C0 %s\n", clean.c_str());
	fprintf(f, "_L 0x6%07X 0x%08X\n", (ptrAddr - 0x08800000) & 0x0FFFFFFF, value);
	fprintf(f, "_L 0x%08X 0x%08X\n", ((uint32_t)type << 16) | 1u, off);
	fclose(f);
	g_Config.bReloadCheats = true;
	return true;
}

static const char *const kTypeNames[] = { "8-bit", "16-bit", "32-bit (angka bulat)", "Float (pecahan)" };
static const char *const kCmpNames[] = { "Sama dengan nilai", "Berubah", "Tidak berubah", "Naik", "Turun" };

static bool ParseValueRaw(const std::string &s, ScanType t, uint32_t *out);
// Nilai dimasker sesuai ukuran data, supaya angka negatif 8/16-bit cocok dengan hasil baca.
static bool ParseValue(const std::string &s, ScanType t, uint32_t *out) {
	if (!ParseValueRaw(s, t, out))
		return false;
	if (t == ScanType::U8) *out &= 0xFF;
	else if (t == ScanType::U16) *out &= 0xFFFF;
	return true;
}

static bool ParseValueRaw(const std::string &s, ScanType t, uint32_t *out) {
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

		left->Add(new Choice("Bekukan (lock) di nilai saat ini"))->OnClick.Add([this](UI::EventParams &) {
			const auto &hits = g_scan.Hits();
			if (g_selected >= 0 && g_selected < (int)hits.size()) {
				const ScanHit &h = hits[g_selected];
				uint32_t val = MemScanner::ReadAt(h.addr, h.size);
				g_freezeSel = MemFreezer::Add(h.addr, h.size, h.kind, val, h.tag ? h.tag : "");
				g_status = StringFromFormat("Dibekukan 0x%08X = 0x%X", h.addr, val);
			}
			RecreateViews();
		});

		left->Add(new Choice("Pantau akses CPU ke alamat ini"))->OnClick.Add([this](UI::EventParams &) {
			const auto &hits = g_scan.Hits();
			if (g_selected >= 0 && g_selected < (int)hits.size()) {
				const ScanHit &h = hits[g_selected];
				if (MemWatch::Start(h.addr, h.size)) {
					g_showWatch = true;
					g_recSel = -1;
					g_status = StringFromFormat("Memantau 0x%08X. Kembali ke game, lakukan aksinya, lalu buka layar ini lagi.", h.addr);
				} else {
					g_status = "Gagal memantau (alamat tidak valid).";
				}
			}
			RecreateViews();
		});
	}

	// ---- 4. Daftar freeze ----
	left->Add(new ItemHeader(StringFromFormat("4. Freeze (%d)", MemFreezer::Count())));
	for (int i = 0; i < MemFreezer::Count(); i++) {
		FreezeEntry e;
		if (!MemFreezer::Get(i, &e))
			continue;
		std::string label = StringFromFormat("%s0x%08X = 0x%X%s%s", i == g_freezeSel ? "> " : "", e.addr, e.value,
			e.label.empty() ? "" : "  ", e.label.c_str());
		left->Add(new Choice(std::string(e.enabled ? "[ON] " : "[OFF] ") + label))->OnClick.Add([this, i](UI::EventParams &) {
			g_freezeSel = i;
			RecreateViews();
		});
	}

	FreezeEntry sel;
	if (MemFreezer::Get(g_freezeSel, &sel)) {
		left->Add(new Choice(sel.enabled ? "Matikan freeze ini" : "Nyalakan freeze ini"))->OnClick.Add([this](UI::EventParams &) {
			FreezeEntry e;
			if (MemFreezer::Get(g_freezeSel, &e))
				MemFreezer::SetEnabled(g_freezeSel, !e.enabled);
			RecreateViews();
		});

		Choice *valChoice = left->Add(new Choice("Ubah nilai beku"));
		valChoice->OnClick.Add([this, valChoice, sm = screenManager(), token = GetRequesterToken()](UI::EventParams &) {
			AskForInput(sm, token, valChoice, "Nilai beku", [this](const std::string &text, bool success) {
				FreezeEntry e;
				if (!success || !MemFreezer::Get(g_freezeSel, &e))
					return;
				uint32_t v = 0;
				ScanType t = e.size == 1 ? ScanType::U8 : (e.size == 2 ? ScanType::U16 : g_type);
				bool ok = e.kind == HitKind::Lui ? ParseLuiImm(text, &v) : ParseValue(text, t, &v);
				if (ok) {
					MemFreezer::SetValue(g_freezeSel, v);
					g_status = "Nilai beku diubah.";
				} else {
					g_status = "Nilai tidak valid.";
				}
				RecreateViews();
			});
		});

		left->Add(new Choice("Cari ulang alamat ini (setelah pindah stage)"))->OnClick.Add([this](UI::EventParams &) {
			int miss = 0;
			uint32_t oldA = 0, newA = 0;
			if (MemFreezer::Relocate(g_freezeSel, &miss, &oldA, &newA))
				g_status = StringFromFormat("Alamat 0x%08X -> 0x%08X (byte beda: %d).", oldA, newA, miss);
			else
				g_status = "Tidak ketemu. Pola di sekitarnya sudah terlalu beda; scan ulang manual.";
			RecreateViews();
		});

		left->Add(new Choice("Hapus freeze ini"))->OnClick.Add([this](UI::EventParams &) {
			MemFreezer::Remove(g_freezeSel);
			g_freezeSel = -1;
			RecreateViews();
		});
	}

	if (MemFreezer::Count() > 0) {
		left->Add(new Choice("Cari ulang SEMUA freeze"))->OnClick.Add([this](UI::EventParams &) {
			int ok = MemFreezer::RelocateAll();
			g_status = StringFromFormat("Alamat diperbarui: %d dari %d.", ok, MemFreezer::Count());
			RecreateViews();
		});
		left->Add(new Choice("Hapus semua freeze"))->OnClick.Add([this](UI::EventParams &) {
			MemFreezer::ClearAll();
			g_freezeSel = -1;
			RecreateViews();
		});
	}

	// ---- 5. Pantau akses CPU ----
	g_recs = MemWatch::Snapshot();
	left->Add(new ItemHeader(StringFromFormat("5. Pantau akses CPU%s (%d)", MemWatch::Active() ? " [AKTIF]" : "", (int)g_recs.size())));
	if (MemWatch::Active()) {
		left->Add(new TextView(StringFromFormat("Memantau 0x%08X (%d byte). Memcheck membuat game lebih lambat; hentikan jika sudah selesai.", MemWatch::WatchAddr(), MemWatch::WatchSize()),
			new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT, UI::Margins(8, 4, 8, 4))));
	}
	left->Add(new Choice(g_showWatch ? "Panel kanan: log akses (ketuk: hasil scan)" : "Panel kanan: hasil scan (ketuk: log akses)"))->OnClick.Add([this](UI::EventParams &) {
		g_showWatch = !g_showWatch;
		g_recSel = -1;
		RecreateViews();
	});
	if (MemWatch::Active()) {
		left->Add(new Choice("Hentikan pantau"))->OnClick.Add([this](UI::EventParams &) {
			MemWatch::Stop();
			g_status = "Pantau dihentikan (log tetap ada).";
			RecreateViews();
		});
	}
	left->Add(new Choice("Bersihkan log akses"))->OnClick.Add([this](UI::EventParams &) {
		MemWatch::Clear();
		g_recSel = -1;
		RecreateViews();
	});

	if (g_recSel >= 0 && g_recSel < (int)g_recs.size()) {
		const AccessRecord &r = g_recs[g_recSel];
		left->Add(new ItemHeader(StringFromFormat("PC 0x%08X  %s", r.pc, r.write ? "tulis" : "baca")));
		left->Add(new TextView(r.text, new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT, UI::Margins(8, 2, 8, 2))));
		if (r.baseReg >= 0) {
			left->Add(new TextView(StringFromFormat("Basis %s = 0x%08X, offset %d. Jika %s adalah pointer, simpanannya bisa dicari.", MemWatch::RegName(r.baseReg), r.baseValue, (int)r.offset, MemWatch::RegName(r.baseReg)),
				new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT, UI::Margins(8, 2, 8, 2))));
			left->Add(new Choice(StringFromFormat("Cari pointer ke 0x%08X (scan 32-bit)", r.baseValue)))->OnClick.Add([this, base = r.baseValue](UI::EventParams &) {
				g_scan.First(ScanType::U32, base);
				g_selected = -1;
				g_orig.clear();
				g_showWatch = false;
				g_status = StringFromFormat("Pemegang pointer 0x%08X: %d alamat. Pilih satu untuk dibuat cheat pointer.", base, (int)g_scan.Hits().size());
				RecreateViews();
			});

			const auto &hits = g_scan.Hits();
			if (g_selected >= 0 && g_selected < (int)hits.size() && hits[g_selected].size == 4 && hits[g_selected].kind == HitKind::Value) {
				const uint32_t ptrAddr = hits[g_selected].addr;
				Choice *ptrChoice = left->Add(new Choice(StringFromFormat("Simpan cheat pointer (di 0x%08X, offset %d)", ptrAddr, (int)r.offset)));
				ptrChoice->OnClick.Add([this, ptrChoice, ptrAddr, off = r.offset, sm = screenManager(), token = GetRequesterToken()](UI::EventParams &) {
					AskForInput(sm, token, ptrChoice, "Nama cheat", [this, ptrAddr, off](const std::string &text, bool success) {
						if (!success)
							return;
						int sz = MemWatch::WatchSize();
						uint32_t val = MemScanner::ReadAt(MemWatch::WatchAddr(), sz);
						g_status = SavePointerCheat(gameID_, text, ptrAddr, off, sz, val) ? "Cheat pointer disimpan (nonaktif). Aktifkan di daftar cheat." : "Gagal menyimpan cheat pointer.";
						RecreateViews();
					});
				});
			}
		}
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

	if (g_showWatch) {
		g_recs = MemWatch::Snapshot();
		const int rlimit = (int)std::min<size_t>(g_recs.size(), 300);
		for (int i = 0; i < rlimit; i++) {
			const AccessRecord &r = g_recs[i];
			std::string label = StringFromFormat("%s%s %08X  %s  x%u", i == g_recSel ? "> " : "", r.write ? "W" : "R", r.pc, r.text.c_str(), r.count);
			if (r.baseReg >= 0)
				label += StringFromFormat("  [%s%+d]", MemWatch::RegName(r.baseReg), (int)r.offset);
			column->Add(new Choice(label))->OnClick.Add([this, i](UI::EventParams &) {
				g_recSel = i;
				RecreateViews();
			});
		}
		if (g_recs.empty())
			column->Add(new TextView("Belum ada akses tercatat.", new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT, UI::Margins(8, 8, 8, 8))));
		return;
	}

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
