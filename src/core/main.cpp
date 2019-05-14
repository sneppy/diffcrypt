#include "coremin.h"
#include "math/math.h"
#include "containers/sorting.h"

Malloc * gMalloc = nullptr;

class BitStream
{
protected:
	/// Data buffer
	void * data;

	/// Number of bits
	uint32 count;

	/// Buffer size in bytes
	uint32 size;

public:
	/// Default constructor
	FORCE_INLINE explicit BitStream(uint32 _count = 0)
		: data{nullptr}
		, count{_count}
		, size{(((_count - 1) >> 6) + 1) << 3}
	{
		if (size) data = gMalloc->malloc(size, sizeof(uint64));
	}

protected:
	/// Reverse bit order in byte
	static FORCE_INLINE ubyte reverseByte(ubyte b)
	{
		b = (b & 0xf0) >> 4 | (b & 0x0f) << 4;
		b = (b & 0xcc) >> 2 | (b & 0x33) << 2;
		b = (b & 0xaa) >> 1 | (b & 0x55) << 1;
		return b;
	}

public:
	/// Buffer constructor
	FORCE_INLINE explicit BitStream(const void * buffer, uint32 _count)
		: count{_count}
		, size{(((_count - 1) >> 6) + 1) << 3}
	{
		// Copy data
		if ((data = gMalloc->malloc(size, sizeof(uint64))))
		#if 1
			PlatformMemory::memcpy(data, buffer, size);
		#else
			for (uint32 i = 0; i < size; ++i)
				as<ubyte>()[i] = reverseByte(reinterpret_cast<const ubyte*>(buffer)[i]);
		#endif
	}

	/// Copy constructor
	FORCE_INLINE BitStream(const BitStream & other)
		: BitStream(other.data, other.count) {}
	
	/// Move constructor
	FORCE_INLINE BitStream(BitStream && other)
		: BitStream(other.count)
	{
		if (data) gMalloc->free(data);
		data = other.data;
		
		other.data = nullptr;
	}

	/// Copy assignment
	FORCE_INLINE BitStream & operator=(const BitStream & other)
	{
		count = other.count;
		size = other.size;

		// Copy data
		if (data) gMalloc->free(data);
		if (data = gMalloc->malloc(size, sizeof(uint64)))
			PlatformMemory::memcpy(data, other.data, size);
	}

	/// Move assignment
	FORCE_INLINE BitStream & operator=(BitStream && other)
	{
		// Dealloc data
		if (data) gMalloc->free(data);

		count = other.count;
		size = other.size;
		data = other.data;

		other.data = nullptr;
	}

	/// Destructor
	FORCE_INLINE ~BitStream()
	{
		if (data) gMalloc->free(data);
	}

protected:
	/// Resize buffer if necessary
	FORCE_INLINE bool resizeIfNecessary(uint32 _size)
	{
		if (_size < size)
		{
			data = gMalloc->realloc(data, _size, sizeof(uint64));
			return true;
		}

		return false;
	}

public:
	/// Return data buffer as type
	template<typename T>
	T * as() const
	{
		return reinterpret_cast<T*>(data);
	}

	/**
	 * Xor compound operator
	 * 
	 * @param [in] other second operand
	 * @return self
	 */
	FORCE_INLINE BitStream & operator^=(const BitStream & other)
	{
		uint64 * a = as<uint64>(), * b = other.as<uint64>();
		const uint32 n = Math::min(size, other.size) >> 3;

		// 64-bit xor
		for (uint32 i = 0; i < n; ++i)
			a[i] ^= b[i];
		
		return *this;
	}

	/**
	 * Xor operator
	 * 
	 * @param [in] other second operand
	 * @return new bitstream
	 */
	FORCE_INLINE BitStream operator^(const BitStream & other) const
	{
		return BitStream(*this) ^= other;
	}

	/**
	 * Rotate left in-place
	 * 
	 * @param [in] n bits shift
	 * @return self
	 */
	FORCE_INLINE BitStream & rotateLeft(int32 n)
	{
		// Max shift
		uint32 s = Math::min(n, 8);
		
		ubyte * src = as<ubyte>();
		ubyte v = 0, u = v;
		ubyte bitmask = (1U << s) - 1;
		for (int32 i = count >> 3; i >= 0; --i)
		{
			v = u, u = src[i] >> (8 - s);
			(src[i] <<= s) |= v;
		}
		
		src[count >> 3] |= u << (8 - (count & 0x7));
		// ! Corner cases not handled
		
		return *this;
	}

