//
//  HeapLookup.c
//  LiquidCoreiOS
//
/*
 Copyright (c) 2018 Eric Lange. All rights reserved.
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 
 - Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 
 - Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "HeapObjects.h"

namespace V82JSC_HeapObject {

static uint64_t __two[] =
{
     1,  4,  4,  4,  1, 16, 16, 16,  1, 16, 16, 16,  1, 16, 16, 16,
     1,  4,  4,  4,  1, 64, 64, 64,  1, 64, 64, 64,  1, 64, 64, 64,
     1,  4,  4,  4,  1, 64, 64, 64,  1, 64, 64, 64,  1, 64, 64, 64,
     1,  4,  4,  4,  1, 64, 64, 64,  1, 64, 64, 64,  1, 64, 64, 64,
     1,  4,  4,  4,  1, 16, 16, 16,  1, 16, 16, 16,  1, 16, 16, 16,
     1,  4,  4,  4,  1,  0,  0,  0,  1,  0,  0,  0,  1,  0,  0,  0,
     1,  4,  4,  4,  1,  0,  0,  0,  1,  0,  0,  0,  1,  0,  0,  0,
     1,  4,  4,  4,  1,  0,  0,  0,  1,  0,  0,  0,  1,  0,  0,  0,
     1,  4,  4,  4,  1, 16, 16, 16,  1, 16, 16, 16,  1, 16, 16, 16,
     1,  4,  4,  4,  1,  0,  0,  0,  1,  0,  0,  0,  1,  0,  0,  0,
     1,  4,  4,  4,  1,  0,  0,  0,  1,  0,  0,  0,  1,  0,  0,  0,
     1,  4,  4,  4,  1,  0,  0,  0,  1,  0,  0,  0,  1,  0,  0,  0,
     1,  4,  4,  4,  1, 16, 16, 16,  1, 16, 16, 16,  1, 16, 16, 16,
     1,  4,  4,  4,  1,  0,  0,  0,  1,  0,  0,  0,  1,  0,  0,  0,
     1,  4,  4,  4,  1,  0,  0,  0,  1,  0,  0,  0,  1,  0,  0,  0,
     1,  4,  4,  4,  1,  0,  0,  0,  1,  0,  0,  0,  1,  0,  0,  0,
};
static uint8_t __trailing_four[] =
{
    0, 0, 0, 0, 1, 1, 1, 1, 1, 0
};
static uint8_t __leading_four[] =
{
    0, 0, 0, 0, 16, 16, 16, 16, 16, 0
};
    
static uint64_t __eight[] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 0
};

uint8_t one(uint64_t b)
{
    return ffsll(~b);
}
uint8_t two(uint64_t b)
{
    uint8_t *bytes = (uint8_t *) &b;
    uint64_t mask =
        __two[bytes[0]]  +
        (__two[bytes[1]] << 8)  +
        (__two[bytes[2]] << 16) +
        (__two[bytes[3]] << 24) +
        (__two[bytes[4]] << 32) +
        (__two[bytes[5]] << 40) +
        (__two[bytes[6]] << 48) +
        (__two[bytes[7]] << 56);
    return ffsll(mask);
}
uint8_t four(uint64_t b)
{
    auto padt = [](uint8_t n) -> uint64_t
    {
        return __trailing_four[__builtin_ctz(0x100 + n)];
    };
    auto padl = [](uint8_t n) -> uint64_t
    {
        int nn = ((((int) n) << 1) + 1) << ((sizeof(int) - sizeof(uint8_t)) * 8 - 1);
        return __leading_four[__builtin_clz(nn)];
    };
    uint8_t *bytes = (uint8_t *) &b;
    uint64_t mask =
        (padl(bytes[0]) + padt(bytes[0]))  +
        ((padl(bytes[1]) + padt(bytes[1])) << 8 ) +
        ((padl(bytes[2]) + padt(bytes[1])) << 16)  +
        ((padl(bytes[3]) + padt(bytes[3])) << 24) +
        ((padl(bytes[4]) + padt(bytes[4])) << 32) +
        ((padl(bytes[5]) + padt(bytes[5])) << 40) +
        ((padl(bytes[6]) + padt(bytes[6])) << 48) +
        ((padl(bytes[7]) + padt(bytes[7])) << 56);
    return ffsll(mask);
}
uint8_t eight(uint64_t b)
{
    auto pad = [](uint8_t n) -> uint64_t
    {
        return __eight[__builtin_ctz(0x100 + n)];
    };
    uint8_t *bytes = (uint8_t *) &b;
    uint64_t mask =
        pad(bytes[0]) +
        (pad(bytes[1]) << 8)  +
        (pad(bytes[2]) << 16) +
        (pad(bytes[3]) << 24) +
        (pad(bytes[4]) << 32) +
        (pad(bytes[5]) << 40) +
        (pad(bytes[6]) << 48) +
        (pad(bytes[7]) << 56);
    return ffsll(mask);
}
uint8_t sixteen(uint64_t b)
{
    auto pad = [](uint8_t n) -> uint64_t
    {
        return __eight[__builtin_ctz(0x100 + n)];
    };
    uint8_t *bytes = (uint8_t *) &b;
    uint64_t mask =
        (pad(bytes[0]) & pad(bytes[1])) +
        ((pad(bytes[2]) & pad(bytes[3])) << 16) +
        ((pad(bytes[4]) & pad(bytes[5])) << 32) +
        ((pad(bytes[6]) & pad(bytes[7])) << 48);
    return ffsll(mask);
}
uint8_t thirtytwo(uint64_t b)
{
    auto pad = [](uint8_t n) -> uint64_t
    {
        return __eight[__builtin_ctz(0x100 + n)];
    };
    uint8_t *bytes = (uint8_t *) &b;
    uint64_t mask =
        (pad(bytes[0]) & pad(bytes[1]) & pad(bytes[2]) & pad(bytes[3])) +
        ((pad(bytes[4]) & pad(bytes[5]) & pad(bytes[6]) & pad(bytes[7])) << 32);
    return ffsll(mask);
}
uint8_t sixtyfour(uint64_t b)
{
    auto pad = [](uint8_t n) -> uint64_t
    {
        return __eight[__builtin_ctz(0x100 + n)];
    };
    uint8_t *bytes = (uint8_t *) &b;
    uint64_t mask =
        pad(bytes[0]) & pad(bytes[1]) & pad(bytes[2]) & pad(bytes[3]) &
        pad(bytes[4]) & pad(bytes[5]) & pad(bytes[6]) & pad(bytes[7]);
    return ffsll(mask);
}

typedef uint8_t (*Transform)(uint64_t);

static Transform transforms[] = {
    [](uint64_t) -> uint8_t { return 1; },
    one,
    two,
    four,
    eight,
    sixteen,
    thirtytwo,
    sixtyfour
};

Transform transform (uint32_t slots)
{
    return transforms[__builtin_ctz(slots) + 1];
}

}
