// parser.h — the recursive-descent parser. AST node types live in
// common.h; this header is self-contained and inline-only.
#pragma once
#include "engine/common.h"

namespace NodioVM
{
    struct Parser
    {
        vector<Tok> toks;
        size_t p = 0;
        wstring* err;
        // compile-time file embedding ($"path"): resolution bases —
        // strEmbedBase = directory of the file being parsed,
        // strEmbedRoot = aardio root (parent of NODIO_LIBDIR) for "~/"
        wstring strEmbedBase;
        wstring strEmbedRoot;

        const Tok& Peek(int off = 0) const { return toks[min(p + off, toks.size() - 1)]; }
        Tok Next()
        {
            // NB: never write min(p++, ...) here — the Windows min macro
            // double-evaluates the argument and would skip a token.
            if (p >= toks.size())
                p = toks.size() - 1;
            return toks[p++];
        }
        bool IsPunct(const wchar_t* op) const
        {
            return Peek().t == TPunct && Peek().text == op;
        }
        bool IsKw(const wchar_t* kw) const
        {
            return Peek().t == TKw && Peek().text == kw;
        }
        bool EatPunct(const wchar_t* op)
        {
            if (IsPunct(op)) { p++; return true; }
            return false;
        }
        bool EatKw(const wchar_t* kw)
        {
            if (IsKw(kw)) { p++; return true; }
            return false;
        }
        void SkipNL()
        {
            while (Peek().t == TNL) p++;
        }
        void ExpectHelper(const wchar_t* op, int srcLine)
        {
            if (!EatPunct(op))
            {
                wstring context;
                int start = max(0, (int)p - 6);
                int end = min((int)toks.size(), (int)p + 4);
                for (int k = start; k < end; k++)
                {
                    if (k == (int)p) context += L" >>>";
                    context += L" [" + toks[k].text + L"]";
                    if (k == (int)p) context += L"<<< ";
                }
                (*err) = L"语法错误: 期望 " + wstring(op) + L" (from NodioVM.cpp:" + std::to_wstring(srcLine) + L", context:" + context + L") @ 行 " +
                         std::to_wstring(Peek().line);
            }
        }
#define Expect(op) ExpectHelper(op, __LINE__)

        PNode ParseBlock()   // until '}' or end
        {
            PNode blk = New(Node::Block);
            SkipNL();
            while (!IsPunct(L"}") && Peek().t != TEnd)
            {
                PNode st = ParseStatement();
                if (st) blk->list.push_back(st);
                SkipNL();
                if (!err->empty())
                    break;
            }
            return blk;
        }

