#include "LandRecords.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_map>

namespace
{
	constexpr int32_t QuadsPerCell = 32;
	constexpr uint32_t CellVertices = QuadsPerCell + 1;
	constexpr size_t CellHeights = static_cast<size_t>(CellVertices) * CellVertices;
	constexpr float VertexSpacing = static_cast<float>(PBRWaterModel::CellSize) / QuadsPerCell;
	/// VHGT stores z in steps of 8 units.
	constexpr float HeightUnit = 8.0f;
	/// Size of a VHGT field: the float offset, then one signed delta per vertex (3 bytes of padding follow).
	constexpr size_t HeightFieldSize = sizeof(float) + CellHeights;
	/// Cells without a LAND read this far below the lowest terrain.
	constexpr float MissingDepth = 8192.0f;

	constexpr size_t RecordHeaderSize = 24;
	constexpr uint32_t CompressedFlag = 0x00040000;
	constexpr uint32_t DeletedFlag = 0x00000020;
	/// CELL's XCLC and LAND's VHGT sit near the start of their records (LAND's after DATA and VNML), so inflating only this
	/// much skips the texture layers and vertex colours that make up most of a LAND.
	constexpr size_t FieldLimit = 8192;

	enum GroupType : int32_t
	{
		WorldChildren = 1,
		ExteriorBlock = 4,
		ExteriorSubBlock = 5,
		CellChildren = 6,
		TemporaryChildren = 9,
	};

