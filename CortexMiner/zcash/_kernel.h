const char *ocl_code = R"_mrb_(
# 1 "input.cl"
# 1 "<built-in>" 1
# 1 "<built-in>" 3
# 340 "<built-in>" 3
# 1 "<command line>" 1
# 1 "<built-in>" 2
# 1 "input.cl" 2
# 1 "./param.h" 1




// Approximate log base 2 of number of elements in hash tables

// Number of rows and slots is affected by this. 20 offers the best performance
// but occasionally misses ~1% of solutions.


// Setting this to 1 might make SILENTARMY faster, see TROUBLESHOOTING.md


// Make hash tables OVERHEAD times larger than necessary to store the average
// number of elements per row. The ideal value is as small as possible to
// reduce memory usage, but not too small or else elements are dropped from the
// hash tables.
//
// The actual number of elements per row is closer to the theoretical average
// (less variance) when 20 is small. So accordingly OVERHEAD can be
// smaller.
//
// Even (as opposed to odd) values of OVERHEAD sometimes significantly decrease
// performance as they cause VRAM channel conflicts.

# 36 "./param.h"



// Length of 1 element (slot) in bytes

// Total size of hash table

// Length of Zcash block header, nonce (part of header)

// Offset of nTime in header

// Length of nonce

// Length of encoded representation of solution size

// Solution size (1344 = 0x540) represented as a compact integer, in hex

// Length of encoded solution (512 * 21 bits / 8 = 1344 bytes)

// Last N_ZERO_BYTES of nonce must be zero due to my BLAKE2B optimization

// Number of bytes Zcash needs out of Blake

// Number of wavefronts per SIMD for the Blake kernel.
// Blake is ALU-bound (beside the atomic counter being incremented) so we need
// at least 2 wavefronts per SIMD to hide the 2-clock latency of integer
// instructions. 10 is the max supported by the hw.

// Maximum number of solutions reported by kernel to host

// Length of SHA256 target


// Optional features
//#undef ENABLE_DEBUG






// An (uncompressed) solution stores (1 << 9) 32-bit values

typedef struct	sols_s
{
    uint	nr;
    uint	likely_invalids;
    uchar	valid[10];
    uint	values[10][(1 << 9)];
}		sols_t;

# 2 "input.cl" 2


# 34 "input.cl"


__constant ulong blake_iv[] =
{
    0x6a09e667f3bcc908, 0xbb67ae8584caa73b,
    0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
    0x510e527fade682d1, 0x9b05688c2b3e6c1f,
    0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
};




__kernel
void kernel_init_ht(__global char *ht)
{
    uint        tid = get_global_id(0);
    *(__global uint *)(ht + tid * ((1 << (((200 / (9 + 1)) + 1) - 20)) * 9) * 32) = 0;
}


# 79 "input.cl"

uint ht_store(uint round, __global char *ht, uint i,
        ulong xi0, ulong xi1, ulong xi2, ulong xi3)
{
    uint		row;
    __global char       *p;
    uint                cnt;

# 111 "input.cl"
    if (!(round % 2))
	row = (xi0 & 0xffff) | ((xi0 & 0xf00000) >> 4);
    else
	row = ((xi0 & 0xf0000) >> 0) |
	    ((xi0 & 0xf00) << 4) | ((xi0 & 0xf00000) >> 12) |
	    ((xi0 & 0xf) << 4) | ((xi0 & 0xf000) >> 12);



    xi0 = (xi0 >> 16) | (xi1 << (64 - 16));
    xi1 = (xi1 >> 16) | (xi2 << (64 - 16));
    xi2 = (xi2 >> 16) | (xi3 << (64 - 16));
    p = ht + row * ((1 << (((200 / (9 + 1)) + 1) - 20)) * 9) * 32;
    cnt = atomic_inc((__global uint *)p);
    if (cnt >= ((1 << (((200 / (9 + 1)) + 1) - 20)) * 9))
        return 1;
    p += cnt * 32 + (8 + ((round) / 2) * 4);
    // store "i" (always 4 bytes before Xi)
    *(__global uint *)(p - 4) = i;
    if (round == 0 || round == 1)
    {
	// store 24 bytes
	*(__global ulong *)(p + 0) = xi0;
	*(__global ulong *)(p + 8) = xi1;
	*(__global ulong *)(p + 16) = xi2;
    }
    else if (round == 2)
    {
	// store 20 bytes
	*(__global uint *)(p + 0) = xi0;
	*(__global ulong *)(p + 4) = (xi0 >> 32) | (xi1 << 32);
	*(__global ulong *)(p + 12) = (xi1 >> 32) | (xi2 << 32);
    }
    else if (round == 3)
    {
	// store 16 bytes
	*(__global uint *)(p + 0) = xi0;
	*(__global ulong *)(p + 4) = (xi0 >> 32) | (xi1 << 32);
	*(__global uint *)(p + 12) = (xi1 >> 32);
    }
    else if (round == 4)
    {
	// store 16 bytes
	*(__global ulong *)(p + 0) = xi0;
	*(__global ulong *)(p + 8) = xi1;
    }
    else if (round == 5)
    {
	// store 12 bytes
	*(__global ulong *)(p + 0) = xi0;
	*(__global uint *)(p + 8) = xi1;
    }
    else if (round == 6 || round == 7)
    {
	// store 8 bytes
	*(__global uint *)(p + 0) = xi0;
	*(__global uint *)(p + 4) = (xi0 >> 32);
    }
    else if (round == 8)
    {
	// store 4 bytes
	*(__global uint *)(p + 0) = xi0;
    }
    return 0;
}