        PNode ParseStatement()
        {
            { static bool bT = GetEnvironmentVariableW(L"ATP", nullptr, 0) != 0; if (bT) fwprintf(stderr, L"%d ", Peek().line); }
            SkipNL();
            if (IsPunct(L";"))       // empty statement
            {
                p++;
                return nullptr;
            }
            if (IsPunct(L")"))
            {
                // stray close-paren or if-( closer appearing inside braced body
                p++;
                if (nIfParenOpen > 0)
                    nIfParenOpen--;
                return nullptr;
            }
                        if (IsPunct(L"{"))
            {
                // statement-leading '{' = bare block (scopes locals;
                // win/ui ctor bodies nest them)
                p++;
                PNode blk = ParseBlock();
                Expect(L"}");
                return blk;
            }
                        { static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0; if (bDbg) fwprintf(stderr, L"%d ", Peek().line); }
            const Tok& t = Peek();
            if (t.t == TKw)
            {
                if (t.text == L"if") return ParseIf();
                if (t.text == L"try") return ParseTry();
                if (t.text == L"while") return ParseWhile();
                if (t.text == L"for") return ParseFor();
                if (t.text == L"select") return ParseSelect();
                if (t.text == L"with") return ParseWith();
                if (t.text == L"do") return ParseDoWhile();
                if (t.text == L"return")
                {
                    p++;
                    PNode n = New(Node::Return);
                    if (!PeekIsStmtEnd())
                    {
                        n->a = ParseExpr();
                        // aardio multi-return: return a, b, c — empty
                        // slots ("return a,,b" / ",,,addr") are null
                        while (true)
                        {
                            size_t saveR = p;
                            SkipNL();
                            if (EatPunct(L","))
                            {
                                SkipNL();
                                if (PeekIsStmtEnd()) break;
                                if (IsPunct(L","))
                                    n->list.push_back(New(Node::NullK));
                                else
                                    n->list.push_back(ParseExpr());
                            }
                            else
                            {
                                p = saveR;
                                break;
                            }
                        }
                    }
                    return n;
                }
                if (t.text == L"break") { p++; return New(Node::Break); }
                if (t.text == L"continue") { p++; return New(Node::Continue); }
                if (t.text == L"var")
                {
                    p++;
                    PNode n = New(Node::VarDecl);
                    // var a = 6, b = 3, c = 9 — collect name/value pairs.
                    // names[0] pairs with n->a; names[k] (k>0) pairs with
                    // n->b->list[k-1].
                    auto ReadName = [&]() {
                        if (Peek().t == TIdent) { n->names.push_back(Next().text); return true; }
                        n->names.push_back(L"");
                        return false;
                    };
                    ReadName();
                    // trailing names with no initializer: var a, b, c
                    while (true)
                    {
                        size_t saveComma = p;
                        SkipNL();
                        if (EatPunct(L",") && Peek().t == TIdent &&
                            Peek(1).t == TPunct &&
                            (Peek(1).text == L"," || Peek(1).text == L";" ||
                             Peek(1).text == L"=" || Peek(1).t == TNL ||
                             Peek(1).t == TEnd))
                        {
                            SkipNL();
                            ReadName();
                        }
                        else
                        {
                            p = saveComma;
                            break;
                        }
                    }
                    SkipNL();
                    if (EatPunct(L"="))
                    {
                        SkipNL();
                        n->a = ParseExpr();
                    }
                    // additional ", name = expr" pairs (and bare expr items
                    // for the multi-return unpack form var a,b = f())
                    if (!n->b) n->b = New(Node::Block);
                    while (true)
                    {
                        size_t saveVal = p;
                        SkipNL();
                        if (EatPunct(L","))
                        {
                            SkipNL();
                            if (PeekIsStmtEnd()) break;
                            if (IsPunct(L","))
                            {
                                n->b->list.push_back(New(Node::NullK));
                                continue;
                            }
                            // "name = expr" pair → new name + its value
                            if (Peek().t == TIdent)
                            {
                                size_t saveName = p;
                                wstring nm = Next().text;
                                SkipNL();
                                if (Peek().t == TPunct && Peek().text == L"=")
                                {
                                    p++;
                                    SkipNL();
                                    n->names.push_back(nm);
                                    n->b->list.push_back(ParseExpr());
                                    continue;
                                }
                                p = saveName;   // not a pair — plain expr
                            }
                            n->b->list.push_back(ParseExpr());
                        }
                        else
                        {
                            p = saveVal;
                            break;
                        }
                    }
                    return n;
                }
                if (t.text == L"import")
                {
                    // import a.b.c — builtin roots or <scriptdir>\a\b\c.aardio
                    // import ..a.b — root-prefixed form (library root)
                    p++;
                    PNode n = New(Node::Import);
                    while (IsPunct(L".."))
                        p++;   // root prefixes resolve absolutely anyway
                    n->names.push_back(ExpectIdent());
                    while (IsPunct(L".") && Peek(1).t == TIdent)
                    {
                        p++;
                        n->names.push_back(Next().text);
                    }
                    return n;
                }
                if (t.text == L"namespace")
                {
                    // namespace a.b { ... } — bare assignments inside the
                    // block land in the (resolved-or-created) namespace
                    // table; 'namespace a.b;' scopes the REST OF FILE
                    // aardio also supports 'namespace ..Name' (global root prefix)
                    p++;
                    PNode n = New(Node::Namespace);
                    if (IsPunct(L"..") || IsPunct(L"::"))
                    {
                        p++;
                        n->bRoot = true;
                    }
                    n->names.push_back(ExpectIdent());
                    while (IsPunct(L".") && Peek(1).t == TIdent)
                    {
                        p++;
                        n->names.push_back(Next().text);
                    }
                    // aardio: 'namespace a.b;' or 'namespace a.b\n' (newline/EOF/non-brace)
                    // scopes the rest of the file
                    bool bRestOfFile = false;
                    if (EatPunct(L";"))
                        bRestOfFile = true;
                    else if (Peek(-1).t == TNL || (!IsPunct(L"{") && Peek().t != TEnd))
                        bRestOfFile = true;
                    if (bRestOfFile)
                    {
                        // rest-of-file namespace
                        PNode blk = New(Node::Block);
                        while (Peek().t != TEnd)
                        {
                            PNode st = ParseStatement();
                            if (st) blk->list.push_back(st);
                            SkipNL();
                            if (!err->empty()) break;
                        }
                        n->a = blk;
                        return n;
                    }
                    Expect(L"{");
                    n->a = ParseBlock();
                    Expect(L"}");
                    return n;
                }
                if (t.text == L"class")
                {
                    // class Name { ctor(p){...}; member = v; fn = function(){} }
                    // aardio also allows dotted names: class win.form {...}
                    // (declares into the namespace path); 'class ::Name'
                    // declares on the global root
                    p++;
                    PNode n = New(Node::Class);
                    if (IsPunct(L"::") || IsPunct(L".."))
                    {
                        p++;
                        n->bRoot = true;
                    }
                    n->names.push_back(ExpectIdent());
                    while (IsPunct(L".") && Peek(1).t == TIdent)
                    {
                        p++;
                        n->names.push_back(Next().text);
                    }
                    SkipNL();
                    Expect(L"{");
                    n->a = New(Node::Block);
                    wstring structDesc;
                    while (!IsPunct(L"}") && Peek().t != TEnd)
                    {
                        SkipNL();
                        if (IsPunct(L"}")) break;
                        if (IsPunct(L";")) { p++; continue; }
                        // round M2: '@path;' — meta reference on the class
                        if (IsPunct(L"@"))
                        {
                            p++;   // '@'
                            PNode path = ParseExpr();
                            PNode asg = New(Node::Assign);
                            asg->str = L"=";
                            PNode key = New(Node::Str);
                            key->str = L"_meta";
                            PNode idx = New(Node::Index);
                            PNode th = New(Node::Ident);
                            th->str = L"this";
                            idx->a = th;
                            idx->b = key;
                            asg->a = idx;
                            asg->b = path;
                            n->a->list.push_back(asg);
                            SkipNL();
                            EatPunct(L";");
                            continue;
                        }
                        // union member: 'union name = { struct x = Cls(); }'
                        // declares an overlapping sub-struct. The inner
                        // initializer references classes that may be
                        // declared later in the file (mouse/_.aardio), so
                        // it must be stored as a table literal, not
                        // evaluated at definition time.
                        if (Peek().t == TIdent && Peek().text == L"union" &&
                            Peek(1).t == TIdent)
                        {
                            p++;                       // 'union'
                            wstring strUName = Next().text;
                            structDesc += L"union " + strUName + L";";
                            PNode keyU = New(Node::Str);
                            keyU->str = strUName;
                            n->a->list.push_back(keyU);
                            if (EatPunct(L"="))
                            {
                                // keep the sub-struct table unevaluated:
                                // build the value lazily via a function so
                                // forward class references resolve at use
                                PNode fnU = New(Node::FuncLit);
                                PNode bodyU = New(Node::Block);
                                PNode retU = New(Node::Return);
                                retU->a = ParseExpr();
                                bodyU->list.push_back(retU);
                                fnU->a = bodyU;
                                n->a->list.push_back(fnU);
                            }
                            else
                            {
                                n->a->list.push_back(New(Node::NullK));
                            }
                            SkipNL();
                            EatPunct(L";");
                            EatPunct(L",");
                            SkipNL();
                            continue;
                        }
                        // round M2: struct member declaration
                        //   int h;  WORD name[32];  int x = 5;
                        if (Peek().t == TIdent && IsStructType(Peek().text) &&
                            Peek(1).t == TIdent)
                        {
                            wstring strType = Next().text;
                            wstring strName = Next().text;
                            int nArr = 0;
                            if (IsPunct(L"["))
                            {
                                p++;
                                if (IsPunct(L"]"))
                                    nArr = -1;
                                else
                                    nArr = (int)Peek().num, p++;
                                Expect(L"]");
                            }
                            structDesc += strType + L" " + strName +
                                (nArr ? (nArr < 0 ? L"[0]" :
                                         L"[" + std::to_wstring(nArr) + L"]")
                                      : L"") + L";";
                            PNode asgV = New(Node::Assign);
                            asgV->str = L"=";
                            PNode idxV = New(Node::Index);
                            PNode thS = New(Node::Ident);
                            thS->str = L"this";
                            PNode kV = New(Node::Str);
                            kV->str = strName;
                            idxV->a = thS;
                            idxV->b = kV;
                            asgV->a = idxV;
                            if (EatPunct(L"="))
                                asgV->b = ParseExpr();
                            else
                                asgV->b = New(Node::NullK);
                            n->a->list.push_back(asgV);
                            SkipNL();
                            EatPunct(L";");
                            EatPunct(L",");
                            SkipNL();
                            continue;
                        }
                        if (Peek().t == TIdent && Peek().text == L"ctor" &&
                            Peek(1).t == TPunct && Peek(1).text == L"(")
                        {
                            p++;                     // 'ctor'
                            n->b = ParseFuncRest();  // FuncLit: params+body
                        }
                        else
                        {
                            PNode st = ParseStatement();
                            if (st) n->a->list.push_back(st);
                        }
                        SkipNL();
                    }
                    Expect(L"}");
                    if (!structDesc.empty())
                    {
                        PNode asgS = New(Node::Assign);
                        asgS->str = L"=";
                        PNode idxS = New(Node::Index);
                        PNode thS = New(Node::Ident);
                        thS->str = L"this";
                        PNode kS = New(Node::Str);
                        kS->str = L"_struct";
                        idxS->a = thS;
                        idxS->b = kS;
                        PNode vT = New(Node::Str);
                        vT->str = structDesc;
                        asgS->a = idxS;
                        asgS->b = vT;
                        n->a->list.insert(n->a->list.begin(), asgS);
                    }
                    return n;
                }
                if (t.text == L"function")
                {
                    // function name(params) { } -> assignment
                    p++;
                    wstring name = ExpectIdent();
                    PNode fn = ParseFuncRest();
                    PNode asg = New(Node::Assign);
                    asg->str = L"=";   // NB: DoAssign keys off op; unset means compound
                    PNode id = New(Node::Ident);
                    id->str = name;
                    asg->a = id;
                    asg->b = fn;
                    return asg;
                }
            }
            // expression / assignment
            // multi-target assign: a,b,c = e — aardio names unpack
            if (Peek().t == TIdent)
            {
                size_t save = p;
                std::vector<wstring> names;
                names.push_back(Next().text);
                while (IsPunct(L",") && Peek(1).t == TIdent &&
                       (Peek(2).t == TIdent || (Peek(2).t == TPunct && Peek(2).text == L"=") || (Peek(2).t == TPunct && Peek(2).text == L"," && Peek(3).t == TIdent)))
                {
                    p++;
                    names.push_back(Next().text);
                }
                // member/index targets: x.dl,x.dt = a,b — aardio unpacks into
                // member paths too; detect when a target starts with . or [
                if (names.size() == 1 &&
                    (IsPunct(L".") || IsPunct(L"[") || IsPunct(L",")))
                {
                    PNode first = New(Node::Ident);
                    first->str = names[0];
                    first = ParsePostfixFrom(first);
                    std::vector<PNode> tgts;
                    tgts.push_back(first);
                    while (IsPunct(L",") && Peek(1).t == TIdent)
                    {
                        p++;
                        PNode t2 = New(Node::Ident);
                        t2->str = Next().text;
                        tgts.push_back(ParsePostfixFrom(t2));
                    }
                    if (IsPunct(L"=") && tgts.size() > 1)
                    {
                        p++;
                        PNode n = New(Node::Assign);
                        n->str = L"=";
                        n->a = tgts[0];           // first target
                        if (!n->b) n->b = New(Node::Block);
                        // positional extras after the first expr
                        SkipNL();
                        n->b->list.push_back(ParseExpr());
                        while (true)
                        {
                            size_t saveVal = p;
                            SkipNL();
                            if (EatPunct(L","))
                            {
                                SkipNL();
                                if (PeekIsStmtEnd()) break;
                                n->b->list.push_back(ParseExpr());
                            }
                            else
                            {
                                p = saveVal;
                                break;
                            }
                        }
                        // remaining targets live in c->list
                        if (!n->c) n->c = New(Node::Block);
                        for (size_t ti = 1; ti < tgts.size(); ti++)
                            n->c->list.push_back(tgts[ti]);
                        return n;
                    }
                    p = save;
                }
                if (IsPunct(L"=") && names.size() > 1)
                {
                    p++;
                    PNode n = New(Node::Assign);
                    n->str = L"=";
                    n->names = names;
                    SkipNL();
                    n->a = ParseExpr();
                    // a,b = e1,e2 — per-name init list
                    if (!n->b) n->b = New(Node::Block);
                    while (true)
                    {
                        size_t saveVal = p;
                        SkipNL();
                        if (EatPunct(L","))
                        {
                            SkipNL();
                            if (PeekIsStmtEnd()) break;
                            n->b->list.push_back(ParseExpr());
                        }
                        else
                        {
                            p = saveVal;
                            break;
                        }
                    }
                    return n;
                }
                p = save;
            }

            bInAssignLhs = true;
            PNode e = ParseExpr();
            bInAssignLhs = false;
            const wchar_t* ops[] = { L"=", L"::=", L":=", L"+=", L"-=",
                                     L"*=", L"/=", L"%=", L"^=", L"|=",
                                     L"&=", L"^^=", L"<<=", L">>=",
                                     L"++=", L"--=", nullptr };
            for (int k = 0; ops[k]; k++)
            {
                if (Peek().t == TPunct && Peek().text == ops[k])
                {
                    p++;
                    PNode asg = New(Node::Assign);
                    asg->str = ops[k];
                    asg->a = e;
                    asg->b = ParseExpr();
                    return asg;
                }
            }
            // postfix ++/-- : the operator follows an expression that is
            // already parsed. It is a statement on its own, so it must be
            // followed by a terminator — newline, EOF, ';', '}', or the
            // ')' that closes a for(...) step slot / a call argument list.
            if ((IsPunct(L"++") || IsPunct(L"--")) && Peek(1).t != TEnd &&
                (Peek(1).t == TNL || Peek(1).t == TEnd ||
                 (Peek(1).t == TPunct && (Peek(1).text == L";" ||
                                          Peek(1).text == L"}" ||
                                          Peek(1).text == L")" ||
                                          Peek(1).text == L","))))
            {
                p++;   // consume the postfix operator
                PNode asg = New(Node::Assign);
                asg->str = (p >= 2 && toks[p - 1].text == L"++") ? L"+=" : L"-=";
                asg->a = e;
                PNode one = New(Node::Num);
                one->num = 1;
                asg->b = one;
                return asg;
            }
            PNode st = New(Node::ExprStmt);
            st->a = e;
            return st;
        }

