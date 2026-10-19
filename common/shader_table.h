// Which game shader a compiled shader is, where it writes each of its parameters, and which
// checkboxes it was compiled with.
//
// Made on the user's machine from the game's own .vcs files (shader_table_build.h). A shader seen at
// runtime is found by its DXBC checksum, which names the variant, and the variant names every
// parameter it writes: its register type, its variable type and its offset, a dword in the
// material's constant buffer or a texture slot. The variant's static combo says which of the
// shader's checkboxes it was built with, S_TRANSLUCENT, S_DETAIL_TEXTURE and the rest, each
// following the material's own F_ feature.

#pragma once
#include <stddef.h>
#include <stdint.h>

// Where the table is. Asked for again, at most once a second, until it is there and reads.
void ShaderDbSetPath(const wchar_t* path);
// Whether it is loaded (trying to load it if not).
bool ShaderDbReady();

// Register types, as the .vcs files number them.
enum : uint8_t {
    SDB_REG_BOOL = 1,
    SDB_REG_INT4 = 2,
    SDB_REG_FLOAT4 = 3,
    SDB_REG_TEXTURE = 4,
    SDB_REG_SAMPLER = 6,
};

// Variable types, as the .vcs files number them. The rest are samplers of other shapes and
// buffers, which a material does not set by value.
enum : uint8_t {
    SDB_VAR_FLOAT = 1, SDB_VAR_FLOAT2 = 2, SDB_VAR_FLOAT3 = 3, SDB_VAR_FLOAT4 = 4,
    SDB_VAR_INT = 5, SDB_VAR_INT2 = 6, SDB_VAR_INT3 = 7, SDB_VAR_INT4 = 8,
    SDB_VAR_BOOL = 9, SDB_VAR_BOOL2 = 10, SDB_VAR_BOOL3 = 11, SDB_VAR_BOOL4 = 12,
    SDB_VAR_SAMPLER2D = 14, SDB_VAR_SAMPLER3D = 15, SDB_VAR_SAMPLERCUBE = 16,
};

// How many dwords a value of this variable type fills, 0 for what is not a value.
int ShaderDbVarDwords(uint8_t varType);
const char* ShaderDbVarTypeName(uint8_t varType);

// The layout of a compiled shader, found by its bytecode, or -1 when the table does not have it
// or is not there. psFlags, when asked for, gets the pixel shader's render state as the table has
// it: bit 0 alpha to coverage, bit 1 blending, bit 2 depth written, bits 4-8 and 9-13 the source
// and destination blend, bit 14 no blend state recorded. combo, when asked for, gets its
// checkboxes, see ShaderDbCombo*, or -1.
int ShaderDbLookup(const void* bytecode, size_t size, uint16_t* psFlags, int* combo = nullptr);

// The game shader a layout belongs to, "vr_complex" and the like, or "".
const char* ShaderDbName(int layout);

// Where a layout writes a parameter, by its name. False when this variant does not write it.
bool ShaderDbFind(int layout, const char* name, uint8_t* regType, uint16_t* offset,
                  uint8_t* varType = nullptr);

// Every field of a layout, in the order the variant writes them.
int ShaderDbFieldCount(int layout);
bool ShaderDbField(int layout, int index, const char** name, uint8_t* regType, uint16_t* offset,
                   uint8_t* varType = nullptr);

// A variant's checkboxes. Each axis has its own name (S_...), the material feature it follows
// (F_..., "" when none) and, for a list, the names of its states joined by '|' ("" when it is a
// plain on and off). The value is -2 where variants sharing the bytecode disagree.
int ShaderDbComboAxisCount(int combo);
bool ShaderDbComboAxis(int combo, int index, const char** axis, const char** feature,
                       const char** stateNames, int* value);
// The value of one checkbox by its S_ or F_ name, -1 when the variant has no such axis and -2
// when it cannot say.
int ShaderDbComboValue(int combo, const char* name);

// The same, keyed by hash. A name's hash is the 32 bit FNV-1a of it, and the load says so if two
// names in the table share one.
uint32_t ShaderDbHash(const char* name);
// The hash of the game shader a layout belongs to, 0 when there is none.
uint32_t ShaderDbNameHash(int layout);
bool ShaderDbFieldHashed(int layout, int index, uint32_t* nameHash, uint8_t* regType,
                         uint16_t* offset, uint8_t* varType);
// featureHash is 0 when the axis follows no feature.
bool ShaderDbComboAxisHashed(int combo, int index, uint32_t* axisHash, uint32_t* featureHash,
                             int* value);