# 186 "input.cl"









__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void kernel_round0(__global ulong *blake_state, __global char *ht,
        __global uint *debug)
{
    uint                tid = get_global_id(0);
    ulong               v[16];
    uint                inputs_per_thread = (1 << (200 / (9 + 1))) / get_global_size(0);
    uint                input = tid * inputs_per_thread;
    uint                input_end = (tid + 1) * inputs_per_thread;
    uint                dropped = 0;
    while (input < input_end)
    {
        // shift "i" to occupy the high 32 bits of the second ulong word in the
        // message block
        ulong word1 = (ulong)input << 32;
        // init vector v
        v[0] = blake_state[0];
        v[1] = blake_state[1];
        v[2] = blake_state[2];
        v[3] = blake_state[3];
        v[4] = blake_state[4];
        v[5] = blake_state[5];
        v[6] = blake_state[6];
        v[7] = blake_state[7];
        v[8] =  blake_iv[0];
        v[9] =  blake_iv[1];
        v[10] = blake_iv[2];
        v[11] = blake_iv[3];
        v[12] = blake_iv[4];
        v[13] = blake_iv[5];
        v[14] = blake_iv[6];
        v[15] = blake_iv[7];
        // mix in length of data
        v[12] ^= 140 + 4 ;
        // last block
        v[14] ^= (ulong)-1;

        // round 1
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  word1);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 2
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  word1);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 3
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  word1);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 4
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  word1);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 5
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  word1);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 6
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  word1);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 7
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  word1);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 8
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  word1);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 9
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  word1);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 10
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  word1);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 11
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  word1);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;
        // round 12
        v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 32);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 24); v[0] = (v[0] +  v[4] +  0);   v[12] = rotate((  v[12] ^ v[0]), (ulong)64 - 16);  v[8] = ( v[8] +   v[12]);  v[4] = rotate(( v[4] ^  v[8]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 32);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 24); v[1] = (v[1] +  v[5] +  0);   v[13] = rotate((  v[13] ^ v[1]), (ulong)64 - 16);  v[9] = ( v[9] +   v[13]);  v[5] = rotate(( v[5] ^  v[9]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 32);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 24); v[2] = (v[2] +  v[6] +  0);  v[14] = rotate(( v[14] ^ v[2]), (ulong)64 - 16);  v[10] = ( v[10] +  v[14]);  v[6] = rotate(( v[6] ^  v[10]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 32);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 24); v[3] = (v[3] +  v[7] +  0);  v[15] = rotate(( v[15] ^ v[3]), (ulong)64 - 16);  v[11] = ( v[11] +  v[15]);  v[7] = rotate(( v[7] ^  v[11]), (ulong)64 - 63);;
        v[0] = (v[0] +  v[5] +  word1);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 32);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 24); v[0] = (v[0] +  v[5] +  0);  v[15] = rotate(( v[15] ^ v[0]), (ulong)64 - 16);  v[10] = ( v[10] +  v[15]);  v[5] = rotate(( v[5] ^  v[10]), (ulong)64 - 63);;
        v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 32);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 24); v[1] = (v[1] +  v[6] +  0);  v[12] = rotate(( v[12] ^ v[1]), (ulong)64 - 16);  v[11] = ( v[11] +  v[12]);  v[6] = rotate(( v[6] ^  v[11]), (ulong)64 - 63);;
        v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 32);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 24); v[2] = (v[2] +  v[7] +  0);   v[13] = rotate((  v[13] ^ v[2]), (ulong)64 - 16);  v[8] = ( v[8] +   v[13]);  v[7] = rotate(( v[7] ^  v[8]), (ulong)64 - 63);;
        v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 32);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 24); v[3] = (v[3] +  v[4] +  0);   v[14] = rotate((  v[14] ^ v[3]), (ulong)64 - 16);  v[9] = ( v[9] +   v[14]);  v[4] = rotate(( v[4] ^  v[9]), (ulong)64 - 63);;

        // compress v into the blake state; this produces the 50-byte hash
        // (two Xi values)
        ulong h[7];
        h[0] = blake_state[0] ^ v[0] ^ v[8];
        h[1] = blake_state[1] ^ v[1] ^ v[9];
        h[2] = blake_state[2] ^ v[2] ^ v[10];
        h[3] = blake_state[3] ^ v[3] ^ v[11];
        h[4] = blake_state[4] ^ v[4] ^ v[12];
        h[5] = blake_state[5] ^ v[5] ^ v[13];
        h[6] = (blake_state[6] ^ v[6] ^ v[14]) & 0xffff;

        // store the two Xi values in the hash table

        dropped += ht_store(0, ht, input * 2,
                h[0],
                h[1],
                h[2],
                h[3]);
        dropped += ht_store(0, ht, input * 2 + 1,
                (h[3] >> 8) | (h[4] << (64 - 8)),
                (h[4] >> 8) | (h[5] << (64 - 8)),
                (h[5] >> 8) | (h[6] << (64 - 8)),
                (h[6] >> 8));




        input++;
    }

    debug[tid * 2] = 0;
    debug[tid * 2 + 1] = dropped;

}