        bool PeekIsAssignEnd() const
        {
            return Peek().t == TPunct && Peek().text == L"=";
        }
        bool PeekIsStmtEnd() const
        {
            return Peek().t == TEnd || Peek().t == TNL ||
                   IsPunct(L";") || IsPunct(L"}");
        }

        wstring ExpectIdent()
        {
            if (Peek().t == TIdent)
                return Next().text;
            (*err) = L"语法错误: 期望标识符 @ 行 " + std::to_wstring(Peek().line);
            return L"?";
        }

        PNode ParseFuncRest()
        {
            Expect(L"(");
            PNode fn = New(Node::FuncLit);
            if (!IsPunct(L")"))
            {
                // aardio varargs: function(f, ...) — "..." collects the
                // remaining arguments into a 1-based table
                if (IsPunct(L"..."))
                {
                    p++;
                    fn->names.push_back(L"...");
                    fn->defaults.push_back(nullptr);
                }
                else
                {
                    // aardio default params: function(a, b=1, c="x") — the
                    // default expression is evaluated at call time when the
                    // argument is missing
                    fn->names.push_back(ExpectIdent());
                    if (EatPunct(L"="))
                        fn->defaults.push_back(ParseExpr());
                    else
                        fn->defaults.push_back(nullptr);
                    while (EatPunct(L","))
                    {
                        SkipNL();
                        if (IsPunct(L"..."))
                        {
                            p++;
                            fn->names.push_back(L"...");
                            fn->defaults.push_back(nullptr);
                            break;
                        }
                        fn->names.push_back(ExpectIdent());
                        if (EatPunct(L"="))
                            fn->defaults.push_back(ParseExpr());
                        else
                            fn->defaults.push_back(nullptr);
                    }
                }
            }
            Expect(L")");
            SkipNL();
            if (IsPunct(L"{"))
            {
                Expect(L"{");
                fn->a = ParseBlock();
                Expect(L"}");
            }
            else
            {
                // brace-less body: function() stmt (e.g. for loop, assign, expr)
                PNode blk = New(Node::Block);
                PNode st = ParseStatement();
                if (st)
                {
                    if (st->k == Node::ExprStmt)
                    {
                        PNode r = New(Node::Return);
                        r->a = st->a;
                        blk->list.push_back(r);
                    }
                    else
                    {
                        blk->list.push_back(st);
                    }
                }
                fn->a = blk;
            }
            return fn;
        }

        // body: '{block}' or a single unbraced statement (aardio form)
        PNode ParseBody()
        {
            SkipNL();
            if (IsPunct(L"{"))
            {
                p++;
                PNode blk = ParseBlock();
                Expect(L"}");
                return blk;
            }
            PNode blk = New(Node::Block);
            PNode st = ParseStatement();
            if (st) blk->list.push_back(st);
            return blk;
        }

        PNode ParseIf()
        {
            p++;  // if
            PNode n = New(Node::If);
            n->c = New(Node::Block);   // elseif/else chain lives here
            bool bHadParen = EatPunct(L"(");
            // aardio lib sources span if-conds across lines with inline
            // `return` (win.ui): sequence exprs; last value is the cond,
            // an inline return unwinds during evaluation
            bool bIfParenClosed = false;
            {
                PNode seq = New(Node::Block);
                seq->list.push_back(ParseExpr());
                // shipped lib code writes `if( a = b )` (win.ui 2101) — a
                // lone '=' at cond level reads as equality
                while (IsPunct(L"=") && Peek(1).t != TEnd)
                {
                    PNode asgL = seq->list.back();
                    p++;   // '='
                    PNode n = New(Node::Bin);
                    n->str = L"==";
                    n->a = asgL;
                    n->b = ParseExpr();
                    seq->list[seq->list.size() - 1] = n;
                }
                while (IsPunct(L")") || IsPunct(L"("))
                {
                    // inline `return` continuation only applies while the
                    // if-( group is still open (win.ui multi-line conds);
                    // a paren-less `if cond return x` has return as BODY
                    SkipNL();
                    if (!(Peek().t == TKw && Peek().text == L"return"))
                        break;
                    seq->list.push_back(ParseExpr());
                }
                SkipNL();
                if (seq->list.size() > 1)
                    n->a = seq;
                else
                    n->a = seq->list[0];
            // aardio allows `if( a ) && ( b ) { }` — the if-( closer is
            // followed by && / || groups; fold them into the cond
            while (IsPunct(L")") &&
                   (Peek(1).t == TPunct && (Peek(1).text == L"&&" ||
                                            Peek(1).text == L"||")))
            {
                p++;   // ')'
                bIfParenClosed = true;
                wstring op = Next().text;   // && / ||
                SkipNL();
                PNode rhs;
                if (IsPunct(L"("))
                {
                    p++;
                    rhs = ParseExpr();
                    SkipNL();
                    Expect(L")");
                }
                else
                    rhs = ParseExpr();
                PNode bin = New(Node::Bin);
                bin->str = op;
                bin->a = n->a;
                bin->b = rhs;
                n->a = bin;
                SkipNL();
            }
            }
            // aardio sugar: if( cond { body } ) — the closing ')' may
            // trail the body (win.ui 421-432 uses this form)
            if (IsPunct(L"{"))
            {
                nIfParenOpen++;   // a stray ')' inside the body belongs to if(
                n->b = ParseBody();
                nIfParenOpen--;
                while (EatPunct(L";")) {}
                SkipNL();
                if (IsPunct(L")"))
                    p++;   // the if-( closer trailing the body
            }
            else
            {
                SkipNL();
                if (!bIfParenClosed && bHadParen)
                    Expect(L")");
                nIfParenOpen++;   // a stray ')' inside the body belongs to if(
                n->b = ParseBody();
                nIfParenOpen--;
                while (EatPunct(L";")) {}
                SkipNL();
            }
            // elseif/else chain in c->list: [cond, block, cond, block, ..., elseBlock]
            while (true)
            {
                while (EatPunct(L";")) {}
                SkipNL();
                if (!IsKw(L"elseif")) break;
                p++;
                // parens are optional: 'elseif cond {' (inet/file.aardio)
                // as well as 'elseif (cond) {'
                bool bElseParen = EatPunct(L"(");
                n->c->list.push_back(ParseExpr());
                // the condition may span lines with the ')' on its own
                // line (key/ime.stateBar.aardio), so skip newlines first
                SkipNL();
                if (bElseParen) Expect(L")");
                n->c->list.push_back(ParseBody());
                while (EatPunct(L";")) {}
                SkipNL();
            }
            while (EatPunct(L";")) {}
            SkipNL();
            if (IsKw(L"else"))
            {
                p++;
                SkipNL();
                n->c->list.push_back(ParseBody());  // trailing = else
                while (EatPunct(L";")) {}
                SkipNL();
            }
            return n;
        }