	/**
	 * Shuffle using a permutation table
	 * 
	 * @param [in] dest destination bit stream
	 * @param [in] perm permutation table
	 * @return dest bitstream
	 */
	FORCE_INLINE BitStream & permute(BitStream & dest, const uint32 * perm) const
	{
		ubyte * src = as<ubyte>();
		ubyte * dst = dest.as<ubyte>();

		for (uint32 i = 0, k; (k = i << 3) < dest.count; ++i, ++dst)
		{
			for (uint32 j = 0; j < 8; ++j)
			{
				// Get bit value
				const uint32 r = perm[k + j];
				const uint32 b = r >> 3, o = (r ^ 0x7) & 0x7;
				const ubyte x = (src[b] >> o) & 0x1;

				// Push in destination
				(*dst <<= 1) |= x;
			}
		}

		return dest;
	}

	/**	
	 * Shuffle using subsitution maps
	 * 
	 * @param [in] dest destination bit stream
	 * @param [in] subs subsitution map(s)
	 * @param [in] n number of sub maps to cycle
	 * @return dest bitstream
	 * @{
	 */
	template<uint8 inSize, uint8 outSize>
	FORCE_INLINE BitStream & substitute(BitStream & dest, const uint32 * subs[], uint32 numSubs) const
	{
		/* uint64 * src = as<uint64>(), * des = dest.as<uint64>();
		uint32 i = 0, j = 0, s = 0;
		uint32 u = (1U << inSize) - 1, v = (1U << outSize) - 1;

		while (j < dest.count)
		{
			uint64 x = (*src & u) >> i;
			uint64 t = static_cast<uint64>(subs[s][x]) << j;
			(*des &= ~v) |= t;

			// Shift bitmask
			u <<= inSize, v <<= outSize;

			// Increment pointers if necessary
			src += ((i += inSize) & 0x3f) == 0;
			des += ((j += outSize) & 0x3f) == 0;

			// Next map
			s = ++s == n ? 0 : s;
		}

		return dest; */

		ubyte * src = as<ubyte>();
		ubyte * dst = dest.as<ubyte>();
		*dst = 0x0;
		
		uint16 b0 = 0, a0 = 0;
		uint16 r = (1U << inSize) - 1;

		for (uint32 i = 0, n = 8, m = 8, s = 0; i << 3 < count; ++i, n += 8)
		{
			const uint16 b = src[i] | (b0 << 8);

			while (n >= inSize)
			{
				const ubyte t = (b >> (n -= inSize)) & r;
				const ubyte x = subs[s][t];

				*dst |= x << (m -= outSize);
				if (m <= 0) (*++dst = 0) |= x << (m += 8);

				// Next sbox
				s = ++s == numSubs ? 0 : s;
			}

			b0 = b;
		}

		return dest;
	}
	template<uint32 inSize, uint32 outSize>
	FORCE_INLINE BitStream & substitute(BitStream & dest, const uint32 * subs) const
	{
		return substitute<inSize, outSize>(dest, &subs, 1);
	}
	/// @}

	/**
	 * Returns a copy of a slice of the bit stream
	 * 
	 * @param [in] _count bit count
	 * @param [in] offset initial offset (in bytes)
	 * @return bit stream slice
	 */
	FORCE_INLINE BitStream slice(uint32 n, uint32 offset = 0) const
	{
		return BitStream(as<ubyte>() + offset, n);
	}

protected:
	/**
	 * Internal code to merge two bit streams
	 */
	FORCE_INLINE void append_internal(const BitStream & other)
	{
		ubyte * dst = as<ubyte>();
		ubyte * src = other.as<ubyte>();

		ubyte r = count & 0x7;
		ubyte s = 8 - r;
		ubyte v = *src >> s;

		// ! Overwrite bits
		dst[count >> 3] |= v;

		ubyte u = 0;
		int32 i = other.count >> 3;
		dst += (count + other.count) >> 3, src += i;
		for (; i >= 0; --i, --src, --dst)
		{
			v = u, u = *src >> s;
			*dst = *src << r | v;
		}
	}

public:
	/**
	 * Append a bit stream
	 * 
	 * @param [in] other bit stream to append
	 * @return self
	 */
	FORCE_INLINE BitStream & append(const BitStream & other)
	{
		const uint32 _size = (((count + other.count - 1) >> 6) + 1) << 3;
		resizeIfNecessary(_size);

		append_internal(other);
		count += other.count;

		return *this;
	}