# 401 "input.cl"














ulong half_aligned_long(__global ulong *p, uint offset)
{
    return
    (((ulong)*(__global uint *)((__global char *)p + offset + 0)) << 0) |
    (((ulong)*(__global uint *)((__global char *)p + offset + 4)) << 32);
}




uint well_aligned_int(__global ulong *_p, uint offset)
{
    __global char *p = (__global char *)_p;
    return *(__global uint *)(p + offset);
}


# 440 "input.cl"

uint xor_and_store(uint round, __global char *ht_dst, uint row,
	uint slot_a, uint slot_b, __global ulong *a, __global ulong *b)
{
    ulong	xi0, xi1, xi2;

    // Note: for 20 == 20, for odd rounds, we could optimize by not
    // storing the byte containing bits from the previous (200 / (9 + 1)) block for
    if (round == 1 || round == 2)
      {
	// xor 24 bytes
	xi0 = *(a++) ^ *(b++);
	xi1 = *(a++) ^ *(b++);
	xi2 = *a ^ *b;
	if (round == 2)
	  {
	    // skip padding byte
	    xi0 = (xi0 >> 8) | (xi1 << (64 - 8));
	    xi1 = (xi1 >> 8) | (xi2 << (64 - 8));
	    xi2 = (xi2 >> 8);
	  }
      }
    else if (round == 3)
      {
	// xor 20 bytes
	xi0 = half_aligned_long(a, 0) ^ half_aligned_long(b, 0);
	xi1 = half_aligned_long(a, 8) ^ half_aligned_long(b, 8);
	xi2 = well_aligned_int(a, 16) ^ well_aligned_int(b, 16);
      }
    else if (round == 4 || round == 5)
      {
	// xor 16 bytes
	xi0 = half_aligned_long(a, 0) ^ half_aligned_long(b, 0);
	xi1 = half_aligned_long(a, 8) ^ half_aligned_long(b, 8);
	xi2 = 0;
	if (round == 4)
	  {
	    // skip padding byte
	    xi0 = (xi0 >> 8) | (xi1 << (64 - 8));
	    xi1 = (xi1 >> 8);
	  }
      }
    else if (round == 6)
      {
	// xor 12 bytes
	xi0 = *a++ ^ *b++;
	xi1 = *(__global uint *)a ^ *(__global uint *)b;
	xi2 = 0;
	if (round == 6)
	  {
	    // skip padding byte
	    xi0 = (xi0 >> 8) | (xi1 << (64 - 8));
	    xi1 = (xi1 >> 8);
	  }
      }
    else if (round == 7 || round == 8)
      {
	// xor 8 bytes
	xi0 = half_aligned_long(a, 0) ^ half_aligned_long(b, 0);
	xi1 = 0;
	xi2 = 0;
	if (round == 8)
	  {
	    // skip padding byte
	    xi0 = (xi0 >> 8);
	  }
      }
    // invalid solutions (which start happenning in round 5) have duplicate
    // inputs and xor to zero, so discard them
    if (!xi0 && !xi1)
	return 0;



    return ht_store(round, ht_dst, ((row << 12) | (( slot_b & 0x3f) << 6) | ( slot_a & 0x3f)),
	    xi0, xi1, xi2, 0);
}





