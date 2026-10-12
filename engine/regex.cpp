// regex.cpp — the aardio-pattern engine (compile + match).
#include "engine/common.h"

namespace NodioVM
{
    bool REClass(unsigned cls, wchar_t c)
    {
        bool r = false;
        if (cls & 1u) r = r || iswdigit(c) != 0;
        if (cls & 2u) r = r || !iswdigit(c);
        if (cls & 4u) r = r || (iswalnum(c) || c == L'_');
        if (cls & 8u) r = r || !(iswalnum(c) || c == L'_');
        if (cls & 16u) r = r || iswspace(c) != 0;
        if (cls & 32u) r = r || !iswspace(c);
        return (cls & 2u) || (cls & 8u) || (cls & 32u) ? r : r;
    }

    bool REClassHit(const RE& el, wchar_t c)
    {
        bool hit = REClass(el.cls, c);
        for (auto& rg : el.ranges)
            if (c >= rg.first && c <= rg.second) hit = true;
        if (el.neg) return !hit;
        return hit;
    }

    struct REParser
    {
        const wstring& pat;
        size_t i = 0;
        int nGroups = 0;
        wstring* err;
        explicit REParser(const wstring& p, wstring* e) : pat(p), err(e) {}

        bool ParseSeq(vector<RE>& out, bool inGroup)
        {
            while (i < pat.size())
            {
                wchar_t c = pat[i];
                if (c == L'|' || c == L')')
                    return inGroup ? true : true;   // caller handles both
                RE el;
                if (c == L'(')
                {
                    i++;
                    if (i + 1 < pat.size() && pat[i] == L'?' && pat[i + 1] == L':')
                        i += 2;
                    else
                        el.gi = ++nGroups;
                    el.k = 3;
                    if (!ParseSeq(el.sub, true))
                        return false;
                    if (i >= pat.size() || pat[i] != L')')
                    {
                        (*err) = L"模式错误: 缺少 )";
                        return false;
                    }
                    i++;
                }
                else if (c == L'[')
                {
                    i++;
                    el.k = 1;
                    if (i < pat.size() && pat[i] == L'^') { el.neg = true; i++; }
                    bool first = true;
                    while (i < pat.size() && (pat[i] != L']' || first))
                    {
                        first = false;
                        wchar_t lo = pat[i];
                        if (lo == L'\\' && i + 1 < pat.size())
                        {
                            i++;
                            wchar_t e = pat[i];
                            unsigned bit = ClassBit(e);
                            if (bit) el.cls |= bit;
                            else if (e == L'n') lo = L'\n';
                            else if (e == L't') lo = L'\t';
                            else if (e == L'r') lo = L'\r';
                            else lo = e;
                        }
                        if (i + 2 < pat.size() && pat[i + 1] == L'-' &&
                            pat[i + 2] != L']')
                        {
                            wchar_t hi = pat[i + 2];
                            if (hi == L'\\' && i + 3 < pat.size())
                                hi = pat[i + 3];
                            el.ranges.push_back({ lo, hi });
                            i += 3;
                        }
                        else
                        {
                            el.ranges.push_back({ lo, lo });
                            i++;
                        }
                    }
                    if (i >= pat.size())
                    {
                        (*err) = L"模式错误: 缺少 ]";
                        return false;
                    }
                    i++;   // ']'
                }
                else if (c == L'.')
                {
                    i++;
                    el.k = 2;
                }
                else if (c == L'$')
                {
                    i++;
                    el.k = 4;
                }
                else if (c == L'^' && out.empty())
                {
                    i++;   // ^ anchor: only meaningful at pattern start; the
                           // search loop honors it by not scanning
                    continue;
                }
                else if (c == L'\\')
                {
                    i++;
                    if (i >= pat.size())
                    {
                        (*err) = L"模式错误: 悬挂的 \\";
                        return false;
                    }
                    wchar_t e = pat[i++];
                    unsigned bit = ClassBit(e);
                    if (bit) { el.k = 1; el.cls |= bit; }
                    else
                    {
                        el.k = 0;
                        if (e == L'n') el.ch = L'\n';
                        else if (e == L't') el.ch = L'\t';
                        else if (e == L'r') el.ch = L'\r';
                        else el.ch = e;
                    }
                }
                else
                {
                    i++;
                    el.k = 0;
                    el.ch = c;
                }
                // quantifier
                if (i < pat.size())
                {
                    wchar_t q = pat[i];
                    if (q == L'*') { el.min = 0; el.max = -1; i++; }
                    else if (q == L'+') { el.min = 1; el.max = -1; i++; }
                    else if (q == L'?') { el.min = 0; el.max = 1; i++; }
                    else if (q == L'{')
                    {
                        size_t j = pat.find(L'}', i);
                        if (j != wstring::npos)
                        {
                            wstring spec = pat.substr(i + 1, j - i - 1);
                            size_t comma = spec.find(L',');
                            el.min = _wtoi(spec.substr(0, comma).c_str());
                            if (comma == wstring::npos)
                                el.max = el.min;
                            else
                            {
                                wstring hi = spec.substr(comma + 1);
                                el.max = hi.empty() ? -1 : _wtoi(hi.c_str());
                            }
                            if (el.min < 0) el.min = 0;
                            i = j + 1;
                        }
                    }
                    if (i < pat.size() && pat[i] == L'?')
                    {
                        el.lazy = true;
                        i++;
                    }
                }
                out.push_back(std::move(el));
            }
            return true;
        }