	constexpr uint32_t Tag(const char (&name)[5])
	{
		return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) | static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 8 |
		       static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(name[3])) << 24;
	}

	template <class T>
	T Read(const uint8_t* p)
	{
		T value;
		std::memcpy(&value, p, sizeof(T));
		return value;
	}

	/**
	 * Inflates a raw DEFLATE stream (RFC 1951) up to the size of the target, one code bit at a time after Adler's puff:
	 * the records are small, and reading stops as soon as the fields wanted are out.
	 */
	class Inflater
	{
	public:
		Inflater(std::span<const uint8_t> compressed, std::span<uint8_t> target) :
			source(compressed), out(target) {}

		/// @return the bytes written: all of the stream or as much of it as fits, what came before any corruption
		size_t Run()
		{
			bool last = false;
			while (!last && !failed && written < out.size()) {
				last = Bits(1) != 0;
				switch (Bits(2)) {
				case 0:
					Stored();
					break;
				case 1:
					Blocks(FixedLengths(), FixedDistances());
					break;
				case 2:
					Dynamic();
					break;
				default:
					failed = true;
					break;
				}
			}
			return written;
		}

	private:
		/// Canonical Huffman code: `count[n]` codes of n bits, `symbol` ordered by code.
		struct Huffman
		{
			uint16_t count[16] = {};
			uint16_t symbol[320] = {};

			void Build(const uint8_t* lengths, uint32_t symbols)
			{
				std::fill(std::begin(count), std::end(count), static_cast<uint16_t>(0));
				for (uint32_t i = 0; i < symbols; ++i)
					++count[lengths[i]];
				uint16_t offsets[16] = {};
				for (uint32_t n = 1; n < 15; ++n)
					offsets[n + 1] = offsets[n] + count[n];
				for (uint32_t i = 0; i < symbols; ++i) {
					if (lengths[i])
						symbol[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
				}
				count[0] = 0;
			}
		};

		static const Huffman& FixedLengths()
		{
			static const Huffman code = [] {
				uint8_t lengths[288];
				std::fill_n(lengths, 144, static_cast<uint8_t>(8));
				std::fill_n(lengths + 144, 112, static_cast<uint8_t>(9));
				std::fill_n(lengths + 256, 24, static_cast<uint8_t>(7));
				std::fill_n(lengths + 280, 8, static_cast<uint8_t>(8));
				Huffman h;
				h.Build(lengths, 288);
				return h;
			}();
			return code;
		}

		static const Huffman& FixedDistances()
		{
			static const Huffman code = [] {
				uint8_t lengths[30];
				std::fill_n(lengths, 30, static_cast<uint8_t>(5));
				Huffman h;
				h.Build(lengths, 30);
				return h;
			}();
			return code;
		}

		uint32_t Bits(uint32_t need)
		{
			while (bitCount < need) {
				if (position >= source.size()) {
					failed = true;
					return 0;
				}
				bitBuffer |= static_cast<uint32_t>(source[position++]) << bitCount;
				bitCount += 8;
			}
			const uint32_t value = bitBuffer & ((1u << need) - 1);
			bitBuffer >>= need;
			bitCount -= need;
			return value;
		}

		int32_t Decode(const Huffman& h)
		{
			int32_t code = 0, first = 0, index = 0;
			for (uint32_t length = 1; length <= 15; ++length) {
				code |= static_cast<int32_t>(Bits(1));
				if (failed)
					return -1;
				const int32_t count = h.count[length];
				if (code - count < first)
					return h.symbol[index + (code - first)];
				index += count;
				first = (first + count) << 1;
				code <<= 1;
			}
			failed = true;
			return -1;
		}

		void Stored()
		{
			bitBuffer = 0;
			bitCount = 0;
			if (position + 4 > source.size()) {
				failed = true;
				return;
			}
			size_t length = source[position] | static_cast<size_t>(source[position + 1]) << 8;
			position += 4;
			length = std::min(length, source.size() - position);
			const size_t copied = std::min(length, out.size() - written);
			std::memcpy(out.data() + written, source.data() + position, copied);
			written += copied;
			position += length;
		}

		void Blocks(const Huffman& lengths, const Huffman& distances)
		{
			static constexpr uint16_t LengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
			static constexpr uint8_t LengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
			static constexpr uint16_t DistanceBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
			static constexpr uint8_t DistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

			while (!failed && written < out.size()) {
				const int32_t symbol = Decode(lengths);
				if (symbol < 0 || symbol == 256)
					return;
				if (symbol < 256) {
					out[written++] = static_cast<uint8_t>(symbol);
					continue;
				}
				const int32_t lengthCode = symbol - 257;
				if (lengthCode >= 29) {
					failed = true;
					return;
				}
				const size_t length = LengthBase[lengthCode] + Bits(LengthExtra[lengthCode]);
				const int32_t distanceCode = Decode(distances);
				if (distanceCode < 0 || distanceCode >= 30) {
					failed = true;
					return;
				}
				const size_t distance = DistanceBase[distanceCode] + Bits(DistanceExtra[distanceCode]);
				if (failed || distance > written) {
					failed = true;
					return;
				}
				for (size_t i = 0; i < length && written < out.size(); ++i, ++written)
					out[written] = out[written - distance];
			}
		}

		void Dynamic()
		{
			static constexpr uint8_t Order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
			const uint32_t lengthCount = Bits(5) + 257;
			const uint32_t distanceCount = Bits(5) + 1;
			const uint32_t codeCount = Bits(4) + 4;
			if (failed || lengthCount > 286 || distanceCount > 30) {
				failed = true;
				return;
			}
			uint8_t lengths[320] = {};
			for (uint32_t i = 0; i < codeCount; ++i)
				lengths[Order[i]] = static_cast<uint8_t>(Bits(3));
			Huffman codeLengths;
			codeLengths.Build(lengths, 19);

			std::fill(std::begin(lengths), std::end(lengths), static_cast<uint8_t>(0));
			uint32_t index = 0;
			while (index < lengthCount + distanceCount) {
				const int32_t symbol = Decode(codeLengths);
				if (symbol < 0)
					return;
				if (symbol < 16) {
					lengths[index++] = static_cast<uint8_t>(symbol);
					continue;
				}
				uint8_t repeated = 0;
				uint32_t count;
				if (symbol == 16) {
					if (index == 0) {
						failed = true;
						return;
					}
					repeated = lengths[index - 1];
					count = 3 + Bits(2);
				} else {
					count = symbol == 17 ? 3 + Bits(3) : 11 + Bits(7);
				}
				if (failed || index + count > lengthCount + distanceCount) {
					failed = true;
					return;
				}
				std::fill_n(lengths + index, count, repeated);
				index += count;
			}

			Huffman literals, distances;
			literals.Build(lengths, lengthCount);
			distances.Build(lengths + lengthCount, distanceCount);
			Blocks(literals, distances);
		}

		std::span<const uint8_t> source;
		std::span<uint8_t> out;
		size_t position = 0;
		size_t written = 0;
		uint32_t bitBuffer = 0;
		uint32_t bitCount = 0;
		bool failed = false;
	};

	/// The contents of a record, inflated into `scratch` when compressed (only its first FieldLimit bytes).
	std::span<const uint8_t> RecordBody(const uint8_t* record, uint32_t size, uint32_t flags, std::vector<uint8_t>& scratch)
	{
		const uint8_t* data = record + RecordHeaderSize;
		if (!(flags & CompressedFlag))
			return { data, size };
		// The inflated size, then a zlib stream: its two header bytes, the DEFLATE data, a checksum left unread.
		constexpr uint32_t Preamble = sizeof(uint32_t) + 2;
		if (size <= Preamble)
			return {};
		const size_t inflated = std::min<size_t>(Read<uint32_t>(data), FieldLimit);
		scratch.resize(inflated);
		const size_t written = Inflater({ data + Preamble, size - Preamble }, scratch).Run();
		return { scratch.data(), written };
	}

	/// Calls `visit(tag, field)` for each field of a record body. A field cut off by FieldLimit comes shortened.
	template <class Visit>
	void ForEachField(std::span<const uint8_t> body, Visit&& visit)
	{
		constexpr size_t FieldHeaderSize = 6;
		size_t offset = 0;
		uint32_t largeSize = 0;
		while (offset + FieldHeaderSize <= body.size()) {
			const uint32_t tag = Read<uint32_t>(body.data() + offset);
			uint32_t size = Read<uint16_t>(body.data() + offset + 4);
			offset += FieldHeaderSize;
			// XXXX carries the real size of the next field, for those over 64 KiB.
			if (largeSize) {
				size = largeSize;
				largeSize = 0;
			}
			const size_t available = std::min<size_t>(size, body.size() - offset);
			if (tag == Tag("XXXX") && available >= sizeof(uint32_t))
				largeSize = Read<uint32_t>(body.data() + offset);
			else
				visit(tag, body.subspan(offset, available));
			offset += available;
		}
	}

	struct CellKey
	{
		static uint64_t Make(int32_t x, int32_t y) { return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y); }
		static int32_t X(uint64_t key) { return static_cast<int32_t>(key >> 32); }
		static int32_t Y(uint64_t key) { return static_cast<int32_t>(key & 0xFFFFFFFFu); }
	};

	using CellHeightMap = std::unordered_map<uint64_t, std::vector<float>>;

	/// Collects the LAND records under a worldspace's children group.
	class WorldReader
	{
	public:
		WorldReader(CellHeightMap& target, std::stop_token token) :
			cells(target), stop(token) {}

		void Walk(const uint8_t* p, const uint8_t* end)
		{
			while (static_cast<size_t>(end - p) >= RecordHeaderSize) {
				const uint32_t tag = Read<uint32_t>(p);
				const uint32_t size = Read<uint32_t>(p + 4);
				if (tag == Tag("GRUP")) {
					if (size < RecordHeaderSize || size > static_cast<size_t>(end - p))
						return;
					const int32_t type = Read<int32_t>(p + 12);
					if (type == ExteriorBlock && stop.stop_requested())
						return;
					// Persistent and visible distant children never hold a LAND.
					if (type == ExteriorBlock || type == ExteriorSubBlock || type == CellChildren || type == TemporaryChildren)
						Walk(p + RecordHeaderSize, p + size);
					p += size;
					continue;
				}
				if (size > static_cast<size_t>(end - p) - RecordHeaderSize)
					return;
				const uint32_t flags = Read<uint32_t>(p + 8);
				if (tag == Tag("CELL"))
					ReadCell(p, size, flags);
				else if (tag == Tag("LAND") && !(flags & DeletedFlag))
					ReadLand(p, size, flags);
				p += RecordHeaderSize + size;
			}
		}

	private:
		/// A cell's children follow its record, so the LAND met next belongs to the last CELL read.
		void ReadCell(const uint8_t* record, uint32_t size, uint32_t flags)
		{
			hasCell = false;
			ForEachField(RecordBody(record, size, flags, scratch), [&](uint32_t tag, std::span<const uint8_t> field) {
				if (tag == Tag("XCLC") && field.size() >= 2 * sizeof(int32_t)) {
					cellX = Read<int32_t>(field.data());
					cellY = Read<int32_t>(field.data() + sizeof(int32_t));
					hasCell = true;
				}
			});
		}

		void ReadLand(const uint8_t* record, uint32_t size, uint32_t flags)
		{
			if (!hasCell)
				return;
			ForEachField(RecordBody(record, size, flags, scratch), [&](uint32_t tag, std::span<const uint8_t> field) {
				if (tag != Tag("VHGT") || field.size() < HeightFieldSize)
					return;
				// Each vertex is a delta on its west neighbour, the first of a row on the row below's first.
				float rowStart = Read<float>(field.data());
				const auto* deltas = reinterpret_cast<const int8_t*>(field.data() + sizeof(float));
				std::vector<float> heights(CellHeights);
				for (uint32_t y = 0; y < CellVertices; ++y) {
					rowStart += deltas[y * CellVertices];
					float height = rowStart;
					heights[y * CellVertices] = height * HeightUnit;
					for (uint32_t x = 1; x < CellVertices; ++x) {
						height += deltas[y * CellVertices + x];
						heights[y * CellVertices + x] = height * HeightUnit;
					}
				}
				cells[CellKey::Make(cellX, cellY)] = std::move(heights);
			});
		}

		CellHeightMap& cells;
		std::stop_token stop;
		std::vector<uint8_t> scratch;
		int32_t cellX = 0, cellY = 0;
		bool hasCell = false;
	};

	/// A plugin mapped into memory (read-only): the records are scattered, and most of the file is never touched.
	class MappedFile
	{
	public:
		explicit MappedFile(const std::filesystem::path& path)
		{
			file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return;
			LARGE_INTEGER fileSize{};
			if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0)
				return;
			mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
			if (!mapping)
				return;
			view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
			if (view)
				size = static_cast<size_t>(fileSize.QuadPart);
		}

		~MappedFile()
		{
			if (view)
				UnmapViewOfFile(view);
			if (mapping)
				CloseHandle(mapping);
			if (file != INVALID_HANDLE_VALUE)
				CloseHandle(file);
		}

		MappedFile(const MappedFile&) = delete;
		MappedFile& operator=(const MappedFile&) = delete;

		const uint8_t* Begin() const { return static_cast<const uint8_t*>(view); }
		const uint8_t* End() const { return Begin() + size; }
		bool Valid() const { return size > RecordHeaderSize; }

	private:
		HANDLE file = INVALID_HANDLE_VALUE;
		HANDLE mapping = nullptr;
		void* view = nullptr;
		size_t size = 0;
	};

	/// The plugins a file's form IDs index: its masters in order, then the file itself.
	std::vector<std::string> ReadMasters(const uint8_t* header, uint32_t size, uint32_t flags, const std::string& self)
	{
		std::vector<std::string> names;
		std::vector<uint8_t> scratch;
		ForEachField(RecordBody(header, size, flags, scratch), [&](uint32_t tag, std::span<const uint8_t> field) {
			if (tag == Tag("MAST") && !field.empty())
				names.emplace_back(reinterpret_cast<const char*>(field.data()), strnlen(reinterpret_cast<const char*>(field.data()), field.size()));
		});
		names.push_back(self);
		return names;
	}

	void ReadPlugin(const std::filesystem::path& path, const PBRWaterLand::Request& request, CellHeightMap& cells, std::stop_token stop)
	{
		const MappedFile mapped(path);
		if (!mapped.Valid())
			return;
		const uint8_t* p = mapped.Begin();
		const uint8_t* end = mapped.End();
		if (Read<uint32_t>(p) != Tag("TES4"))
			return;
		const uint32_t headerSize = Read<uint32_t>(p + 4);
		if (headerSize > static_cast<size_t>(end - p) - RecordHeaderSize)
			return;
		const auto files = ReadMasters(p, headerSize, Read<uint32_t>(p + 8), path.filename().string());
		p += RecordHeaderSize + headerSize;

		// A form ID's top byte indexes the file's own master list, so the worldspace is compared by plugin and local ID.
		auto isWorld = [&](uint32_t formID) {
			const uint32_t index = formID >> 24;
			return index < files.size() && (formID & 0x00FFFFFF) == request.worldSpace && _stricmp(files[index].c_str(), request.owner.c_str()) == 0;
		};

		WorldReader reader(cells, stop);
		// The top-level groups: only the worldspaces', and within it the children of this one.
		while (static_cast<size_t>(end - p) >= RecordHeaderSize && Read<uint32_t>(p) == Tag("GRUP")) {
			const uint32_t size = Read<uint32_t>(p + 4);
			if (size < RecordHeaderSize || size > static_cast<size_t>(end - p))
				return;
			if (Read<uint32_t>(p + 8) == Tag("WRLD")) {
				const uint8_t* q = p + RecordHeaderSize;
				const uint8_t* groupEnd = p + size;
				while (static_cast<size_t>(groupEnd - q) >= RecordHeaderSize) {
					const uint32_t innerSize = Read<uint32_t>(q + 4);
					if (Read<uint32_t>(q) == Tag("GRUP")) {
						if (innerSize < RecordHeaderSize || innerSize > static_cast<size_t>(groupEnd - q))
							return;
						if (Read<int32_t>(q + 12) == WorldChildren && isWorld(Read<uint32_t>(q + 8)))
							reader.Walk(q + RecordHeaderSize, q + innerSize);
						q += innerSize;
					} else {
						if (innerSize > static_cast<size_t>(groupEnd - q) - RecordHeaderSize)
							return;
						q += RecordHeaderSize + innerSize;
					}
				}
			}
			p += size;
		}
	}
}

