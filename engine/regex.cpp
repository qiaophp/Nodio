// regex.cpp — the pattern engine (compile + match).
#include "engine/common.h"

namespace NodioVM
{
    bool REClass(unsigned cls, wchar_t c)
    {
        bool r = false;
        if (cls & 1u) r = r || (c >= L'0' && c <= L'9');
        if (cls & 2u) r = r || !(c >= L'0' && c <= L'9');
        if (cls & 4u) r = r || ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_');
        if (cls & 8u) r = r || !((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_');
        if (cls & 16u) r = r || (iswspace(c) != 0);
        if (cls & 32u) r = r || !iswspace(c);
        if (cls & 64u) r = r || ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'));
        if (cls & 128u) r = r || !((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'));
        if (cls & 256u) r = r || (c >= L'a' && c <= L'z');
        if (cls & 512u) r = r || (c >= L'A' && c <= L'Z');
        if (cls & 1024u) r = r || (iswxdigit(c) != 0);
        if (cls & 2048u) r = r || (iswpunct(c) != 0);
        if (cls & 4096u) r = r || ((unsigned)c < 0x80);
        if (cls & 8192u) r = r || ((unsigned)c >= 0x80);
        return r;
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
            case L'a': return 64;
            case L'A': return 128;
            case L'l': return 256;
            case L'u': return 512;
            case L'x': return 1024;
            case L'p': return 2048;
            case L'i': return 4096;
            case L'I': return 8192;
            default: return 0;
            }
        }

        void ParseQuantifier(RE& el)
        {
            if (i >= pat.size()) return;
            wchar_t q = pat[i];
            bool hasQuant = false;
            int qMin = 1, qMax = 1;
            bool qLazy = false;
            if (q == L'*') { qMin = 0; qMax = -1; i++; hasQuant = true; }
            else if (q == L'+') { qMin = 1; qMax = -1; i++; hasQuant = true; }
            else if (q == L'?')
            {
                if (i + 1 < pat.size() && pat[i + 1] == L'!')
                {
                    i += 2;
                    RE lookahead;
                    lookahead.k = 11; // zero-width negative lookahead
                    lookahead.sub.push_back(std::move(el));
                    el = std::move(lookahead);
                    return;
                }
                else if (i + 1 < pat.size() && pat[i + 1] == L'=')
                {
                    i += 2;
                    RE lookahead;
                    lookahead.k = 10; // zero-width positive lookahead
                    lookahead.sub.push_back(std::move(el));
                    el = std::move(lookahead);
                    return;
                }
                qMin = 0; qMax = 1; i++; hasQuant = true;
            }
            else if (q == L'{')
            {
                size_t j = pat.find(L'}', i);
                if (j != wstring::npos)
                {
                    wstring spec = pat.substr(i + 1, j - i - 1);
                    size_t comma = spec.find(L',');
                    qMin = _wtoi(spec.substr(0, comma).c_str());
                    if (comma == wstring::npos)
                        qMax = qMin;
                    else
                    {
                        wstring hi = spec.substr(comma + 1);
                        qMax = hi.empty() ? -1 : _wtoi(hi.c_str());
                    }
                    if (qMin < 0) qMin = 0;
                    i = j + 1;
                    hasQuant = true;
                }
            }
            if (hasQuant)
            {
                if (i < pat.size() && pat[i] == L'?')
                {
                    qLazy = true;
                    i++;
                }
                el.min = qMin;
                el.max = qMax;
                el.lazy = qLazy;
            }
        }

        bool ParseOne(RE& el)
        {
            if (i >= pat.size()) return false;
            wchar_t c = pat[i];
            if (c == L'(')
            {
                i++;
                int gi = -1;
                if (i + 1 < pat.size() && pat[i] == L'?' && pat[i + 1] == L':')
                    i += 2;
                else
                    gi = ++nGroups;
                vector<RE> sub;
                if (!ParseSeq(sub, L')'))
                    return false;
                if (i < pat.size() && pat[i] == L')')
                    i++;
                el.k = 3;
                el.gi = gi;
                el.sub = std::move(sub);
                ParseQuantifier(el);
                return true;
            }
            else if (c == L'<')
            {
                // Non-capturing group
                if (pat.size() >= i + 3 && pat.substr(i, 3) == L"<@@")
                {
                    size_t endAt = pat.find(L"@>", i + 3);
                    if (endAt != wstring::npos)
                    {
                        wstring lit = pat.substr(i + 3, endAt - (i + 3));
                        i = endAt + 2;
                        el.k = 3;
                        el.gi = -1;
                        for (wchar_t chLit : lit)
                        {
                            RE chEl;
                            chEl.k = 0;
                            chEl.ch = chLit;
                            chEl.nocase = true;
                            el.sub.push_back(chEl);
                        }
                        ParseQuantifier(el);
                        return true;
                    }
                }
                else if (pat.size() >= i + 2 && pat.substr(i, 2) == L"<@")
                {
                    size_t endAt = pat.find(L"@>", i + 2);
                    if (endAt != wstring::npos)
                    {
                        wstring lit = pat.substr(i + 2, endAt - (i + 2));
                        i = endAt + 2;
                        el.k = 3;
                        el.gi = -1;
                        for (wchar_t chLit : lit)
                        {
                            RE chEl;
                            chEl.k = 0;
                            chEl.ch = chLit;
                            chEl.nocase = false;
                            el.sub.push_back(chEl);
                        }
                        ParseQuantifier(el);
                        return true;
                    }
                }
                i++; // skip '<'
                vector<RE> sub;
                if (!ParseSeq(sub, L'>'))
                    return false;
                if (i < pat.size() && pat[i] == L'>')
                    i++;
                el.k = 3;
                el.gi = -1;
                el.sub = std::move(sub);
                ParseQuantifier(el);
                return true;
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
                    if (i + 2 < pat.size() && pat[i + 1] == L'-' && pat[i + 2] != L']')
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
                if (i < pat.size() && pat[i] == L']')
                    i++;
                ParseQuantifier(el);
                return true;
            }
            else if (c == L'.')
            {
                i++;
                el.k = 2;
                ParseQuantifier(el);
                return true;
            }
            else if (c == L'$')
            {
                i++;
                el.k = 4;
                return true;
            }
            else if (c == L'!')
            {
                i++;
                el.k = 9;
                if (i >= pat.size() || pat[i] == L')' || pat[i] == L'>' || pat[i] == L'|')
                {
                    el.ch = L'$';
                    return true;
                }
                if (i + 1 < pat.size() && pat[i] == L'\\' && pat[i + 1] == L'z')
                {
                    i += 2;
                    el.ch = L'$';
                    return true;
                }
                RE subEl;
                if (ParseOne(subEl))
                {
                    el.sub.push_back(std::move(subEl));
                    return true;
                }
                el.ch = L'$';
                return true;
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
                ParseQuantifier(el);
                return true;
            }
            else if (c == L'%' && i + 1 < pat.size())
            {
                // Balanced delimiter pattern %<begin><end>
                // e.g. %(), %{}, %"", %'', %\[\], %\<\>
                size_t pCur = i + 1;
                wchar_t openC = 0, closeC = 0;
                if (pCur < pat.size() && pat[pCur] == L'\\')
                    pCur++;
                if (pCur < pat.size())
                {
                    openC = pat[pCur++];
                    if (pCur < pat.size() && pat[pCur] == L'\\')
                        pCur++;
                    if (pCur < pat.size())
                    {
                        closeC = pat[pCur++];
                        el.k = 7;
                        el.ch = openC;
                        el.chClose = closeC;
                        i = pCur;
                        if (i < pat.size() && pat[i] == L'?')
                        {
                            el.lazy = true;
                            i++;
                        }
                        else
                        {
                            ParseQuantifier(el);
                        }
                        return true;
                    }
                }
                // Fallback to literal '%'
                i++;
                el.k = 0;
                el.ch = c;
                ParseQuantifier(el);
                return true;
            }
            else
            {
                i++;
                el.k = 0;
                el.ch = c;
                ParseQuantifier(el);
                return true;
            }
        }

        bool ParseSeq(vector<RE>& out, wchar_t stopChar)
        {
            while (i < pat.size())
            {
                wchar_t c = pat[i];
                if (stopChar != 0 && c == stopChar)
                    break;
                if (c == L')' && stopChar == L')')
                    break;
                if (c == L'>' && stopChar == L'>')
                    break;
                if (c == L'|')
                {
                    i++; // skip '|'
                    if (out.empty())
                        continue;
                    RE left = std::move(out.back());
                    out.pop_back();

                    vector<RE> b2;
                    if (i < pat.size() && pat[i] == L'<')
                    {
                        RE right;
                        if (!ParseOne(right))
                            return false;
                        b2.push_back(std::move(right));
                    }
                    else
                    {
                        while (i < pat.size())
                        {
                            wchar_t rc = pat[i];
                            if (stopChar != 0 && rc == stopChar)
                                break;
                            if (rc == L')' && stopChar == L')')
                                break;
                            if (rc == L'>' && stopChar == L'>')
                                break;
                            if (rc == L'|')
                                break;
                            RE elRight;
                            if (!ParseOne(elRight))
                                return false;
                            b2.push_back(std::move(elRight));
                        }
                    }

                    RE altEl;
                    altEl.k = 8;
                    altEl.gi = -1;
                    if (left.k == 8 && left.min == 1 && left.max == 1 && left.gi < 0)
                    {
                        altEl.alt = std::move(left.alt);
                    }
                    else
                    {
                        vector<RE> b1;
                        b1.push_back(std::move(left));
                        altEl.alt.push_back(std::move(b1));
                    }
                    altEl.alt.push_back(std::move(b2));

                    out.push_back(std::move(altEl));
                    continue;
                }
                RE el;
                if (!ParseOne(el))
                    return false;
                if ((el.k == 3 || el.k == 8) && el.min == 1 && el.max == 1 && el.gi >= 0)
                {
                    int gi = el.gi;
                    el.gi = -1;
                    RE startEl; startEl.k = 5; startEl.gi = gi;
                    out.push_back(std::move(startEl));
                    if (el.k == 3)
                    {
                        for (auto& s : el.sub)
                            out.push_back(std::move(s));
                    }
                    else
                    {
                        out.push_back(std::move(el));
                    }
                    RE endEl; endEl.k = 6; endEl.gi = gi;
                    out.push_back(std::move(endEl));
                }
                else
                {
                    out.push_back(std::move(el));
                }
            }
            return true;
        }
    };

    size_t REMatchOne(const RE& el, const wstring& s, size_t pos,
                             vector<wstring>& caps);

    long long REMatchSeq(const vector<RE>& els, size_t idx,
                         const wstring& s, size_t pos,
                         vector<wstring>& caps, vector<size_t>& groupStarts,
                         bool anchored);

    long long REMatchRep(const vector<RE>& els, size_t idx,
                         const wstring& s, size_t pos, int count,
                         vector<wstring>& caps, vector<size_t>& groupStarts,
                         bool anchored);

    size_t REMatchOne(const RE& el, const wstring& s, size_t pos,
                             vector<wstring>& caps)
    {
        if (pos >= s.size())
        {
            if (el.k == 8)
            {
                for (const auto& branch : el.alt)
                {
                    vector<wstring> tmp = caps;
                    vector<size_t> starts;
                    long long e = REMatchSeq(branch, 0, s, pos, tmp, starts, false);
                    if (e >= 0)
                    {
                        if (el.gi >= 0)
                        {
                            if ((size_t)el.gi >= tmp.size())
                                tmp.resize((size_t)el.gi + 1);
                            tmp[(size_t)el.gi] = L"";
                        }
                        caps = tmp;
                        return (size_t)e;
                    }
                }
            }
            if (el.k == 3)
            {
                vector<wstring> tmp = caps;
                vector<size_t> starts;
                long long e = REMatchSeq(el.sub, 0, s, pos, tmp, starts, false);
                if (e >= 0)
                {
                    if (el.gi >= 0)
                    {
                        if ((size_t)el.gi >= tmp.size())
                            tmp.resize((size_t)el.gi + 1);
                        tmp[(size_t)el.gi] = L"";
                    }
                    caps = tmp;
                    return (size_t)e;
                }
            }
            return RENPOS;
        }
        switch (el.k)
        {
        case 0:
            if (el.nocase)
                return (towlower(s[pos]) == towlower(el.ch)) ? pos + 1 : RENPOS;
            return s[pos] == el.ch ? pos + 1 : RENPOS;
        case 2: return s[pos] != L'\n' ? pos + 1 : RENPOS;
        case 1: return REClassHit(el, s[pos]) ? pos + 1 : RENPOS;
        case 3:
        {
            vector<wstring> tmp = caps;
            vector<size_t> starts;
            long long e = REMatchSeq(el.sub, 0, s, pos, tmp, starts, false);
            if (e < 0)
                return RENPOS;
            if (el.gi >= 0)
            {
                if ((size_t)el.gi >= tmp.size())
                    tmp.resize((size_t)el.gi + 1);
                tmp[(size_t)el.gi] = s.substr(pos, (size_t)e - pos);
            }
            caps = tmp;
            return (size_t)e;
        }
        case 8: // Alternation
        {
            for (const auto& branch : el.alt)
            {
                vector<wstring> tmp = caps;
                vector<size_t> starts;
                long long e = REMatchSeq(branch, 0, s, pos, tmp, starts, false);
                if (e >= 0)
                {
                    if (el.gi >= 0)
                    {
                        if ((size_t)el.gi >= tmp.size())
                            tmp.resize((size_t)el.gi + 1);
                        tmp[(size_t)el.gi] = s.substr(pos, (size_t)e - pos);
                    }
                    caps = tmp;
                    return (size_t)e;
                }
            }
            return RENPOS;
        }
        case 7:
        {
            if (s[pos] != el.ch)
                return RENPOS;
            wchar_t openC = el.ch;
            wchar_t closeC = el.chClose;
            if (openC == closeC)
            {
                size_t p = pos + 1;
                while (p < s.size() && s[p] != closeC)
                    p++;
                if (p < s.size() && s[p] == closeC)
                    return p + 1;
                return RENPOS;
            }
            int depth = 1;
            size_t p = pos + 1;
            while (p < s.size() && depth > 0)
            {
                if (s[p] == openC) depth++;
                else if (s[p] == closeC) depth--;
                p++;
            }
            if (depth == 0)
                return p;
            return RENPOS;
        }
        case 9: return pos;
        default: return RENPOS;
        }
    }

    long long REMatchSeq(const vector<RE>& els, size_t idx,
                         const wstring& s, size_t pos,
                         vector<wstring>& caps, vector<size_t>& groupStarts,
                         bool anchored)
    {
        if (idx == els.size())
            return (long long)pos;
        const RE& el = els[idx];
        if (el.k == 4)   // '$' — zero-width end anchor
        {
            if (pos != s.size()) return -1;
            return REMatchSeq(els, idx + 1, s, pos, caps, groupStarts, anchored);
        }
        if (el.k == 9)   // '!' — zero-width boundary assertion
        {
            if (el.sub.empty() || el.ch == L'$')
            {
                if (pos != s.size()) return -1;
            }
            else
            {
                const RE& target = el.sub[0];
                bool rightMatch = false;
                if (pos < s.size())
                {
                    if (target.k == 1)
                        rightMatch = REClassHit(target, s[pos]);
                    else if (target.k == 0)
                        rightMatch = (s[pos] == target.ch);
                }
                else
                {
                    if (target.k == 1 && (target.cls & (2u | 8u | 32u | 128u)))
                        rightMatch = true; // \D, \W, \S, \A hit at end-of-string
                }

                bool leftMatch = false;
                if (pos > 0)
                {
                    if (target.k == 1)
                        leftMatch = REClassHit(target, s[pos - 1]);
                    else if (target.k == 0)
                        leftMatch = (s[pos - 1] == target.ch);
                }
                else
                {
                    if (target.k == 1 && (target.cls & (2u | 8u | 32u | 128u)))
                        leftMatch = true; // start of string matches \D, \W, \S
                }

                if (!rightMatch || leftMatch)
                    return -1;
            }
            return REMatchSeq(els, idx + 1, s, pos, caps, groupStarts, anchored);
        }
        if (el.k == 10)   // '?=' — zero-width positive lookahead
        {
            if (!el.sub.empty())
            {
                vector<wstring> tryCaps = caps;
                vector<size_t> tryStarts = groupStarts;
                long long e = REMatchSeq(el.sub, 0, s, pos, tryCaps, tryStarts, false);
                if (e < 0) return -1;
            }
            return REMatchSeq(els, idx + 1, s, pos, caps, groupStarts, anchored);
        }
        if (el.k == 11)   // '?!' — zero-width negative lookahead
        {
            if (!el.sub.empty())
            {
                vector<wstring> tryCaps = caps;
                vector<size_t> tryStarts = groupStarts;
                long long e = REMatchSeq(el.sub, 0, s, pos, tryCaps, tryStarts, false);
                if (e >= 0) return -1;
            }
            return REMatchSeq(els, idx + 1, s, pos, caps, groupStarts, anchored);
        }
        if (el.k == 5)   // GroupStart
        {
            vector<size_t> nextStarts = groupStarts;
            if (el.gi >= 0)
            {
                if ((size_t)el.gi >= nextStarts.size())
                    nextStarts.resize((size_t)el.gi + 1, 0);
                nextStarts[(size_t)el.gi] = pos;
            }
            return REMatchSeq(els, idx + 1, s, pos, caps, nextStarts, anchored);
        }
        if (el.k == 6)   // GroupEnd
        {
            vector<wstring> nextCaps = caps;
            if (el.gi >= 0 && (size_t)el.gi < groupStarts.size())
            {
                size_t sp = groupStarts[(size_t)el.gi];
                if ((size_t)el.gi >= nextCaps.size())
                    nextCaps.resize((size_t)el.gi + 1);
                nextCaps[(size_t)el.gi] = s.substr(sp, pos - sp);
            }
            long long e = REMatchSeq(els, idx + 1, s, pos, nextCaps, groupStarts, anchored);
            if (e >= 0)
            {
                caps = nextCaps;
                return e;
            }
            return -1;
        }
        if (el.k == 8)   // Alternation
        {
            for (const auto& branch : el.alt)
            {
                vector<wstring> tryCaps = caps;
                vector<size_t> tryStarts = groupStarts;
                long long e = REMatchSeq(branch, 0, s, pos, tryCaps, tryStarts, false);
                if (e >= 0)
                {
                    if (el.gi >= 0)
                    {
                        if ((size_t)el.gi >= tryCaps.size())
                            tryCaps.resize((size_t)el.gi + 1);
                        tryCaps[(size_t)el.gi] = s.substr(pos, (size_t)e - pos);
                    }
                    long long nextE = REMatchSeq(els, idx + 1, s, (size_t)e, tryCaps, tryStarts, anchored);
                    if (nextE >= 0)
                    {
                        caps = tryCaps;
                        groupStarts = tryStarts;
                        return nextE;
                    }
                }
            }
            return -1;
        }
        return REMatchRep(els, idx, s, pos, 0, caps, groupStarts, anchored);
    }

    long long REMatchRep(const vector<RE>& els, size_t idx,
                         const wstring& s, size_t pos, int count,
                         vector<wstring>& caps, vector<size_t>& groupStarts,
                         bool anchored)
    {
        const RE& el = els[idx];
        bool canMore = el.max < 0 || count < el.max;
        if (el.lazy)
        {
            if (count >= el.min)
            {
                vector<wstring> tryCaps = caps;
                long long e = REMatchSeq(els, idx + 1, s, pos, tryCaps, groupStarts, anchored);
                if (e >= 0) { caps = tryCaps; return e; }
            }
            if (canMore)
            {
                vector<wstring> oneCaps = caps;
                size_t np = REMatchOne(el, s, pos, oneCaps);
                if (np != RENPOS && np != pos)
                {
                    long long e = REMatchRep(els, idx, s, np, count + 1,
                                             oneCaps, groupStarts, anchored);
                    if (e >= 0) { caps = oneCaps; return e; }
                }
            }
            return -1;
        }
        // greedy: consume more first, then stop here
        if (canMore)
        {
            vector<wstring> oneCaps = caps;
            size_t np = REMatchOne(el, s, pos, oneCaps);
            if (np != RENPOS && np != pos)
            {
                long long e = REMatchRep(els, idx, s, np, count + 1,
                                         oneCaps, groupStarts, anchored);
                if (e >= 0) { caps = oneCaps; return e; }
            }
        }
        if (count >= el.min)
        {
            vector<wstring> tryCaps = caps;
            long long e = REMatchSeq(els, idx + 1, s, pos, tryCaps, groupStarts, anchored);
            if (e >= 0) { caps = tryCaps; return e; }
        }
        return -1;
    }

    // one anchored attempt; fills caps and match end

    bool RETry(const vector<vector<RE>>& branches, const wstring& s,
                      size_t pos, bool anchored, vector<wstring>& caps,
                      size_t* pEnd)
    {
        // buffer reuse across scan positions: the per-position attempt
        // loop (string.find over a long haystack) allocated two vectors
        // per position — measured ~24ns/position on a 33k-char scan.
        // thread_local is safe: RETry is never re-entered from inside
        // the matcher (REMatchSeq only recurses into itself).
        static thread_local vector<wstring> tmp;
        static thread_local vector<size_t> groupStarts;
        for (auto& els : branches)
        {
            tmp.clear();
            groupStarts.clear();
            long long e = REMatchSeq(els, 0, s, pos, tmp, groupStarts, anchored);
            if (e >= 0)
            {
                caps = tmp;
                *pEnd = (size_t)e;
                return true;
            }
        }
        return false;
    }

    // literal fast path gate: patterns with none of the metacharacters
    // below parse to a single literal-char sequence, so string.find /
    // match / count can use wstring::find (memchr speed) instead of the
    // per-position matcher. Keep in sync with REParser::ParseOne's
    // special cases (^ $ . [ ] ( ) < > | \ * + ? { } ! %).
    bool REIsPlainLiteral(const wstring& pat)
    {
        if (pat.empty()) return false;
        for (wchar_t c : pat)
        {
            switch (c)
            {
            case L'^': case L'$': case L'.': case L'[': case L']':
            case L'(': case L')': case L'<': case L'>': case L'|':
            case L'\\': case L'*': case L'+': case L'?': case L'{':
            case L'}': case L'!': case L'%':
                return false;
            }
        }
        return true;
    }

    // ---------------- pattern compiler ----------------

    struct CompiledRE
    {
        vector<vector<RE>> branches;
        bool anchored = false;
        int nGroups = 0;
        bool ok = false;
        wstring err;
    };

    bool RECompile(const wstring& pat, vector<vector<RE>>& branches,
                          bool* pAnchored, int* pGroups, wstring* err)
    {
        thread_local std::unordered_map<wstring, CompiledRE> s_reCache;
        auto it = s_reCache.find(pat);
        if (it != s_reCache.end())
        {
            if (!it->second.ok)
            {
                if (err) *err = it->second.err;
                return false;
            }
            branches = it->second.branches;
            if (pAnchored) *pAnchored = it->second.anchored;
            if (pGroups) *pGroups = it->second.nGroups;
            return true;
        }

        REParser ps(pat, err);
        bool anchored = false;
        size_t j = 0;
        if (!pat.empty() && pat[0] == L'^') { anchored = true; j = 1; }
        ps.i = j;
        vector<RE> els;
        if (!ps.ParseSeq(els, 0))
        {
            if (s_reCache.size() >= 128) s_reCache.clear();
            s_reCache[pat] = { {}, false, 0, false, err ? *err : L"" };
            return false;
        }
        branches.push_back(std::move(els));
        if (err && !err->empty())
        {
            if (s_reCache.size() >= 128) s_reCache.clear();
            s_reCache[pat] = { {}, false, 0, false, *err };
            return false;
        }
        if (pAnchored) *pAnchored = anchored;
        if (pGroups) *pGroups = ps.nGroups;

        if (s_reCache.size() >= 128) s_reCache.clear();
        s_reCache[pat] = { branches, anchored, ps.nGroups, true, L"" };
        return true;
    }
}
