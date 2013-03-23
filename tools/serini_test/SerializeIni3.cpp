/* Copyright 2013 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "BaseUtil.h"
#include "SerializeIni3.h"

#include "AppPrefs3.h"
#include "IniParser.h"

namespace serini3 {

static int64_t ParseBencInt(const char *bytes)
{
    bool negative = *bytes == '-';
    if (negative)
        bytes++;
    int64_t value = 0;
    for (; str::IsDigit(*bytes); bytes++) {
        value = value * 10 + (*bytes - '0');
        if (value - (negative ? 1 : 0) < 0)
            return 0;
    }
    return negative ? -value : value;
}

static char *UnescapeStr(const char *s)
{
    if (!str::StartsWith(s, "$[") || !str::EndsWith(s, "]$"))
        return str::Dup(s);
    str::Str<char> ret;
    const char *end = s + str::Len(s) - 2;
    for (const char *c = s + 2; c < end; c++) {
        if (*c != '$') {
            ret.Append(*c);
            continue;
        }
        switch (*++c) {
        case '$': ret.Append('$'); break;
        case 'n': ret.Append('\n'); break;
        case 'r': ret.Append('\r'); break;
        default: ret.Append('$'); ret.Append(*c); break;
        }
    }
    return ret.StealData();
}

static void *DeserializeRec(IniFile& ini, SettingInfo *meta, const char *sectionName=NULL, size_t idx=0)
{
    IniSection *section = ini.FindSection(sectionName, idx);
    IniLine *line = NULL;
    int r, g, b, a;

    if (idx > 0 && !(section = ini.FindSection(sectionName, idx - 1)))
        return NULL;

    uint8_t *base = (uint8_t *)calloc(1, meta[0].offset);
    for (size_t i = 1; i <= (size_t)meta[0].type; i++) {
        if (Type_Struct == meta[i].type) {
            ScopedMem<char> name(sectionName ? str::Join(sectionName, ".", meta[i].name) : str::Dup(meta[i].name));
            // TODO: reconstruct this index for nested arrays
            *(void **)(base + meta[i].offset) = DeserializeRec(ini, meta[i].substruct, name, idx);
            continue;
        }
        if (Type_Array == meta[i].type) {
            ScopedMem<char> name(sectionName ? str::Join(sectionName, ".", meta[i].name) : str::Dup(meta[i].name));
            Vec<void *> array;
            void *item;
            for (size_t j = 1; (item = DeserializeRec(ini, meta[i].substruct, name, j)); j++) {
                array.Append(item);
            }
            *(size_t *)(base + meta[i+1].offset) = array.Size();
            *(void ***)(base + meta[i].offset) = array.StealData();
            i++; // skip implicit array count field
            continue;
        }
        if (!section || !(line = section->FindLine(meta[i].name))) {
            // printf("couldn't find line for %s (%s)\n", meta[i].name, sectionName);
            continue;
        }
        switch (meta[i].type) {
        case Type_Bool:
            *(bool *)(base + meta[i].offset) = str::EqI(line->value, "true") || ParseBencInt(line->value) != 0;
            break;
        case Type_Int:
            *(int *)(base + meta[i].offset) = (int)ParseBencInt(line->value);
            break;
        case Type_Int64:
            *(int64_t *)(base + meta[i].offset) = ParseBencInt(line->value);
            break;
        case Type_Float:
            if (!str::Parse(line->value, "%f", (float *)(base + meta[i].offset)))
                *(float *)(base + meta[i].offset) = 0.f;
            break;
        case Type_Color:
            if (str::Parse(line->value, "#%2x%2x%2x%2x", &a, &r, &g, &b))
                *(COLORREF *)(base + meta[i].offset) = RGB(r, g, b) | (a << 24);
            else if (str::Parse(line->value, "#%2x%2x%2x", &r, &g, &b))
                *(COLORREF *)(base + meta[i].offset) = RGB(r, g, b);
            else
                *(COLORREF *)(base + meta[i].offset) = (COLORREF)0;
            break;
        case Type_String:
            *(WCHAR **)(base + meta[i].offset) = str::conv::FromUtf8(ScopedMem<char>(UnescapeStr(line->value)));
            break;
        case Type_Utf8String:
            *(char **)(base + meta[i].offset) = UnescapeStr(line->value);
            break;
        default:
            CrashIf(true);
        }
    }
    return base;
}

void *Deserialize(const char *data, size_t dataLen, SettingInfo *def)
{
    CrashIf(!data || str::Len(data) != dataLen);
    IniFile ini(data);
    return DeserializeRec(ini, def);
}

// only escape characters which are significant to IniParser:
// newlines and heading/trailing whitespace
static bool NeedsEscaping(const char *s)
{
    return str::IsWs(*s) || *s && str::IsWs(*(s + str::Len(s) - 1)) ||
           str::FindChar(s, '\n') || str::FindChar(s, '\r');
}

// escapes strings containing newlines or heading/trailing whitespace
static char *EscapeStr(const char *s)
{
    str::Str<char> ret;
    // use an unlikely character combination for indicating an escaped string
    ret.Append("$[");
    for (const char *c = s; *c; c++) {
        switch (*c) {
        // TODO: escape any other characters?
        case '$': ret.Append("$$"); break;
        case '\n': ret.Append("$n"); break;
        case '\r': ret.Append("$r"); break;
        default: ret.Append(*c);
        }
    }
    ret.Append("]$");
    return ret.StealData();
}

static void SerializeRec(str::Str<char>& out, const void *data, SettingInfo *meta, const char *sectionName=NULL)
{
    if (sectionName)
        out.AppendFmt("[%s]\r\n", sectionName);

    COLORREF c;
    uint8_t *base = (uint8_t *)data;
    for (size_t i = 1; i <= (size_t)meta[0].type; i++) {
        CrashIf(str::FindChar(meta[i].name, '=') || NeedsEscaping(meta[i].name));
        switch (meta[i].type) {
        case Type_Bool:
            out.AppendFmt("%s = %s\r\n", meta[i].name, *(bool *)(base + meta[i].offset) ? "true" : "false");
            break;
        case Type_Int:
            out.AppendFmt("%s = %d\r\n", meta[i].name, *(int *)(base + meta[i].offset));
            break;
        case Type_Int64:
            out.AppendFmt("%s = %I64d\r\n", meta[i].name, *(int64_t *)(base + meta[i].offset));
            break;
        case Type_Float:
            out.AppendFmt("%s = %g\r\n", meta[i].name, *(float *)(base + meta[i].offset));
            break;
        case Type_Color:
            c = *(COLORREF *)(base + meta[i].offset);
            // TODO: COLORREF doesn't really have an alpha value
            if (((c >> 24) & 0xff))
                out.AppendFmt("%s = #%02x%02x%02x%02x\r\n", meta[i].name, (c >> 24) & 0xff, GetRValue(c), GetGValue(c), GetBValue(c));
            else
                out.AppendFmt("%s = #%02x%02x%02x\r\n", meta[i].name, GetRValue(c), GetGValue(c), GetBValue(c));
            break;
        case Type_String:
            if (*(const WCHAR **)(base + meta[i].offset)) {
                ScopedMem<char> value(str::conv::ToUtf8(*(const WCHAR **)(base + meta[i].offset)));
                if (!NeedsEscaping(value))
                    out.AppendFmt("%s = %s\r\n", meta[i].name, value);
                else
                    out.AppendFmt("%s = %s\r\n", meta[i].name, ScopedMem<char>(EscapeStr(value)));
            }
            break;
        case Type_Utf8String:
            if (!*(const char **)(base + meta[i].offset))
                /* skip empty string */;
            else if (!NeedsEscaping(*(const char **)(base + meta[i].offset)))
                out.AppendFmt("%s = %s\r\n", meta[i].name, *(const char **)(base + meta[i].offset));
            else
                out.AppendFmt("%s = %s\r\n", meta[i].name, ScopedMem<char>(EscapeStr(*(const char **)(base + meta[i].offset))));
            break;
        case Type_Struct:
            // nested structs are serialized after all other values
            break;
        case Type_Array:
            // nested structs are serialized after all other values
            i++; // skip implicit array count field
            break;
        default:
            CrashIf(true);
        }
    }

    for (size_t i = 1; i <= (size_t)meta[0].type; i++) {
        if (Type_Struct == meta[i].type) {
            ScopedMem<char> name(sectionName ? str::Join(sectionName, ".", meta[i].name) : str::Dup(meta[i].name));
            SerializeRec(out, *(const void **)(base + meta[i].offset), meta[i].substruct, name);
        }
        else if (Type_Array == meta[i].type) {
            ScopedMem<char> name(sectionName ? str::Join(sectionName, ".", meta[i].name) : str::Dup(meta[i].name));
            size_t count = *(size_t *)(base + meta[i+1].offset);
            for (size_t j = 0; j < count; j++) {
                SerializeRec(out, (*(void ***)(base + meta[i].offset))[j], meta[i].substruct, name);
            }
            i++; // skip implicit array count field
        }
    }
}

char *Serialize(const void *data, SettingInfo *def, size_t *sizeOut)
{
    str::Str<char> out;
    out.Append(UTF8_BOM "; this file will be overwritten - modify at your own risk\r\n");
    SerializeRec(out, data, def);
    if (sizeOut)
        *sizeOut = out.Size();
    return out.StealData();
}

void FreeStruct(void *data, SettingInfo *meta)
{
    if (!data)
        return;
    uint8_t *base = (uint8_t *)data;
    for (size_t i = 1; i <= (size_t)meta[0].type; i++) {
        if (Type_Struct == meta[i].type)
            FreeStruct(*(void **)(base + meta[i].offset), meta[i].substruct);
        else if (Type_Array == meta[i].type) {
            size_t count = *(size_t *)(base + meta[i+1].offset);
            for (size_t j = 0; j < count; j++) {
                FreeStruct((*(void ***)(base + meta[i].offset))[j], meta[i].substruct);
            }
            free(*(void **)(base + meta[i].offset));
            i++;
        }
        else if (Type_String == meta[i].type || Type_Utf8String == meta[i].type)
            free(*(void **)(base + meta[i].offset));
    }
    free(data);
}

};