        unsigned ClassBit(wchar_t e)
        {
            switch (e)
            {
            case L'd': return 1;
            case L'D': return 2;
            case L'w': return 4;
            case L'W': return 8;
            case L's': return 16;
            case L'S': return 32;
            default: return 0;
            }
        }
    };

    size_t REMatchOne(const RE& el, const wstring& s, size_t pos,
                             vector<wstring>& caps);

    long long REMatchSeq(const vector<RE>& els, size_t idx,
                                const wstring& s, size_t pos,
                                vector<wstring>& caps, bool anchored)
    {
        if (idx == els.size())
            return (long long)pos;
        const RE& el = els[idx];
        if (el.k == 4)   // '$' — zero-width end anchor
        {
            if (pos != s.size()) return -1;
            return REMatchSeq(els, idx + 1, s, pos, caps, anchored);
        }
        return REMatchRep(els, idx, s, pos, 0, caps, anchored);
    }

    size_t REMatchOne(const RE& el, const wstring& s, size_t pos,
                             vector<wstring>& caps)
    {
        if (pos >= s.size())
            return RENPOS;
        switch (el.k)
        {
        case 0: return s[pos] == el.ch ? pos + 1 : RENPOS;
        case 2: return s[pos] != L'\n' ? pos + 1 : RENPOS;
        case 1: return REClassHit(el, s[pos]) ? pos + 1 : RENPOS;
        case 3:
        {
            vector<wstring> tmp = caps;
            long long e = REMatchSeq(el.sub, 0, s, pos, tmp, false);
            if (e < 0)
                return RENPOS;
            if (el.gi >= 0)
            {
                if ((size_t)el.gi >= caps.size())
                    caps.resize((size_t)el.gi + 1);
                caps[(size_t)el.gi] = s.substr(pos, (size_t)e - pos);
            }
            return (size_t)e;
        }
        default: return RENPOS;
        }
    }

    long long REMatchRep(const vector<RE>& els, size_t idx,
                                const wstring& s, size_t pos, int count,
                                vector<wstring>& caps, bool anchored)
    {
        const RE& el = els[idx];
        bool canMore = el.max < 0 || count < el.max;
        if (el.lazy)
        {
            if (count >= el.min)
            {
                long long e = REMatchSeq(els, idx + 1, s, pos, caps, anchored);
                if (e >= 0) return e;
            }
            if (canMore)
            {
                size_t np = REMatchOne(el, s, pos, caps);
                if (np != RENPOS && np != pos)
                {
                    long long e = REMatchRep(els, idx, s, np, count + 1,
                                             caps, anchored);
                    if (e >= 0) return e;
                }
            }
            return -1;
        }
        // greedy: consume more first, then stop here
        if (canMore)
        {
            size_t np = REMatchOne(el, s, pos, caps);
            if (np != RENPOS && np != pos)
            {
                long long e = REMatchRep(els, idx, s, np, count + 1,
                                         caps, anchored);
                if (e >= 0) return e;
            }
        }
        if (count >= el.min)
        {
            long long e = REMatchSeq(els, idx + 1, s, pos, caps, anchored);
            if (e >= 0) return e;
        }
        return -1;
    }

    // one anchored attempt; fills caps and match end

    bool RETry(const vector<vector<RE>>& branches, const wstring& s,
                      size_t pos, bool anchored, vector<wstring>& caps,
                      size_t* pEnd)
    {
        for (auto& els : branches)
        {
            vector<wstring> tmp;
            long long e = REMatchSeq(els, 0, s, pos, tmp, anchored);
            if (e >= 0)
            {
                caps = tmp;
                *pEnd = (size_t)e;
                return true;
            }
        }
        return false;
    }

    // ---------------- pattern compiler ----------------

    bool RECompile(const wstring& pat, vector<vector<RE>>& branches,
                          bool* pAnchored, int* pGroups, wstring* err)
    {
        REParser ps(pat, err);
        bool anchored = false;
        size_t j = 0;
        if (!pat.empty() && pat[0] == L'^') { anchored = true; j = 1; }
        ps.i = j;
        while (true)
        {
            vector<RE> els;
            if (!ps.ParseSeq(els, false))
                return false;
            branches.push_back(std::move(els));
            if (ps.i < ps.pat.size() && ps.pat[ps.i] == L'|')
            {
                ps.i++;
                continue;
            }
            break;
        }
        if (!err->empty())
            return false;
        *pAnchored = anchored;
        *pGroups = ps.nGroups;
        return true;
    }
}
