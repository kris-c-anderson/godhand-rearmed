// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/HW/GSTextureCache.h"

#include <utility>

namespace GSTextureReplacements
{
	struct ReplacementTexture
	{
		u32 width;
		u32 height;
		GSTexture::Format format;
		std::pair<u8, u8> alpha_minmax;

		u32 pitch;
		std::vector<u8> data;

		struct MipData
		{
			u32 width;
			u32 height;
			u32 pitch;
			std::vector<u8> data;
		};
		std::vector<MipData> mips;

		// pcsx2gs (God Hand port): the alpha levels this texture was snapped to (MatchOriginalAlpha); 0 = not snapped.
		u64 snapped_levels_hash = 0;
	};

	void Initialize();
	void GameChanged();
	void ReloadReplacementMap();
	void UpdateConfig(Pcsx2Config::GSOptions& old_config);
	void Shutdown();

	u32 CalcMipmapLevelsForReplacement(u32 width, u32 height);

	bool HasAnyReplacementTextures();
	bool HasReplacementTexture(const GSTextureCache::HashCacheKey& hash);
	bool HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey& hash);
	GSTexture* LookupReplacementTexture(const GSTextureCache::HashCacheKey& hash, bool mipmap, bool* pending, std::pair<u8, u8>* alpha_minmax);
	GSTexture* CreateReplacementTexture(const ReplacementTexture& rtex, bool mipmap);

	// pcsx2gs (God Hand port): a replacement may only use the alpha levels of the game's own texture: each texel's alpha
	// is snapped to the nearest level the texture's palette has (an opaque texture: 0x80 everywhere; a cutout such as
	// a mountain backdrop: 0 or 0x80; a graded cloud keeps its gradient). Packs made from dumps and edited carry
	// 0x7F, 0x40, 0x89, 0xAC... there: God Hand's alpha test (GREATER 0x7F, AFAIL FB_ONLY) fails on them, so they blend
	// half-transparent, write no depth (the scene behind shows through, the depth of field blurs over them), and values
	// above 0x80 blend brighter than the original. PS2X_PCSX2_MATCH_ALPHA=0 turns it off.
	void MatchOriginalAlpha(const GSTextureCache::HashCacheKey& hash, const u32* clut, u32 entries);
	void ProcessAsyncLoadedTextures();

	void DumpTexture(const GSTextureCache::HashCacheKey& hash, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA,
		GSTextureCache::SourceRegion region, GSLocalMemory& mem, u32 level);
	void ClearDumpedTextureList();

	/// Get the number of textures that have been dumped.
	u32 GetDumpedTextureCount();

	/// Get the number of replacement textures that have been loaded/cached.
	u32 GetLoadedTextureCount();

	class File
	{
	public:
		virtual bool Read(void* data, size_t amt) = 0;
		virtual bool Seek(size_t offset) = 0;
		virtual s64 Size() = 0;

		template <typename T>
		bool ReadRawStruct(T* out) { return Read(out, sizeof(*out)); }
	};

	class CFile : public File
	{
		FILE* file;
	public:
		CFile(FILE* file_): file(file_) {};
		bool Read(void* data, size_t amt) override;
		bool Seek(size_t offset) override;
		s64 Size() override;
	};

	class MemoryFile : public File
	{
		const u8* buffer;
		size_t len;
		size_t pos;
	public:
		MemoryFile(const void* buffer_, size_t len_): buffer(static_cast<const u8*>(buffer_)), len(len_), pos(0) {};
		bool Read(void* data, size_t amt) override;
		bool Seek(size_t offset) override;
		s64 Size() override;
	};

	/// Loader will take a filename and interpret the format (e.g. DDS, PNG, etc).
	using ReplacementTextureLoader = bool (*)(File& file, const char* filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image);
	ReplacementTextureLoader GetLoader(const std::string_view filename);

	/// Saves an image buffer to a PNG file (for dumping).
	bool SavePNGImage(const std::string& filename, u32 width, u32 height, const u8* buffer, u32 pitch);
} // namespace GSTextureReplacements
