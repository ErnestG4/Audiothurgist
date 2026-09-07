#pragma once
// Tiny JSON for a FIXED schema — no allocation, no exceptions, no STL, no
// printf. Not a general JSON library: an emitter that writes into a caller
// buffer and a pull parser that walks keys, so a schema reader is a loop of
// `while(in.NextKey(k)) { if(!strcmp(k,"x")) in.Int(v); else in.Skip(); }`
// — unknown keys and extra fields are skipped, which is the forward-
// compatibility rule for the card files (docs/packages/sample-persist.md).
//
// JsonOut: every write checks capacity; on overflow `ok` drops to false and
// the buffer stays NUL-terminated (truncated — never use it). Commas are
// placed automatically (Key resets the comma state, values set it).
//
// JsonIn: bounded by [p, end) — never reads past end, never past a NUL.
// Any structural error sets `ok` false and every later call fails fast.
// Nesting of skipped values is unbounded (iterative), parsed nesting is
// whatever the schema reader recurses to.
//
// Numbers: emitted as integers or fixed 6-decimal floats with trailing
// zeros trimmed (deterministic, locale-free; no newlib float printf
// needed); parsed with sign, fraction and exponent. Strings: `"` `\` and
// controls escaped on output; \uXXXX decoded to UTF-8 on input.
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace bb {

struct JsonOut
{
    char*  buf;
    size_t cap;
    size_t len       = 0;
    bool   ok        = true;
    bool   needComma = false;

    JsonOut(char* b, size_t c) : buf(b), cap(c) { if(cap) buf[0] = 0; else ok = false; }

    void Put(char c)
    {
        if(!ok) return;
        if(len + 1 >= cap) { ok = false; return; }
        buf[len++] = c;
        buf[len]   = 0;
    }
    void Raw(const char* s) { while(*s && ok) Put(*s++); }
    void Comma() { if(needComma) Put(','); needComma = false; }

    void BeginObject() { Comma(); Put('{'); }
    void EndObject()   { Put('}'); needComma = true; }
    void BeginArray()  { Comma(); Put('['); }
    void EndArray()    { Put(']'); needComma = true; }

    void Key(const char* k) { Comma(); Quoted(k); Put(':'); needComma = false; }

    void Int(long v)
    {
        Comma();
        char t[24]; int n = 0;
        unsigned long u = v < 0 ? (unsigned long)(-(v + 1)) + 1u : (unsigned long)v;
        do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while(u);
        if(v < 0) Put('-');
        while(n) Put(t[--n]);
        needComma = true;
    }
    void Bool(bool b) { Comma(); Raw(b ? "true" : "false"); needComma = true; }

    // fixed 6 decimals, trimmed: 0.25 → "0.25", 48000 → "48000", 1e-7 → "0"
    void Float(float v)
    {
        Comma();
        double d = v;
        if(!(d == d) || d > 1e15 || d < -1e15) d = 0.;   // NaN/inf/out of range → 0
        if(d < 0) { Put('-'); d = -d; }
        uint64_t ip = (uint64_t)d;
        uint64_t f6 = (uint64_t)((d - (double)ip) * 1e6 + 0.5);
        if(f6 >= 1000000u) { ip++; f6 = 0; }
        char t[24]; int n = 0;
        do { t[n++] = (char)('0' + ip % 10u); ip /= 10u; } while(ip);
        while(n) Put(t[--n]);
        if(f6)
        {
            char fr[7]; for(int i = 5; i >= 0; i--) { fr[i] = (char)('0' + f6 % 10u); f6 /= 10u; }
            int last = 5; while(last > 0 && fr[last] == '0') last--;
            Put('.'); for(int i = 0; i <= last; i++) Put(fr[i]);
        }
        needComma = true;
    }
    void String(const char* s) { Comma(); Quoted(s); needComma = true; }

    // key/value shorthands
    void KeyInt(const char* k, long v)          { Key(k); Int(v); }
    void KeyFloat(const char* k, float v)       { Key(k); Float(v); }
    void KeyBool(const char* k, bool v)         { Key(k); Bool(v); }
    void KeyString(const char* k, const char* v){ Key(k); String(v); }

  private:
    void Quoted(const char* s)
    {
        Put('"');
        for(; *s && ok; s++)
        {
            const unsigned char c = (unsigned char)*s;
            if(c == '"' || c == '\\') { Put('\\'); Put((char)c); }
            else if(c == '\n') Raw("\\n");
            else if(c == '\r') Raw("\\r");
            else if(c == '\t') Raw("\\t");
            else if(c < 0x20)
            {
                Raw("\\u00");
                Put("0123456789abcdef"[c >> 4]); Put("0123456789abcdef"[c & 15]);
            }
            else Put((char)c);
        }
        Put('"');
    }
};

struct JsonIn
{
    const char* p;
    const char* end;
    bool        ok = true;

    JsonIn(const char* text, size_t n) : p(text), end(text ? text + n : text) { if(!text) ok = false; }
    explicit JsonIn(const char* text) : JsonIn(text, text ? strlen(text) : 0) {}

    // ── structure ──
    bool BeginObject() { return Expect('{'); }
    bool BeginArray()  { return Expect('['); }

    // Next key of the current object into key[cap] (truncated if longer,
    // always NUL-terminated). False at '}' (consumed) or on error — test
    // `ok` after the loop to tell the two apart.
    bool NextKey(char* key, size_t cap)
    {
        if(!ok) return false;
        Ws();
        if(Peek() == '}') { p++; return false; }
        if(Peek() == ',') { p++; Ws(); }
        if(Peek() != '"') return Fail();
        if(!String(key, cap)) return false;
        Ws();
        if(Peek() != ':') return Fail();
        p++;
        return true;
    }
    // True when another array element follows; false at ']' (consumed) or
    // on error.
    bool ArrayMore()
    {
        if(!ok) return false;
        Ws();
        if(Peek() == ']') { p++; return false; }
        if(Peek() == ',') { p++; Ws(); if(Peek() == ']') return Fail(); }
        if(p >= end) return Fail();
        return true;
    }

    // ── scalars ──
    bool Number(double& v)
    {
        if(!ok) return false;
        Ws();
        bool neg = false;
        if(Peek() == '-') { neg = true; p++; }
        if(!Digit(Peek())) return Fail();
        double ip = 0;
        while(Digit(Peek())) { ip = ip * 10. + (Peek() - '0'); p++; }
        double fr = 0, sc = 0.1;
        if(Peek() == '.')
        {
            p++;
            if(!Digit(Peek())) return Fail();
            while(Digit(Peek())) { fr += (Peek() - '0') * sc; sc *= 0.1; p++; }
        }
        double val = ip + fr;
        if(Peek() == 'e' || Peek() == 'E')
        {
            p++;
            bool eneg = false;
            if(Peek() == '+') p++; else if(Peek() == '-') { eneg = true; p++; }
            if(!Digit(Peek())) return Fail();
            int e = 0;
            while(Digit(Peek())) { if(e < 400) e = e * 10 + (Peek() - '0'); p++; }
            double m = 1; while(e--) m *= 10.;
            val = eneg ? val / m : val * m;
        }
        v = neg ? -val : val;
        return true;
    }
    bool Float(float& v) { double d; if(!Number(d)) return false; v = (float)d; return true; }
    bool Int(int& v)
    {
        double d; if(!Number(d)) return false;
        if(d > 2147483647. || d < -2147483648.) return Fail();
        v = (int)d;
        return true;
    }
    bool Bool(bool& v)
    {
        if(!ok) return false;
        Ws();
        if(Match("true"))  { v = true;  return true; }
        if(Match("false")) { v = false; return true; }
        // tolerate 0/1 for flags
        double d; if(!Number(d)) return false;
        v = d != 0.;
        return true;
    }
    // String value into out[cap] (truncated, NUL-terminated; the whole
    // token is consumed regardless).
    bool String(char* out, size_t cap)
    {
        if(!ok) return false;
        Ws();
        if(Peek() != '"') return Fail();
        p++;
        size_t n = 0;
        for(;;)
        {
            if(p >= end) return Fail();
            char c = *p++;
            if(c == '"') break;
            if(c == 0) return Fail();
            if(c == '\\')
            {
                if(p >= end) return Fail();
                const char e = *p++;
                uint32_t cp = 0;
                switch(e)
                {
                    case '"': c = '"'; break;   case '\\': c = '\\'; break;
                    case '/': c = '/'; break;   case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;  case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;  case 'f': c = '\f'; break;
                    case 'u':
                        for(int i = 0; i < 4; i++)
                        {
                            if(p >= end) return Fail();
                            const int h = Hex(*p++);
                            if(h < 0) return Fail();
                            cp = (cp << 4) | (uint32_t)h;
                        }
                        Utf8(out, cap, n, cp);
                        continue;
                    default: return Fail();
                }
            }
            if(n + 1 < cap) out[n++] = c;
        }
        if(cap) out[n < cap ? n : cap - 1] = 0;
        return true;
    }
    bool Null()
    {
        if(!ok) return false;
        Ws();
        return Match("null");
    }

    // Skip any value (scalar, string, object, array — nested to any depth).
    bool Skip()
    {
        if(!ok) return false;
        Ws();
        const char c = Peek();
        if(c == '"') { char d[1]; return String(d, 1); }
        if(c == '[' || c == '{')
        {
            int depth = 0;
            for(;;)
            {
                if(p >= end) return Fail();
                const char x = *p;
                if(x == '"') { char d[1]; if(!String(d, 1)) return false; continue; }
                p++;
                if(x == '[' || x == '{') depth++;
                else if(x == ']' || x == '}') { if(--depth == 0) return true; }
                else if(x == 0) return Fail();
            }
        }
        if(p >= end) return Fail();
        while(p < end && *p && *p != ',' && *p != ']' && *p != '}' && !Space(*p)) p++;
        return true;
    }

    // True when only whitespace remains (a whole document was consumed).
    bool AtEnd() { Ws(); return ok && p >= end; }

  private:
    static bool Digit(char c) { return c >= '0' && c <= '9'; }
    static bool Space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
    static int  Hex(char c)
    {
        if(c >= '0' && c <= '9') return c - '0';
        if(c >= 'a' && c <= 'f') return c - 'a' + 10;
        if(c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    char Peek() const { return p < end ? *p : 0; }
    void Ws() { while(p < end && Space(*p)) p++; }
    bool Fail() { ok = false; return false; }
    bool Expect(char c)
    {
        if(!ok) return false;
        Ws();
        if(Peek() != c) return Fail();
        p++;
        return true;
    }
    bool Match(const char* lit)
    {
        const size_t n = strlen(lit);
        if((size_t)(end - p) < n || memcmp(p, lit, n) != 0) return false;
        p += n;
        return true;
    }
    static void Utf8(char* out, size_t cap, size_t& n, uint32_t cp)
    {
        char t[3]; int k = 0;
        if(cp < 0x80) { t[k++] = (char)cp; }
        else if(cp < 0x800) { t[k++] = (char)(0xC0 | (cp >> 6)); t[k++] = (char)(0x80 | (cp & 0x3F)); }
        else if(cp >= 0xD800 && cp < 0xE000) { t[k++] = '?'; }   // lone surrogate
        else { t[k++] = (char)(0xE0 | (cp >> 12)); t[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[k++] = (char)(0x80 | (cp & 0x3F)); }
        for(int i = 0; i < k; i++) if(n + 1 < cap) out[n++] = t[i];
    }
};

} // namespace bb