namespace PBRWaterLand
{
	std::shared_ptr<PBRWaterModel::Bathymetry> Load(const Request& request, std::stop_token stop)
	{
		CellHeightMap cells;
		for (const auto& plugin : request.plugins) {
			if (stop.stop_requested())
				return nullptr;
			ReadPlugin(plugin, request, cells, stop);
		}
		if (cells.empty() || stop.stop_requested())
			return nullptr;

		int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
		float lowest = std::numeric_limits<float>::max(), highest = std::numeric_limits<float>::lowest();
		for (const auto& [key, heights] : cells) {
			minX = std::min(minX, CellKey::X(key));
			minY = std::min(minY, CellKey::Y(key));
			maxX = std::max(maxX, CellKey::X(key));
			maxY = std::max(maxY, CellKey::Y(key));
			const auto [lo, hi] = std::minmax_element(heights.begin(), heights.end());
			lowest = std::min(lowest, *lo);
			highest = std::max(highest, *hi);
		}

		auto grid = std::make_shared<PBRWaterModel::Bathymetry>();
		grid->width = static_cast<uint32_t>(maxX - minX + 1) * QuadsPerCell + 1;
		grid->height = static_cast<uint32_t>(maxY - minY + 1) * QuadsPerCell + 1;
		grid->minX = minX * static_cast<float>(PBRWaterModel::CellSize);
		grid->minY = minY * static_cast<float>(PBRWaterModel::CellSize);
		grid->stepX = grid->stepY = VertexSpacing;
		grid->minZ = lowest - MissingDepth;
		grid->maxZ = highest;
		grid->heights.assign(static_cast<size_t>(grid->width) * grid->height, grid->minZ);
		for (const auto& [key, heights] : cells) {
			const uint32_t originX = static_cast<uint32_t>(CellKey::X(key) - minX) * QuadsPerCell;
			const uint32_t originY = static_cast<uint32_t>(CellKey::Y(key) - minY) * QuadsPerCell;
			for (uint32_t y = 0; y < CellVertices; ++y)
				std::copy_n(&heights[y * CellVertices], CellVertices, &grid->heights[static_cast<size_t>(originY + y) * grid->width + originX]);
		}
		return grid;
	}
}