        PNode ParseWhile()
        {
            p++;
            PNode n = New(Node::While);
            Expect(L"(");
            SkipNL();
            std::vector<PNode> condStmts;
            while (!IsPunct(L")") && Peek().t != TEnd)
            {
                PNode st = ParseStatement();
                if (st) condStmts.push_back(st);
                while (EatPunct(L";")) {}
                SkipNL();
                if (!err->empty()) break;
            }
            Expect(L")");
            if (condStmts.size() == 1)
                n->a = condStmts[0];
            else
            {
                PNode blk = New(Node::Block);
                blk->list = std::move(condStmts);
                n->a = blk;
            }
            n->b = ParseBody();
            return n;
        }

        // with(expr) { body } — bind expr's table members as locals for the
        // body scope; `owner` inside the body is the expr value (win.ui)
        PNode ParseWith()
        {
            p++;
            PNode n = New(Node::With);
            SkipNL();
            if (IsPunct(L"("))
            {
                p++;
                n->a = ParseExpr();
                SkipNL();
                Expect(L")");
            }
            else
                n->a = ParseExpr();
            n->b = ParseBody();
            return n;
        }

        // do { body } while(cond) - the C form; condition may be
        // parenthesized or bare
        PNode ParseDoWhile()
        {
            p++;
            PNode n = New(Node::DoWhile);
            SkipNL();
            n->b = ParseBody();
            SkipNL();
            EatKw(L"while");
            SkipNL();
            if (IsPunct(L"("))
            {
                p++;
                n->a = ParseExpr();
                SkipNL();
                Expect(L")");
            }
            else
                n->a = ParseExpr();
            while (EatPunct(L";")) {}
            SkipNL();
            return n;
        }

        // M7: select(v) { case <op?> v1[,v2..] {..} ... else {..} } —
        // identity compare by default (===); an optional leading operator
        // (== != > < >= <=) overrides the comparator; `case a;b` / `case
        // a to b` are inclusive numeric ranges (>= <=); first match wins;
        // `else` / `case else` is the default arm
        PNode ParseSelect()
        {
            p++;   // select
            PNode n = New(Node::If);
            PNode sel = New(Node::Num);   // sentinel: b->num==1 = select form
            sel->num = 1;
            n->b = sel;
            Expect(L"(");
            n->a = ParseExpr();          // the select value
            Expect(L")");
            SkipNL();
            n->c = New(Node::Block);
            Expect(L"{");
            SkipNL();
            while (!IsPunct(L"}") && Peek().t != TEnd)
            {
                SkipNL();
                if (IsPunct(L"}")) break;
                bool bElse = false;
                wstring op = L"===";
                if (IsKw(L"case"))
                {
                    p++;
                    // optional comparator
                    static const wchar_t* CMP[] = { L"===", L"!==", L"==",
                        L"!=", L">=", L"<=", L">", L"<", nullptr };
                    for (int k = 0; CMP[k]; k++)
                    {
                        if (IsPunct(CMP[k]))
                        {
                            op = CMP[k];
                            p++;
                            break;
                        }
                    }
                    if (IsKw(L"else"))
                    {
                        p++;
                        bElse = true;
                    }
                }
                else if (IsKw(L"else"))
                {
                    p++;
                    bElse = true;
                }
                else
                {
                    (*err) = L"语法错误: select 内期望 case @ 行 " +
                             std::to_wstring(Peek().line);
                    return n;
                }
                PNode blk;
                if (!bElse)
                {
                    // case values: v1[,v2...] and/or range a;b / a to b
                    PNode c1 = ParseExpr();
                    if (IsPunct(L";") ||
                        (Peek().t == TIdent && Peek().text == L"to"))
                    {
                        p++;
                        PNode c2 = ParseExpr();
                        // encode a range as a Bin node: str="range",
                        // a=v1, b=v2, c=Str(op) — eval applies >= <=
                        PNode rng = New(Node::Bin);
                        rng->str = L"range";
                        rng->a = c1;
                        rng->b = c2;
                        rng->c = nullptr;   // NB: null = chain tail (else arms misfire)
                        n->c->list.push_back(rng);
                    }
                    else
                    {
                        // first value (op may be an operator form); the
                        // multi-value chain continues via ','
                        PNode first = New(Node::Bin);
                        first->str = op;
                        first->a = nullptr;   // a = select value (filled at eval)
                        first->b = c1;
                        PNode chain = first;
                        while (EatPunct(L","))
                        {
                            SkipNL();
                            PNode nxt = New(Node::Bin);
                            nxt->str = op;
                            nxt->a = nullptr;
                            nxt->b = ParseExpr();
                            chain->c = nxt;   // list through c
                            chain = nxt;
                        }
                        n->c->list.push_back(first);   // NB: chain walked to the tail
                    }
                }
                else
                    n->c->list.push_back(nullptr);
                SkipNL();
                // arm body
                n->c->list.push_back(ParseBody());
                while (EatPunct(L";")) {}
                SkipNL();
            }
            Expect(L"}");
            return n;
        }

        PNode ParseFor()
        {
            p++;
            bool paren = EatPunct(L"(");
            // for-in: for k, v in tab { } (parenthesized form also accepted)
            if (Peek().t == TIdent &&
                (Peek(1).t == TKw && Peek(1).text == L"in" ||
                 (Peek(1).t == TPunct && Peek(1).text == L",")))
            {
                PNode n = New(Node::ForIn);
                n->names.push_back(Next().text);
                while (EatPunct(L","))
                    n->names.push_back(ExpectIdent());
                if (!EatKw(L"in"))
                {
                    (*err) = L"语法错误: for-in 缺少 in @ 行 " +
                             std::to_wstring(Peek().line);
                    return n;
                }
                n->a = ParseExpr();
                if (paren) Expect(L")");
                n->b = ParseBody();
                return n;
            }
            // numeric: for(i=1;cond;step){} — step/cond may be omitted;
            // 2-expr form for(i=n;limit) counts n..limit (1 if limit<n).
            // The parens are optional in the numeric form too: the library
            // writes 'for x=x1;x2;step { ... }' (mouse/_.aardio).
            PNode n = New(Node::ForNum);
            if (!paren && IsPunct(L"("))
            {
                p++;
                paren = true;
            }
            n->a = ParseStatement();     // init (assign)
            // slot separators: ';' normally. A dialect spells all three
            // slots with commas — 'for(i=1,len,1)' (fsys/aar.aardio,
            // dotNet/_.aardio). Accept ',' where ';' was expected.
            if (!EatPunct(L";"))
                EatPunct(L",");
            SkipNL();
            // the condition slot may be empty: for(i=0;;i++) runs forever
            if (!IsPunct(L";") && !IsPunct(L",") && !IsPunct(L")"))
                n->b = ParseExpr();      // cond
            if (!EatPunct(L";"))
                EatPunct(L",");
            SkipNL();
            if (!IsPunct(L")"))
                n->c = ParseStatement(); // step (may be absent)
            // the unparenthesized form has no closing paren to consume
            if (paren)
                Expect(L")");
            n->names.push_back(L"limit");  // tag: middle is a limit
            n->d = ParseBody();
            return n;
        }

        PNode ParseTry()
        {
            // try { } catch(e) { } — e optional
            p++;   // try
            PNode n = New(Node::Try);
            SkipNL();
            Expect(L"{");
            n->a = ParseBlock();
            Expect(L"}");
            SkipNL();
            if (IsKw(L"catch"))
            {
                p++;
                SkipNL();
                if (EatPunct(L"("))
                {
                    n->names.push_back(ExpectIdent());
                    Expect(L")");
                    SkipNL();
                }
                Expect(L"{");
                n->b = ParseBlock();
                Expect(L"}");
            }
            return n;
        }

        PNode New(Node::K k)   // member: stamps the current token line
        {
            PNode n = std::make_shared<Node>();
            n->k = k;
            n->line = Peek().line;
            return n;
        }