void equihash_round(uint round, __global char *ht_src, __global char *ht_dst,
	__global uint *debug)
{
    uint                tid = get_global_id(0);
    uint		tlid = get_local_id(0);
    __global char       *p;
    uint                cnt;
    uchar		first_words[((1 << (((200 / (9 + 1)) + 1) - 20)) * 9)];
    uchar		mask;
    uint                i, j;
    // ((1 << (((200 / (9 + 1)) + 1) - 20)) * 9) is already oversized (by a factor of 9), but we want to
    // make it even larger
    ushort		collisions[((1 << (((200 / (9 + 1)) + 1) - 20)) * 9) * 3];
    uint                nr_coll = 0;
    uint                n;
    uint		dropped_coll = 0;
    uint		dropped_stor = 0;
    __global ulong      *a, *b;
    uint		xi_offset;
    // read first words of Xi from the previous (round - 1) hash table
    xi_offset = (8 + ((round - 1) / 2) * 4);
    // the mask is also computed to read data from the previous round







    mask = 0; 



    p = (ht_src + tid * ((1 << (((200 / (9 + 1)) + 1) - 20)) * 9) * 32);
    cnt = *(__global uint *)p;
    cnt = min(cnt, (uint)((1 << (((200 / (9 + 1)) + 1) - 20)) * 9)); // handle possible overflow in prev. round
    if (!cnt)
	// no elements in row, no collisions
	return ;

    p += xi_offset;
    for (i = 0; i < cnt; i++, p += 32)
        first_words[i] = *(__global uchar *)p;

    // find collisions
    for (i = 0; i < cnt; i++)
        for (j = i + 1; j < cnt; j++)

            if ((first_words[i] & mask) ==
		    (first_words[j] & mask))
              {
                // collision!
                if (nr_coll >= sizeof (collisions) / sizeof (*collisions))
                    dropped_coll++;
                else

                    // note: this assumes slots can be encoded in 8 bits
                    collisions[nr_coll++] =
			((ushort)j << 8) | ((ushort)i & 0xff);



              }
    // XOR colliding pairs of Xi
    for (n = 0; n < nr_coll; n++)
      {
        i = collisions[n] & 0xff;
        j = collisions[n] >> 8;



        a = (__global ulong *)
            (ht_src + tid * ((1 << (((200 / (9 + 1)) + 1) - 20)) * 9) * 32 + i * 32 + xi_offset);
        b = (__global ulong *)
            (ht_src + tid * ((1 << (((200 / (9 + 1)) + 1) - 20)) * 9) * 32 + j * 32 + xi_offset);
	dropped_stor += xor_and_store(round, ht_dst, tid, i, j, a, b);
      }
    if (round < 8)
	// reset the counter in preparation of the next round
	*(__global uint *)(ht_src + tid * ((1 << (((200 / (9 + 1)) + 1) - 20)) * 9) * 32) = 0;

    debug[tid * 2] = dropped_coll;
    debug[tid * 2 + 1] = dropped_stor;

}





    __kernel __attribute__((reqd_work_group_size(64, 1, 1))) void kernel_round1 (__global char *ht_src, __global char *ht_dst,__global uint *debug) { equihash_round(1, ht_src, ht_dst, debug);}
    __kernel __attribute__((reqd_work_group_size(64, 1, 1))) void kernel_round2 (__global char *ht_src, __global char *ht_dst,__global uint *debug) { equihash_round(2, ht_src, ht_dst, debug);}
    __kernel __attribute__((reqd_work_group_size(64, 1, 1))) void kernel_round3 (__global char *ht_src, __global char *ht_dst,__global uint *debug) { equihash_round(3, ht_src, ht_dst, debug);}
    __kernel __attribute__((reqd_work_group_size(64, 1, 1))) void kernel_round4 (__global char *ht_src, __global char *ht_dst,__global uint *debug) { equihash_round(4, ht_src, ht_dst, debug);}
    __kernel __attribute__((reqd_work_group_size(64, 1, 1))) void kernel_round5 (__global char *ht_src, __global char *ht_dst,__global uint *debug) { equihash_round(5, ht_src, ht_dst, debug);}
    __kernel __attribute__((reqd_work_group_size(64, 1, 1))) void kernel_round6 (__global char *ht_src, __global char *ht_dst,__global uint *debug) { equihash_round(6, ht_src, ht_dst, debug);}
    __kernel __attribute__((reqd_work_group_size(64, 1, 1))) void kernel_round7 (__global char *ht_src, __global char *ht_dst,__global uint *debug) { equihash_round(7, ht_src, ht_dst, debug);}