	/**
	 * Merge two bit streams
	 * 
	 * @param [in] other bit streams to merge
	 * @return dest bitstream
	 */
	FORCE_INLINE BitStream merge(const BitStream & other)
	{
		// Copy first bitstream
		BitStream out(*this);
		resizeIfNecessary((((count + other.count - 1) >> 6) + 1) << 3);

		// Append second bitstream
		out.append_internal(other);
		return out;
	}
};

#include <omp.h>
double start;

const uint32 ip[] = {
	57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
	61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7,
	56, 48, 40, 32, 24, 16, 8, 0, 58, 50, 42, 34, 26, 18, 10, 2,
	60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6
};
const uint32 fp[] = {
	39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30,
	37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28,
	35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26,
	33, 1, 41, 9, 49, 17, 57, 25, 32, 0, 40, 8, 48, 16, 56, 24
};
const uint32 xpn[] = {
	31, 0, 1, 2, 3, 4,
	3, 4, 5, 6, 7, 8,
	7, 8, 9, 10, 11, 12,
	11, 12, 13, 14, 15, 16,
	15, 16, 17, 18, 19, 20,
	19, 20, 21, 22, 23, 24,
	23, 24, 25, 26, 27, 28,
	27, 28, 29, 30, 31, 0
};
const uint32 perm[] = {
	15, 6, 19, 20,
	28, 11, 27, 16,
	0, 14, 22, 25,
	4, 17, 30, 9,
	1, 7, 23, 13,
	31, 26, 2, 8,
	18, 12, 29, 5,
	21, 10, 3, 24
};
const uint32 subs[8][64] = {
	{
		14, 0, 4, 15, 13, 7, 1, 4, 2, 14, 15, 2, 11, 13, 8, 1,
		3, 10, 10, 6, 6, 12, 12, 11, 5, 9, 9, 5, 0, 3, 7, 8,
		4, 15, 1, 12, 14, 8, 8, 2, 13, 4, 6, 9, 2, 1, 11, 7,
		15, 5, 12, 11, 9, 3, 7, 14, 3, 10, 10, 0, 5, 6, 0, 13
	},
	{
		15, 3, 1, 13, 8, 4, 14, 7, 6, 15, 11, 2, 3, 8, 4, 14,
		9, 12, 7, 0, 2, 1, 13, 10, 12, 6, 0, 9, 5, 11, 10, 5,
		0, 13, 14, 8, 7, 10, 11, 1, 10, 3, 4, 15, 13, 4, 1, 2,
		5, 11, 8, 6, 12, 7, 6, 12, 9, 0, 3, 5, 2, 14, 15, 9
	},
	{
		10, 13, 0, 7, 9, 0, 14, 9, 6, 3, 3, 4, 15, 6, 5, 10,
		1, 2, 13, 8, 12, 5, 7, 14, 11, 12, 4, 11, 2, 15, 8, 1,
		13, 1, 6, 10, 4, 13, 9, 0, 8, 6, 15, 9, 3, 8, 0, 7,
		11, 4, 1, 15, 2, 14, 12, 3, 5, 11, 10, 5, 14, 2, 7, 12
	},
	{
		7, 13, 13, 8, 14, 11, 3, 5, 0, 6, 6, 15, 9, 0, 10, 3,
		1, 4, 2, 7, 8, 2, 5, 12, 11, 1, 12, 10, 4, 14, 15, 9,
		10, 3, 6, 15, 9, 0, 0, 6, 12, 10, 11, 1, 7, 13, 13, 8,
		15, 9, 1, 4, 3, 5, 14, 11, 5, 12, 2, 7, 8, 2, 4, 14
	},
	{
		2, 14, 12, 11, 4, 2, 1, 12, 7, 4, 10, 7, 11, 13, 6, 1,
		8, 5, 5, 0, 3, 15, 15, 10, 13, 3, 0, 9, 14, 8, 9, 6,
		4, 11, 2, 8, 1, 12, 11, 7, 10, 1, 13, 14, 7, 2, 8, 13,
		15, 6, 9, 15, 12, 0, 5, 9, 6, 10, 3, 4, 0, 5, 14, 3
	},
	{
		12, 10, 1, 15, 10, 4, 15, 2, 9, 7, 2, 12, 6, 9, 8, 5,
		0, 6, 13, 1, 3, 13, 4, 14, 14, 0, 7, 11, 5, 3, 11, 8,
		9, 4, 14, 3, 15, 2, 5, 12, 2, 9, 8, 5, 12, 15, 3, 10,
		7, 11, 0, 14, 4, 1, 10, 7, 1, 6, 13, 0, 11, 8, 6, 13
	},
	{
		4, 13, 11, 0, 2, 11, 14, 7, 15, 4, 0, 9, 8, 1, 13, 10,
		3, 14, 12, 3, 9, 5, 7, 12, 5, 2, 10, 15, 6, 8, 1, 6,
		1, 6, 4, 11, 11, 13, 13, 8, 12, 1, 3, 4, 7, 10, 14, 7,
		10, 9, 15, 5, 6, 0, 8, 15, 0, 14, 5, 2, 9, 3, 2, 12
	},
	{
		13, 1, 2, 15, 8, 13, 4, 8, 6, 10, 15, 3, 11, 7, 1, 4,
		10, 12, 9, 5, 3, 6, 14, 11, 5, 0, 0, 14, 12, 9, 7, 2,
		7, 2, 11, 1, 4, 14, 1, 7, 9, 4, 12, 10, 14, 8, 2, 13,
		0, 15, 6, 12, 10, 9, 13, 0, 15, 3, 3, 5, 5, 6, 8, 11
	}
};