        // expressions (precedence climbing)
        bool bNoCoalesce = false;   // inside a ternary's middle branch
        int nIfParenOpen = 0;       // aardio if( cond { } ) sugar
        bool bInAssignLhs = false;  // top-level statement assignment target
        PNode ParseExpr() { return ParseTernary(); }
        PNode ParseTernary()
        {
            PNode l = ParseOr();
            // aardio also lets the ? start the next line (win/_.aardio 315)
            size_t saveT = p;
            SkipNL();
            bool bQ = IsPunct(L"?");
            if (!bQ)
                p = saveT;
            if (bQ)
            {
                p++;
                PNode n = New(Node::Bin);
                n->str = L"?:";
                n->a = l;
                // the middle branch must not swallow the ternary's ':'
                bool bOld = bNoCoalesce;
                bNoCoalesce = true;
                n->b = ParseExpr();
                bNoCoalesce = bOld;
                SkipNL();
                if (IsPunct(L":"))
                {
                    p++;
                    n->c = ParseExpr();
                    return n;      // true ternary cond ? a : b
                }
                // aardio `a ? b` guard: null/false → false, else b (lazy);
                // encode as Bin "?" — eval: a truthy ? b : false
                n->str = L"?";
                return n;
            }
            return l;
        }
        PNode ParseOr()
        {
            PNode l = ParseAnd();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"||") || IsKw(L"or")))
                {
                    p = save;
                    break;
                }
                p++;
                PNode n = New(Node::Bin);
                n->str = L"||";
                n->a = l;
                // the right operand may start on the next line (the library
                // writes '(...) or\n    (...)')
                SkipNL();
                n->b = ParseAnd();
                l = n;
            }
            // aardio a:b — null coalesce (b evaluated lazily)
            if (bNoCoalesce)
                return l;
            while (true)
            {
                size_t saveC = p;
                SkipNL();
                if (!(IsPunct(L":") && Peek(1).t != TEnd &&
                      !(Peek(1).t == TPunct && Peek(1).text == L"=")))
                {
                    p = saveC;
                    break;
                }
                p++;
                PNode n = New(Node::Bin);
                n->str = L":";
                n->a = l;
                // the right operand may start on the next line (the library
                // splits loadDll( exist(...) : $"dll" ) across lines)
                SkipNL();
                n->b = ParseAnd();
                l = n;
            }
            return l;
        }
        PNode ParseAnd()
        {
            PNode l = ParseEq();
            while (true)
            {
                // aardio: a binary operator may start the NEXT line
                // aardio: a binary operator may start the NEXT line —
                // peek past newlines and restore when no operator follows
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"&&") || IsKw(L"and")))
                {
                    p = save;
                    break;
                }
                p++;
                PNode n = New(Node::Bin);
                n->str = L"&&";
                n->a = l;
                n->b = ParseEq();
                l = n;
            }
            return l;
        }
        PNode ParseBitOr()
        {
            PNode l = ParseBitXor();
            // "|=" is a statement-level compound assignment; while parsing
            // an assign LHS it must reach the assignment handler instead
            while (IsPunct(L"|") || (!bInAssignLhs && IsPunct(L"|=")))
            {
                wstring op = Next().text;
                if (op == L"|=")
                {
                    // compound assignment: build an Assign node so DoAssign
                    // applies the op and stores through the LHS target
                    PNode n = New(Node::Assign);
                    n->str = L"|=";
                    n->a = l;
                    n->b = ParseBitXor();
                    l = n;
                }
                else
                {
                    PNode n = New(Node::Bin);
                    n->str = L"|";
                    n->a = l;
                    n->b = ParseBitXor();
                    l = n;
                }
            }
            return l;
        }
        PNode ParseBitXor()
        {
            PNode l = ParseBitAnd();
            while (IsPunct(L"^"))
            {
                p++;
                PNode n = New(Node::Bin);
                n->str = L"^";   // aardio: ^ is bitwise XOR (** is exponent)
                n->a = l;
                n->b = ParseBitAnd();
                l = n;
            }
            return l;
        }
        PNode ParseBitAnd()
        {
            PNode l = ParseShift();
            // "&=" is a statement-level compound assignment (see ParseBitOr)
            while (IsPunct(L"&") || (!bInAssignLhs && IsPunct(L"&=")))
            {
                wstring op = Next().text;
                PNode n = New(op == L"&=" ? Node::Assign : Node::Bin);
                n->str = op;
                n->a = l;
                n->b = ParseShift();
                l = n;
            }
            return l;
        }
        PNode ParseShift()
        {
            PNode l = ParseCmp();
            while (true)
            {
                size_t save = p;
                SkipNL();
                // "<<=" / ">>=" are statement-level compound assignments
                // (see ParseBitOr); the plain shifts stay binary here
                if (!(IsPunct(L"<<") || IsPunct(L">>") || IsPunct(L">>>") ||
                      (!bInAssignLhs && IsPunct(L"<<=")) ||
                      (!bInAssignLhs && IsPunct(L">>="))))
                {
                    p = save;
                    break;
                }
                wstring op = Next().text;
                bool bCompound = (op == L"<<=" || op == L">>=");
                PNode n = New(bCompound ? Node::Assign : Node::Bin);
                n->str = op;
                n->a = l;
                SkipNL();
                n->b = ParseCmp();
                l = n;
            }
            return l;
        }
        PNode ParseEq()
        {
            PNode l = ParseBitOr();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"===") || IsPunct(L"==") || IsPunct(L"!=") ||
                      IsPunct(L"!==") || (!bInAssignLhs && IsPunct(L"="))))
                {
                    p = save;
                    break;
                }
                wstring op = Next().text;
                if (op == L"=") op = L"==";
                PNode n = New(Node::Bin);
                n->str = op;
                n->a = l;
                SkipNL();
                n->b = ParseBitOr();
                l = n;
            }
            return l;
        }
        PNode ParseCmp()
        {
            PNode l = ParseConcat();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"<") || IsPunct(L">") || IsPunct(L"<=") || IsPunct(L">=")))
                {
                    p = save;
                    break;
                }
                PNode n = New(Node::Bin);
                n->str = Next().text;
                n->a = l;
                SkipNL();
                n->b = ParseConcat();
                l = n;
            }
            return l;
        }
        PNode ParseConcat()   // '..'/'++' bind below + (concat level), right assoc
        {
            PNode l = ParseAdd();
            while (true)
            {
                // a binary '++' may start the NEXT line (the library splits
                // long SQL/XML assembly across lines with a leading '++').
                // '..' must NOT be treated that way: a '..' at the start of
                // a line is the root-namespace accessor (`..thread.delay =
                // delay;`), not the concat operator.
                size_t save = p;
                SkipNL();
                bool bSkippedNL = (p != save);
                // aardio postfix 'expr++' (increment statement): when '++'
                // is followed by a statement/argument end it binds to the
                // left expr, not concat. ')' and ',' matter for the
                // for(...) step slot and call argument lists.
                if (IsPunct(L"++") || IsPunct(L"--"))
                {
                    const Tok& nx = Peek(1);
                    bool bEnd = nx.t == TNL || nx.t == TEnd ||
                                (nx.t == TPunct &&
                                 (nx.text == L";" || nx.text == L"}" ||
                                  nx.text == L")" || nx.text == L","));
                    if (bEnd)
                        return l;   // leave the operator for the statement
                }
                if (IsPunct(L"++") || (!bSkippedNL && IsPunct(L"..")))
                {
                    PNode n = New(Node::Bin);
                    n->str = Next().text;
                    n->a = l;
                    SkipNL();
                    n->b = ParseConcat();
                    l = n;
                    continue;
                }
                p = save;
                return l;
            }
        }
        PNode ParseAdd()
        {
            PNode l = ParseMul();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"+") || IsPunct(L"-")))
                {
                    p = save;
                    break;
                }
                PNode n = New(Node::Bin);
                n->str = Next().text;
                n->a = l;
                SkipNL();
                n->b = ParseMul();
                l = n;
            }
            return l;
        }
        PNode ParseMul()
        {
            PNode l = ParseUnary();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"*") || IsPunct(L"/") || IsPunct(L"%")))
                {
                    p = save;
                    break;
                }
                PNode n = New(Node::Bin);
                n->str = Next().text;
                n->a = l;
                SkipNL();
                n->b = ParseUnary();
                l = n;
            }
            return l;
        }
        PNode ParseUnary()
        {
            if (IsPunct(L"-") || IsPunct(L"!") || IsKw(L"not") ||
                IsPunct(L"#") || IsPunct(L"~"))
            {
                PNode n = New(Node::Un);
                n->str = Peek().text;
                p++;
                n->a = ParseUnary();
                return n;
            }
            return ParsePow();
        }
        PNode ParsePow()
        {
            PNode l = ParsePostfix();
            // aardio: ** is exponent; ^ is bitwise XOR (ParseBitXor)
            if (IsPunct(L"**"))
            {
                wstring op = Next().text;
                PNode n = New(Node::Bin);
                n->str = op;
                n->a = l;
                n->b = ParseUnary();   // right assoc
                return n;
            }
            return l;
        }
        PNode ParsePostfix()
        {
            PNode e = ParsePrimary();
            while (true)
            {
                if (IsPunct(L".") && (Peek(1).t == TIdent || Peek(1).t == TKw || (Peek(1).t == TPunct && Peek(1).text == L"$")))
                {
                    p++;
                    PNode m = New(Node::Member);
                    m->a = e;
                    // '$name' members span two tokens ('$' + ident)
                    if (Peek().t == TPunct && Peek().text == L"$" &&
                        Peek(1).t == TIdent)
                    {
                        p++;
                        m->str = L"$" + Next().text;
                    }
                    else
                        m->str = Next().text;
                    e = m;
                }
                else if (IsPunct(L"?."))
                {
                    p++;
                    PNode m = New(Node::Member);
                    m->bSafe = true;
                    m->a = e;
                    if (Peek().t == TPunct && Peek().text == L"$" &&
                        Peek(1).t == TIdent)
                    {
                        p++;
                        m->str = L"$" + Next().text;
                    }
                    else
                        m->str = Next().text;
                    e = m;
                }
                else if (IsPunct(L"?["))
                {
                    p++;
                    PNode m = New(Node::Index);
                    m->bSafe = true;
                    m->a = e;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"[") && Peek(1).t == TPunct &&
                         Peek(1).text == L"[")
                {
                    // aardio t[["k"]] — direct-key index, skips metatables
                    p += 2;
                    PNode m = New(Node::Index);
                    m->a = e;
                    m->bDirect = true;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"@") && Peek(1).t != TIdent)
                {
                    // aardio 'tab@' — metatable get/set (postfix @ not
                    // followed by an ident path; '@path' is the M2 form)
                    p++;
                    PNode m = New(Node::Member);
                    m->bMeta = true;
                    m->a = e;
                    m->str = L"_meta";
                    e = m;
                }
                else if (IsPunct(L"#"))
                {
                    // postfix # — aardio expr# = length (string chars /
                    // table array length)
                    p++;
                    PNode n = New(Node::Un);
                    n->str = L"#";
                    n->a = e;
                    e = ParsePostfixCont(n);
                }
                else if (IsPunct(L"["))
                {
                    p++;
                    PNode m = New(Node::Index);
                    m->a = e;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"("))
                {
                    p++;
                    PNode c = New(Node::Call);
                    c->a = e;
                    SkipNL();
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    if (!IsPunct(L")"))
                    {
                        // aardio f(,,,,1): leading empty slots
                        if (IsPunct(L",") )
                            c->list.push_back(New(Node::NullK));
                        else if (IsPunct(L")"))
                            c->list.push_back(New(Node::NullK));  // empty slot before )
                        else if (Peek().t == TIdent && Peek(1).t == TPunct && Peek(1).text == L"=")
                        {
                            // named args: f(name = expr; ...) — one table arg
                            PNode tbl = New(Node::TableCtor);
                            while (Peek().t == TIdent && Peek(1).t == TPunct &&
                                   Peek(1).text == L"=")
                            {
                                PNode key = New(Node::Str);
                                key->str = Next().text;
                                p++;
                                tbl->list.push_back(key);
                                tbl->list.push_back(ParseExpr());
                                SkipNL();
                                EatPunct(L";");
                                SkipNL();
                                if (IsPunct(L",")) { p++; SkipNL(); continue; }
                            }
                            c->list.push_back(tbl);
                        }
                        else
                            c->list.push_back(ParseExpr());
                        SkipNL();
                        while (EatPunct(L","))
                            {
                                SkipNL();
                                if (IsPunct(L",") || IsPunct(L")"))
                                    c->list.push_back(New(Node::NullK));
                                else if (IsPunct(L")"))
                                    c->list.push_back(New(Node::NullK));  // empty slot before )
                                else
                                    c->list.push_back(ParseExpr());
                                SkipNL();
                            }
                    }
                    bInAssignLhs = bOldLhs;
                    Expect(L")");
                    e = c;
                }
                else if (IsPunct(L"..") && Peek(1).t == TIdent &&
                         !(Peek(1).line != Peek().line))   // root: '..' + ident
                {
                    // '..name' after expression start = global root member;
                    // handled in ParsePrimary for statement-leading form.
                    break;
                }
                else break;
            }
            return e;
        }
        // $"path" compile-time embed — resolve the path, read the file.
        // Text files embed their content; binary payloads embed the
        // resolved absolute path (raw.loadDll keeps loading by path).
        wstring ResolveEmbedPath(const wstring& strPath)
        {
            wstring rel = strPath;
            wstring rootForm;
            if (!rel.empty() && rel[0] == L'~' &&
                rel.size() > 1 && (rel[1] == L'/' || rel[1] == L'\\'))
            {
                rootForm = strEmbedRoot.empty() ? L""
                    : strEmbedRoot + L"\\" + rel.substr(2);
                rel = rel.substr(2);
            }
            // candidates: ~ root first, then the parsing file's dir,
            // then verbatim (absolute paths hit this last branch)
            wstring cand;
            if (!rootForm.empty() && _waccess(rootForm.c_str(), 0) == 0)
                cand = rootForm;
            else if (!strEmbedBase.empty() &&
                     _waccess((strEmbedBase + L"\\" + rel).c_str(), 0) == 0)
                cand = strEmbedBase + L"\\" + rel;
            else if (_waccess(rel.c_str(), 0) == 0)
                cand = rel;
            else if (!rootForm.empty())
                cand = rootForm;
            else
                cand = strEmbedBase.empty() ? rel
                       : strEmbedBase + L"\\" + rel;

            FILE* pf = nullptr;
            if (_wfopen_s(&pf, cand.c_str(), L"rb") != 0 || !pf)
            {
                // Unresolved. A "~\" path that exists neither on disk nor
                // in the aardio root is a published-payload resource: the
                // embed map only exists at runtime, so mark the path for
                // deferred resolution instead of inlining the path text
                // (which would make raw.loadDll fail on a nonexistent file).
                if (rel != strPath && strPath.size() > 2)
                    return L"$NODIOEMBED$" + rel;
                return cand;   // unresolved — surface the path in errors
            }
            fseek(pf, 0, SEEK_END);
            long cb = ftell(pf);
            fseek(pf, 0, SEEK_SET);
            std::vector<char> raw((size_t)cb);
            size_t rd = cb > 0 ? fread(raw.data(), 1, (size_t)cb, pf) : 0;
            fclose(pf);
            raw.resize(rd);

            // binary sniff: NUL byte in the first 4KB → payload, not text.
            // Binary files (DLL/ICO/PNG/...) embed their RAW BYTES as a
            // byte string (one byte per wchar, the engine's byte-string
            // convention) so raw.loadDll can map them from memory without
            // the source file — this is what a published exe needs.
            bool bBinary = false;
            size_t nSniff = min(rd, (size_t)4096);
            for (size_t k = 0; k < nSniff; k++)
                if (raw[k] == 0) { bBinary = true; break; }
            if (bBinary)
            {
                wstring w;
                w.resize(rd);
                for (size_t k = 0; k < rd; k++)
                    w[k] = (wchar_t)(unsigned char)raw[k];
                return w;
            }

            // UTF-8 (BOM or bare) → wide text content
            size_t off = (rd >= 3 && (unsigned char)raw[0] == 0xEF &&
                          (unsigned char)raw[1] == 0xBB &&
                          (unsigned char)raw[2] == 0xBF) ? 3 : 0;
            int w = MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                        (int)(rd - off), nullptr, 0);
            std::vector<wchar_t> wide((size_t)max(w, 0) + 1, 0);
            if (w > 0)
                MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                    (int)(rd - off), wide.data(), w);
            return wstring(wide.data(), (size_t)w);
        }

        PNode ParsePrimary()
        {
            const Tok& t = Peek();
            // "..." expression = the varargs table
            if (t.t == TPunct && t.text == L"...")
            {
                p++;
                PNode n = New(Node::Ident);
                n->str = L"...";
                return n;
            }
            if (t.t == TNum) { p++; PNode n = New(Node::Num); n->num = t.num; return n; }
            // compile-time file embed: $"path" — official aardio inlines
            // the file at parse time. Text files embed their content;
            // binary payloads (dll/ico/...) embed their raw bytes as a
            // byte string so raw.loadDll maps them from memory.
            // '$' lexes as an identifier character (IsIdentChar), so the
            // embed marker arrives as TIdent "$" (or TPunct "$" when
            // adjacent to a quote in some lexer paths) — accept both
            if ((t.t == TIdent || t.t == TPunct) && t.text == L"$" &&
                Peek(1).t == TStr)
            {
                wstring strPath = Peek(1).text;
                p += 2;
                PNode n = New(Node::Str);
                n->str = ResolveEmbedPath(strPath);
                return n;
            }
            if (t.t == TStr) { p++; PNode n = New(Node::Str); n->str = t.text; return n; }
            if (t.t == TIdent)
            {
                p++;
                PNode n = New(Node::Ident);
                n->str = t.text;
                return n;
            }
            // root namespace '..ident' / '..io.print'
            if (IsPunct(L"..") && Peek(1).t == TIdent)
            {
                p += 2;
                PNode m = New(Node::Member);
                m->bRoot = true;
                PNode root = New(Node::Ident);
                root->str = L"..";
                m->a = root;
                m->str = Peek(-1).text;
                // continue postfix chain from the member
                return ParsePostfixFrom(m);
            }
            // '::ident' / '::io.print' — same root scope (aardio ::)
            if (IsPunct(L"::") && Peek(1).t == TIdent)
            {
                p += 2;
                PNode m = New(Node::Member);
                m->bRoot = true;
                PNode root = New(Node::Ident);
                root->str = L"..";
                m->a = root;
                m->str = Peek(-1).text;
                return ParsePostfixFrom(m);
            }
            if (IsPunct(L"("))
            {
                p++;
                SkipNL();
                // grouping restores null-coalesce freedom: a ternary
                // middle suppresses ':' but a nested '( a : b )'
                // inside it has its own scope (win/ui/ctrl 61)
                bool bOldC = bNoCoalesce;
                bNoCoalesce = false;
                bool bOldLhs = bInAssignLhs;
                bInAssignLhs = false;
                PNode e = ParseExpr();
                bInAssignLhs = bOldLhs;
                bNoCoalesce = bOldC;
                SkipNL();
                Expect(L")");
                return ParsePostfixFrom(e);
            }
            if (IsKw(L"true") || IsKw(L"false"))
            {
                p++;
                PNode n = New(Node::Bool);
                n->num = t.text == L"true" ? 1 : 0;
                return n;
            }
            if (IsKw(L"null")) { p++; return New(Node::NullK); }
            if (IsKw(L"function"))
            {
                p++;
                return ParseFuncRest();
            }
            // aardio lambda: lambda(a,b) expr — implicit return
            if (t.t == TKw && t.text == L"lambda")
            {
                p++;
                Expect(L"(");
                PNode fn = New(Node::FuncLit);
                if (!IsPunct(L")"))
                {
                    // lambda(f,...) - varargs param supported
                    if (IsPunct(L"..."))
                    {
                        p++;
                        fn->names.push_back(L"...");
                        fn->defaults.push_back(nullptr);
                    }
                    else
                    {
                    fn->names.push_back(ExpectIdent());
                    if (EatPunct(L"="))
                        fn->defaults.push_back(ParseExpr());
                    else
                        fn->defaults.push_back(nullptr);
                    while (EatPunct(L","))
                    {
                        SkipNL();
                        if (IsPunct(L"..."))
                        {
                            p++;
                            fn->names.push_back(L"...");
                            fn->defaults.push_back(nullptr);
                            break;
                        }
                        fn->names.push_back(ExpectIdent());
                        if (EatPunct(L"="))
                            fn->defaults.push_back(ParseExpr());
                        else
                            fn->defaults.push_back(nullptr);
                    }
                    }
                }
                Expect(L")");
                PNode body = New(Node::Block);
                PNode retSt = New(Node::Return);
                retSt->a = ParseExpr();
                // brace-less body may end in an assignment
                // (function(v) owner.x = v inside a table literal)
                if (Peek().t == TPunct &&
                    (Peek().text == L"=" || Peek().text == L":="))
                {
                    PNode asg = New(Node::Assign);
                    asg->str = Next().text;
                    asg->a = retSt->a;
                    asg->b = ParseExpr();
                    retSt->a = asg;
                }
                body->list.push_back(retSt);
                fn->a = body;
                return fn;
            }
            if (IsPunct(L"["))
            {
                // aardio array literal: [a,b,c] — 1-based table array
                // and [@meta] / [@meta, a, b]
                p++;
                PNode n = New(Node::TableCtor);
                SkipNL();
                int nArr = 0;
                while (!IsPunct(L"]") && Peek().t != TEnd)
                {
                    SkipNL();
                    if (IsPunct(L"]")) break;
                    if (IsPunct(L"@"))
                    {
                        p++;
                        PNode path = ParseExpr();
                        PNode key = New(Node::Str);
                        key->str = L"_meta";
                        n->list.push_back(key);
                        n->list.push_back(path);
                        SkipNL();
                        EatPunct(L";");
                        EatPunct(L",");
                        continue;
                    }
                    PNode kN = New(Node::Num);
                    kN->num = ++nArr;
                    n->list.push_back(kN);
                    n->list.push_back(ParseExpr());
                    SkipNL();
                    EatPunct(L",");
                }
                SkipNL();
                Expect(L"]");
                return n;
            }
            // aardio class expression: ::MSG = class{ ... } (no name)
            if (t.t == TKw && t.text == L"class")
            {
                p++;
                PNode n = New(Node::Class);
                if (IsPunct(L"::"))
                    p++;
                if (Peek().t == TIdent)
                    n->names.push_back(Next().text);
                SkipNL();
                Expect(L"{");
                n->a = New(Node::Block);
                wstring structDesc;
                while (!IsPunct(L"}") && Peek().t != TEnd)
                {
                    SkipNL();
                    if (IsPunct(L"}")) break;
                    if (IsPunct(L";")) { p++; continue; }
                    // member: name = expr | struct decl | '@path;'
                    if (IsPunct(L"@"))
                    {
                        p++;
                        PNode path = ParseExpr();
                        PNode asg = New(Node::Assign);
                        asg->str = L"=";
                        PNode key = New(Node::Str);
                        key->str = L"_meta";
                        PNode idx = New(Node::Index);
                        PNode th = New(Node::Ident);
                        th->str = L"this";
                        idx->a = th;
                        idx->b = key;
                        asg->a = idx;
                        asg->b = path;
                        n->a->list.push_back(asg);
                        SkipNL();
                        EatPunct(L";");
                        continue;
                    }
                    if (Peek().t == TIdent && IsStructType(Peek().text) &&
                        Peek(1).t == TIdent)
                    {
                        wstring strType = Next().text;
                        wstring strName = Next().text;
                        int nArr = 0;
                        if (IsPunct(L"["))
                        {
                            p++;
                            if (IsPunct(L"]"))
                                nArr = -1;
                            else
                                nArr = (int)Peek().num, p++;
                            Expect(L"]");
                        }
                        structDesc += strType + L" " + strName +
                            (nArr ? (nArr < 0 ? L"[0]" :
                                     L"[" + std::to_wstring(nArr) + L"]")
                                  : L"") + L";";
                        PNode asg = New(Node::Assign);
                        asg->str = L"=";
                        PNode key = New(Node::Str);
                        key->str = strName;
                        PNode idx = New(Node::Index);
                        PNode th = New(Node::Ident);
                        th->str = L"this";
                        idx->a = th;
                        idx->b = key;
                        asg->a = idx;
                        // struct member with initializer: INT cbSize = 48;
                        if (EatPunct(L"="))
                            asg->b = ParseExpr();
                        else
                        {
                            // default-init the struct member as null
                            PNode nn = New(Node::NullK);
                            asg->b = nn;
                        }
                        n->a->list.push_back(asg);
                        SkipNL();
                        EatPunct(L";");
                        EatPunct(L",");
                        SkipNL();
                        continue;
                    }
                    if ((Peek().t == TIdent || Peek().t == TKw) &&
                        Peek(1).t == TPunct && Peek(1).text == L"=")
                    {
                        PNode asg = New(Node::Assign);
                        asg->str = L"=";
                        PNode id = New(Node::Index);
                        PNode th = New(Node::Ident);
                        th->str = L"this";
                        PNode key = New(Node::Str);
                        key->str = Peek().text;
                        id->a = th;
                        id->b = key;
                        p += 2;
                        asg->a = id;
                        asg->b = ParseExpr();
                        n->a->list.push_back(asg);
                        SkipNL();
                        EatPunct(L";");
                        continue;
                    }
                    // ctor: ctor(params){...} — the constructor body is
                    // stored on n->b (FuncLit); without this branch the
                    // block after ctor(...) would close the class early
                    if (Peek().t == TIdent && Peek().text == L"ctor" &&
                        Peek(1).t == TPunct && Peek(1).text == L"(")
                    {
                        p++;                     // 'ctor'
                        n->b = ParseFuncRest();  // FuncLit: params+body
                    }
                    else
                    {
                        PNode st = ParseStatement();
                        if (st) n->a->list.push_back(st);
                    }
                    SkipNL();
                }
                Expect(L"}");
                if (!structDesc.empty())
                {
                    PNode asgS = New(Node::Assign);
                    asgS->str = L"=";
                    PNode idxS = New(Node::Index);
                    PNode thS = New(Node::Ident);
                    thS->str = L"this";
                    PNode kS = New(Node::Str);
                    kS->str = L"_struct";
                    idxS->a = thS;
                    idxS->b = kS;
                    PNode vT = New(Node::Str);
                    vT->str = structDesc;
                    asgS->a = idxS;
                    asgS->b = vT;
                    n->a->list.insert(n->a->list.begin(), asgS);
                }
                return n;
            }
            // M7: `return` in expression position — aardio allows
            // `if( cond
            //      return f(x) )` in lib sources (win.ui): the inline
            // return exits the enclosing function with the value
            if (t.t == TKw && t.text == L"return")
            {
                p++;
                PNode n = New(Node::Return);
                if (!PeekIsStmtEnd() && !IsPunct(L")") && !IsPunct(L","))
                    n->a = ParseExpr();
                return n;
            }
            if (IsPunct(L"{"))
            {
                p++;
                PNode n = New(Node::TableCtor);
                SkipNL();
                wstring structDesc;
                while (!IsPunct(L"}") && Peek().t != TEnd)
                {
                    SkipNL();
                    if (IsPunct(L"}")) break;
                    // round M2: '@path;' — meta reference (stores under
                    // the "_meta" key; aardio tables/classes attach their
                    // metatable this way)
                    if (IsPunct(L"@"))
                    {
                        p++;   // '@'
                        PNode path = ParseExpr();
                        PNode key = New(Node::Str);
                        key->str = L"_meta";
                        n->list.push_back(key);
                        n->list.push_back(path);
                        SkipNL();
                        EatPunct(L";");
                        continue;
                    }
                    // round M2: struct member declaration —
                    //   int x;  WORD name[32];  int left = expr;
                    // emits a normal pair (init or null) plus a "_struct"
                    // layout string consumed by raw.struct/raw.api/raw.convert
                    if (Peek().t == TIdent && IsStructType(Peek().text) &&
                        Peek(1).t == TIdent)
                    {
                        wstring strType = Next().text;
                        wstring strName = Next().text;
                        int nArr = 0;
                        if (IsPunct(L"["))
                        {
                            p++;
                            if (IsPunct(L"]"))
                                nArr = -1;    // variable length [0]
                            else
                                nArr = (int)Peek().num, p++;
                            Expect(L"]");
                        }
                        structDesc += strType + L" " + strName +
                            (nArr ? (nArr < 0 ? L"[0]" :
                                     L"[" + std::to_wstring(nArr) + L"]")
                                 : L"") + L";";
                        // initializer or null value
                        PNode keyN = New(Node::Str);
                        keyN->str = strName;
                        n->list.push_back(keyN);
                        if (EatPunct(L"="))
                            n->list.push_back(ParseExpr());
                        else
                        {
                            PNode nn = New(Node::NullK);
                            n->list.push_back(nn);
                        }
                        SkipNL();
                        EatPunct(L";");
                        EatPunct(L",");
                        SkipNL();
                        continue;
                    }
                    // pair: name = expr | name: expr (aardio allows both separators;
                    // win/_.aardio uses {max: 3;min: 6}) | ["k"] = expr |
                    // expr (array item)
                    if ((Peek().t == TIdent || Peek().t == TKw) && Peek(1).t == TPunct &&
                        (Peek(1).text == L"=" || Peek(1).text == L":"))
                    {
                        bool bColonPair = (Peek(1).text == L":");
                        PNode key = New(Node::Str);
                        key->str = Peek().text;
                        p += 2;
                        n->list.push_back(key);
                        if (bColonPair)
                        {
                            // colon value must not eat the next pair
                            bool bOld = bNoCoalesce;
                            bNoCoalesce = true;
                            n->list.push_back(ParseExpr());
                            bNoCoalesce = bOld;
                        }
                        else
                            n->list.push_back(ParseExpr());
                    }
                    else if (IsPunct(L"["))
                    {
                        p++;
                        PNode key = ParseExpr();
                        Expect(L"]");
                        Expect(L"=");
                        n->list.push_back(key);
                        n->list.push_back(ParseExpr());
                    }
                    else
                    {
                        PNode idx = New(Node::Num);
                        idx->num = (double)n->list.size() / 2 + 1;   // 1-based
                        n->list.push_back(idx);
                        n->list.push_back(ParseExpr());
                    }
                    SkipNL();
                    EatPunct(L";");
                    if (!EatPunct(L","))
                    {
                        // newline separated pairs are fine
                    }
                    SkipNL();
                }
                Expect(L"}");
                if (!structDesc.empty())
                {
                    PNode keyS = New(Node::Str);
                    keyS->str = L"_struct";
                    PNode valS = New(Node::Str);
                    valS->str = structDesc;
                    n->list.push_back(keyS);
                    n->list.push_back(valS);
                }
                return n;
            }
            (*err) = L"语法错误: 意外的 " + (t.t == TEnd ? wstring(L"文件尾")
                     : t.text) + L" @ 行 " + std::to_wstring(t.line);
            if (Peek().t != TEnd)
                p++;   // progress guarantee — never loop forever
            return New(Node::NullK);
        }
        PNode ParsePostfixFrom(PNode e) { return ParsePostfixCont(e); }
        PNode ParsePostfixCont(PNode e)
        {
            while (true)
            {
                if (IsPunct(L".") && (Peek(1).t == TIdent || Peek(1).t == TKw || (Peek(1).t == TPunct && Peek(1).text == L"$")))
                {
                    p++;
                    PNode m = New(Node::Member);
                    m->a = e;
                    // '$name' members span two tokens ('$' + ident)
                    if (Peek().t == TPunct && Peek().text == L"$" &&
                        Peek(1).t == TIdent)
                    {
                        p++;
                        m->str = L"$" + Next().text;
                    }
                    else
                        m->str = Next().text;
                    e = m;
                }
                else if (IsPunct(L"?."))
                {
                    p++;
                    PNode m = New(Node::Member);
                    m->bSafe = true;
                    m->a = e;
                    if (Peek().t == TPunct && Peek().text == L"$" &&
                        Peek(1).t == TIdent)
                    {
                        p++;
                        m->str = L"$" + Next().text;
                    }
                    else
                        m->str = Next().text;
                    e = m;
                }
                else if (IsPunct(L"?["))
                {
                    p++;
                    PNode m = New(Node::Index);
                    m->bSafe = true;
                    m->a = e;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"[") && Peek(1).t == TPunct &&
                         Peek(1).text == L"[")
                {
                    // aardio t[["k"]] — direct-key index, skips metatables
                    p += 2;
                    PNode m = New(Node::Index);
                    m->a = e;
                    m->bDirect = true;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"@") && Peek(1).t != TIdent)
                {
                    // aardio 'tab@' — metatable get/set (postfix @ not
                    // followed by an ident path; '@path' is the M2 form)
                    p++;
                    PNode m = New(Node::Member);
                    m->bMeta = true;
                    m->a = e;
                    m->str = L"_meta";
                    e = m;
                }
                else if (IsPunct(L"#"))
                {
                    // postfix # — aardio expr# = length (string chars /
                    // table array length)
                    p++;
                    PNode n = New(Node::Un);
                    n->str = L"#";
                    n->a = e;
                    e = ParsePostfixCont(n);
                }
                else if (IsPunct(L"["))
                {
                    p++;
                    PNode m = New(Node::Index);
                    m->a = e;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"("))
                {
                    p++;
                    PNode c = New(Node::Call);
                    c->a = e;
                    SkipNL();
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    if (!IsPunct(L")"))
                    {
                        // aardio f(,,,,1): leading empty slots
                        if (IsPunct(L",") )
                            c->list.push_back(New(Node::NullK));
                        else if (IsPunct(L")"))
                            c->list.push_back(New(Node::NullK));  // empty slot before )
                        else if (Peek().t == TIdent && Peek(1).t == TPunct && Peek(1).text == L"=")
                        {
                            // named args: f(name = expr; ...) — one table arg
                            PNode tbl = New(Node::TableCtor);
                            while (Peek().t == TIdent && Peek(1).t == TPunct &&
                                   Peek(1).text == L"=")
                            {
                                PNode key = New(Node::Str);
                                key->str = Next().text;
                                p++;
                                tbl->list.push_back(key);
                                tbl->list.push_back(ParseExpr());
                                SkipNL();
                                EatPunct(L";");
                                SkipNL();
                                if (IsPunct(L",")) { p++; SkipNL(); continue; }
                            }
                            c->list.push_back(tbl);
                        }
                        else
                            c->list.push_back(ParseExpr());
                        SkipNL();
                        while (EatPunct(L","))
                            {
                                SkipNL();
                                if (IsPunct(L",") || IsPunct(L")"))
                                    c->list.push_back(New(Node::NullK));
                                else if (IsPunct(L")"))
                                    c->list.push_back(New(Node::NullK));  // empty slot before )
                                else
                                    c->list.push_back(ParseExpr());
                                SkipNL();
                            }
                    }
                    bInAssignLhs = bOldLhs;
                    Expect(L")");
                    e = c;
                }
                else break;
            }
            return e;
        }
    };

    // ---------------- interpreter ----------------
    // ---- round 38: win.form — minimal windowing --------------------------
    //
    // A real Win32 window with button/static/edit children. Callbacks run
    // VM code re-entrantly inside doModal's message loop (Interp state is
    // saved/restored around each dispatch). Forms register in a global
    // HWND map; the WndProc dispatches WM_COMMAND to control tables.

}