// kernel_round8 takes an extra argument, "sols"
__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void kernel_round8(__global char *ht_src, __global char *ht_dst,
	__global uint *debug, __global sols_t *sols)
{
    uint                tid = get_global_id(0);
    equihash_round(8, ht_src, ht_dst, debug);
    if (!tid){
	    sols->nr = sols->likely_invalids = 0;
    }
}






uint expand_ref(__global char *ht, uint xi_offset, uint row, uint slot)
{
    return *(__global uint *)(ht + row * ((1 << (((200 / (9 + 1)) + 1) - 20)) * 9) * 32 +
	    slot * 32 + xi_offset - 4);
}






uint expand_refs(uint *ins, uint nr_inputs, __global char **htabs,
	uint round)
{
    __global char	*ht = htabs[round % 2];
    uint		i = nr_inputs - 1;
    uint		j = nr_inputs * 2 - 1;
    uint		xi_offset = (8 + ((round) / 2) * 4);
    int			dup_to_watch = -1;
    do
    {
        ins[j] = expand_ref(ht, xi_offset,
            (ins[i] >> 12), ((ins[i] >> 6) & 0x3f));
        ins[j - 1] = expand_ref(ht, xi_offset,
            (ins[i] >> 12), (ins[i] & 0x3f));
        if (!round)
        {
            if (dup_to_watch == -1)
            dup_to_watch = ins[j];
            else if (ins[j] == dup_to_watch || ins[j - 1] == dup_to_watch)
            return 0;
        }
        if (!i)
            break ;
        i--;
        j -= 2;
    }
    while (1);
    return 1;
}






void potential_sol(__global char **htabs, __global sols_t *sols,
	uint ref0, uint ref1)
{
    uint	nr_values;
    uint	values_tmp[(1 << 9)];
    uint	sol_i;
    uint	i;
    nr_values = 0;
    values_tmp[nr_values++] = ref0;
    values_tmp[nr_values++] = ref1;
    uint round = 9 - 1;
    do
    {
	    round--;
	    if (!expand_refs(values_tmp, nr_values, htabs, round))
	        return ;
	    nr_values *= 2;
    }
    while (round > 0);
    
    // solution appears valid, copy it to sols
    sol_i = atomic_inc(&sols->nr);
    //if (sol_i >= 1)
	//    return ;
    uint t = 1<<9;
    



    memcpy(sols->values[sol_i], values_tmp, sizeof(uint)*t);
    sols->valid[sol_i] = 1;
}




__kernel
void kernel_sols(__global char *ht0, __global char *ht1, __global sols_t *sols)
{
    uint		tid = get_global_id(0);

    __global char	*htabs[2] = { ht0, ht1 };
    uint		ht_i = (9 - 1) % 2; // table filled at last round
    uint		cnt;
    uint		xi_offset = (8 + ((9 - 1) / 2) * 4);
    uint		i, j;
    __global char	*a, *b;
    uint		ref_i, ref_j;
    // it is ok for the collisions array to be so small, as if it fills up
    // the potential solutions are likely invalid (many duplicate inputs)
    ulong		collisions[1];
    uint		coll;

    // in the final hash table, we are looking for a match on both the bits
    // part of the previous (200 / (9 + 1)) colliding bits, and the last (200 / (9 + 1)) bits.
    uint		mask = 0xffffff;



    a = htabs[ht_i] + tid * ((1 << (((200 / (9 + 1)) + 1) - 20)) * 9) * 32;
    cnt = *(__global uint *)a;
    cnt = min(cnt, (uint)((1 << (((200 / (9 + 1)) + 1) - 20)) * 9)); // handle possible overflow in last round
    coll = 0;
    a += xi_offset;
    for (i = 0; i < cnt; i++, a += 32)
	for (j = i + 1, b = a + 32; j < cnt; j++, b += 32)
	    if (((*(__global uint *)a) & mask) ==
		    ((*(__global uint *)b) & mask))
	    {
            ref_i = *(__global uint *)(a - 4);
            ref_j = *(__global uint *)(b - 4);
            if (coll < sizeof (collisions) / sizeof (*collisions))
                collisions[coll++] = ((ulong)ref_i << 32) | ref_j;
            else
                atomic_inc(&sols->likely_invalids);
	    }
    if (!coll)
	    return ;
    for (i = 0; i < coll; i++)
	    potential_sol(htabs, sols, collisions[i] >> 32, collisions[i] & 0xffffffff);
}

)_mrb_";