int main()
{
	Memory::createGMalloc();

	const uint32 * _subs[] = {
		subs[0], subs[1], subs[2], subs[3],
		subs[4], subs[5], subs[6], subs[7]
	};
	
	/* char ptx[] = "\x01\x23\x45\x67\x89\xab\xcd\xef";
	char key[] = "\x13\x34\x57\x79\x9b\xbc\xdf\xf1"; */
	char ptx[] = "Hello world!";
	char key[] = "SneppyRulez";

	BitStream input(ptx, 64), output(64);
	BitStream l, r;
	BitStream u(32), v(32);
	BitStream k0(key, 64), e(48);
	BitStream k[16];

	printf("input: %llx\n", input.as<uint64>()[0]);

	//////////////////////////////////////////////////
	// Key schedule
	//////////////////////////////////////////////////
	
	BitStream c(28), d(28);
	const uint32 shifts[] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
	const uint32 kperm[] = {13, 16, 10, 23, 0, 4, 2, 27, 14, 5, 20, 9, 22, 18, 11, 3, 25, 7, 15, 6, 26, 19, 12, 1, 40, 51, 30, 36, 46, 54, 29, 39, 50, 44, 32, 47, 43, 48, 38, 55, 33, 52, 45, 41, 49, 35, 28, 31}; 

	k0.permute(c, (const uint32[]){56, 48, 40, 32, 24, 16, 8, 0, 57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35});
	k0.permute(d, (const uint32[]){62, 54, 46, 38, 30, 22, 14, 6, 61, 55, 45, 37, 29, 21, 13, 5, 60, 52, 44, 36, 28, 20, 12, 4, 27, 19, 11, 3});

	printf("key = %llx\n", k0.as<uint64>()[0]);

	for (uint32 i = 0; i < 16; ++i)
	{
		new (k + i) BitStream(48);

		c.rotateLeft(shifts[i]);
		d.rotateLeft(shifts[i]);

		c.merge(d).permute(k[i], kperm);
	}

	for (uint32 i = 0; i < 1 << 18; ++i)
	{
		// Initial permutation
		input.permute(output, ip);

		// Split in left and right blocks
		l = output.slice(32), r = output.slice(32, 4);

		for (uint32 i = 0; i < 15; ++i)
		{
			// DES round
			(r.permute(e, xpn) ^= k[i]).substitute<6, 4>(u, _subs, 8).permute(v, perm) ^= l;
			l = r, r = v;
		}

		// Last round
		l ^= (r.permute(e, xpn) ^= k[15]).substitute<6, 4>(u, _subs, 8).permute(v, perm);
		l.merge(r).permute(output, fp);
	}

	printf("0x%llx\n", output.as<uint64>()[0]);

	return 0;
}