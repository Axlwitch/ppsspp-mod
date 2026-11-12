// Pengaturan efek shader game (u_setting / u_setting2), header-only.
// Definisi slider dibaca dari <glsl>/fx_settings.txt, nilai disimpan di <glsl>/fx_values.txt.
// Satu baris definisi per setting, urutan baris = slot:
//   Nama | min | max | default | step
// Slot 1-4 -> u_setting.xyzw, slot 5-8 -> u_setting2.xyzw (hanya di FS).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Core/Config.h"

#if !defined(GLSL_REPLACE_DIR) && defined(__ANDROID__)
#define GLSL_REPLACE_DIR "/storage/emulated/0/PSP/glsl"
#endif

struct FxSettingDef {
	std::string name;
	float minV = 0.0f;
	float maxV = 1.0f;
	float def = 0.0f;
	float step = 0.01f;
};

namespace FxSettings {

constexpr int kMax = 8;

struct State {
	std::mutex lock;
	std::vector<FxSettingDef> defs;
	float values[kMax] = {};
	std::atomic<int> version{ 0 };
	std::atomic<bool> loaded{ false };
};

inline State &S() {
	static State s;
	return s;
}

inline Path Dir() {
#ifdef GLSL_REPLACE_DIR
	return Path(std::string(GLSL_REPLACE_DIR));
#else
	return g_Config.memStickDirectory / std::string("glsl");
#endif
}

inline std::string Trim(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

inline void WriteTemplateIfMissing(const Path &file) {
	std::ifstream test(file.ToString());
	if (test.is_open())
		return;
	File::CreateDir(Dir());
	std::ofstream out(file.ToString());
	if (!out.is_open())
		return;
	out << "# Satu baris = satu slider di menu Graphics. Urutan baris = slot (maks 8).\n"
		   "# Format: Nama | min | max | default | step\n"
		   "# Slot 1-4 -> u_setting.x/y/z/w, slot 5-8 -> u_setting2.x/y/z/w\n"
		   "# Di shader: uniform highp vec4 u_setting;  uniform highp vec4 u_setting2;\n"
		   "Specular strength | 0 | 4 | 1 | 0.05\n"
		   "Specular shininess | 1 | 128 | 32 | 1\n";
}

// Muat ulang definisi + nilai dari file. Aman dipanggil kapan saja.
inline void Load() {
	State &s = S();
	std::lock_guard<std::mutex> guard(s.lock);
	const Path dir = Dir();
	const Path defFile = dir / std::string("fx_settings.txt");
	WriteTemplateIfMissing(defFile);

	s.defs.clear();
	std::ifstream in(defFile.ToString());
	std::string line;
	while (in && std::getline(in, line) && (int)s.defs.size() < kMax) {
		line = Trim(line);
		if (line.empty() || line[0] == '#')
			continue;
		std::vector<std::string> parts;
		size_t pos = 0;
		while (true) {
			size_t bar = line.find('|', pos);
			parts.push_back(Trim(line.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos)));
			if (bar == std::string::npos)
				break;
			pos = bar + 1;
		}
		if (parts[0].empty())
			continue;
		FxSettingDef d;
		d.name = parts[0];
		if (parts.size() > 1 && !parts[1].empty()) d.minV = (float)atof(parts[1].c_str());
		if (parts.size() > 2 && !parts[2].empty()) d.maxV = (float)atof(parts[2].c_str());
		if (parts.size() > 3 && !parts[3].empty()) d.def = (float)atof(parts[3].c_str());
		else d.def = d.minV;
		if (parts.size() > 4 && !parts[4].empty()) d.step = (float)atof(parts[4].c_str());
		if (d.maxV <= d.minV) d.maxV = d.minV + 1.0f;
		if (d.step <= 0.0f) d.step = (d.maxV - d.minV) / 100.0f;
		d.def = std::min(std::max(d.def, d.minV), d.maxV);
		s.defs.push_back(d);
	}

	// Nilai tersimpan: baris "Nama=nilai".
	std::map<std::string, float> saved;
	std::ifstream vin((dir / std::string("fx_values.txt")).ToString());
	while (vin && std::getline(vin, line)) {
		size_t eq = line.rfind('=');
		if (eq == std::string::npos)
			continue;
		saved[Trim(line.substr(0, eq))] = (float)atof(Trim(line.substr(eq + 1)).c_str());
	}
	for (int i = 0; i < kMax; i++)
		s.values[i] = 0.0f;
	for (size_t i = 0; i < s.defs.size(); i++) {
		const FxSettingDef &d = s.defs[i];
		auto it = saved.find(d.name);
		float v = it != saved.end() ? it->second : d.def;
		s.values[i] = std::min(std::max(v, d.minV), d.maxV);
	}
	s.loaded = true;
	s.version++;
}

inline void EnsureLoaded() {
	if (!S().loaded)
		Load();
}

inline std::vector<FxSettingDef> Defs() {
	EnsureLoaded();
	State &s = S();
	std::lock_guard<std::mutex> guard(s.lock);
	return s.defs;
}

// Pointer nilai untuk slider UI (indeks 0..kMax-1).
inline float *ValuePtr(int i) {
	EnsureLoaded();
	return &S().values[i];
}

inline int Version() {
	EnsureLoaded();
	return S().version.load();
}

inline void GetPacked(float out[kMax]) {
	EnsureLoaded();
	State &s = S();
	for (int i = 0; i < kMax; i++)
		out[i] = s.values[i];
}

// Dipanggil UI setelah slider berubah: simpan ke file dan paksa uniform dikirim ulang.
inline void NotifyChanged() {
	State &s = S();
	{
		std::lock_guard<std::mutex> guard(s.lock);
		File::CreateDir(Dir());
		std::ofstream out((Dir() / std::string("fx_values.txt")).ToString());
		if (out.is_open()) {
			for (size_t i = 0; i < s.defs.size(); i++)
				out << s.defs[i].name << "=" << s.values[i] << "\n";
		}
	}
	s.version++;
}

inline void ResetToDefaults() {
	State &s = S();
	{
		std::lock_guard<std::mutex> guard(s.lock);
		for (size_t i = 0; i < s.defs.size(); i++)
			s.values[i] = s.defs[i].def;
	}
	NotifyChanged();
}

}  // namespace FxSettings
