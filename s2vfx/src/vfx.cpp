#include "vfx.h"

#include <cctype>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace vfx
{
namespace
{
bool IsWord(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string Strip(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a]))
        a++;
    while (b > a && IsSpace(s[b - 1]))
        b--;
    return s.substr(a, b - a);
}

size_t SkipSpace(const std::string& t, size_t i)
{
    while (i < t.size() && IsSpace(t[i]))
        i++;
    return i;
}

size_t WordEnd(const std::string& t, size_t i)
{
    while (i < t.size() && IsWord(t[i]))
        i++;
    return i;
}

bool WordStart(const std::string& t, size_t i)
{
    return i < t.size() && IsWord(t[i]) && (i == 0 || !IsWord(t[i - 1]));
}

// From t[open] == '(' : the index of its closing ')', allowing one level of parentheses inside (as python/vfx.py's
// patterns do); npos if there is none.
size_t CloseParen(const std::string& t, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < t.size(); i++)
    {
        if (t[i] == '(')
        {
            if (++depth > 2)
                return std::string::npos;
        }
        else if (t[i] == ')')
        {
            if (--depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw VfxError("cannot open " + path.string());
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // As Python reads text: \r\n and \r become \n.
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '\r')
        {
            out += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n')
                i++;
        }
        else
            out += s[i];
    }
    return out;
}

std::string StripComments(const std::string& text)
{
    std::string a;
    for (size_t i = 0; i < text.size();)
    {
        if (text.compare(i, 2, "/*") == 0)
        {
            size_t e = text.find("*/", i + 2);
            if (e != std::string::npos)
            {
                for (size_t k = i; k < e + 2; k++)
                    if (text[k] == '\n')
                        a += '\n';
                i = e + 2;
                continue;
            }
        }
        a += text[i++];
    }
    std::string b;
    for (size_t i = 0; i < a.size();)
    {
        if (a.compare(i, 2, "//") == 0)
        {
            while (i < a.size() && a[i] != '\n')
                i++;
            continue;
        }
        b += a[i++];
    }
    return b;
}

std::string Expand(const std::filesystem::path& path, const std::vector<std::filesystem::path>& dirs,
                   std::set<std::string>& seen)
{
    std::string real = std::filesystem::weakly_canonical(path).string();
    if (seen.count(real))
        return "";
    seen.insert(real);
    std::string text = StripComments(ReadText(path));
    std::string out;
    size_t i = 0;
    while (i <= text.size())
    {
        size_t eol = text.find('\n', i);
        if (eol == std::string::npos)
            eol = text.size();
        std::string line = text.substr(i, eol - i);
        size_t p = 0;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
            p++;
        bool replaced = false;
        if (line.compare(p, 8, "#include") == 0)
        {
            size_t q = p + 8;
            size_t s = q;
            while (s < line.size() && IsSpace(line[s]))
                s++;
            if (s > q && s < line.size() && line[s] == '"')
            {
                size_t e = line.find('"', s + 1);
                if (e != std::string::npos && e > s + 1)
                {
                    std::string name = line.substr(s + 1, e - s - 1);
                    std::vector<std::filesystem::path> search{path.parent_path()};
                    search.insert(search.end(), dirs.begin(), dirs.end());
                    bool found = false;
                    for (const auto& d : search)
                    {
                        auto f = d / name;
                        if (std::filesystem::exists(f))
                        {
                            out += Expand(f, dirs, seen);
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        throw VfxError(path.string() + ": include " + name + " not found");
                    replaced = true;
                }
            }
        }
        if (!replaced)
            out += line;
        if (eol < text.size())
            out += '\n';
        i = eol + 1;
    }
    return out;
}

// {block name: body} for the top level blocks; a name that appears twice has its bodies joined.
std::map<std::string, std::string> Blocks(const std::string& text)
{
    static const char* NAMES[] = {"HEADER", "MODES", "FEATURES", "COMMON", "VS", "PS"};
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t start = std::string::npos, open = 0;
        std::string name;
        for (size_t i = pos; i < text.size() && start == std::string::npos; i++)
        {
            if (!WordStart(text, i))
                continue;
            size_t e = WordEnd(text, i);
            std::string w = text.substr(i, e - i);
            for (const char* n : NAMES)
            {
                if (w != n)
                    continue;
                size_t b = SkipSpace(text, e);
                if (b < text.size() && text[b] == '{')
                {
                    start = i;
                    open = b + 1;
                    name = w;
                }
            }
        }
        if (start == std::string::npos)
            break;
        int depth = 1;
        size_t i = open;
        while (depth && i < text.size())
        {
            if (text[i] == '{')
                depth++;
            else if (text[i] == '}')
                depth--;
            i++;
        }
        out[name] += text.substr(open, i - 1 - open) + "\n";
        pos = i;
    }
    return out;
}

// Replaces every "Name(args) ;" statement of the given names (as whole words) with what fn returns.
std::string ReplaceStatements(const std::string& t, const std::vector<std::string>& names,
                              const std::function<std::string(const std::string&, const std::string&)>& fn)
{
    std::string out;
    size_t i = 0;
    while (i < t.size())
    {
        if (WordStart(t, i))
        {
            size_t e = WordEnd(t, i);
            std::string w = t.substr(i, e - i);
            bool known = false;
            for (const auto& n : names)
                known = known || n == w;
            if (known)
            {
                size_t open = SkipSpace(t, e);
                if (open < t.size() && t[open] == '(')
                {
                    size_t close = CloseParen(t, open);
                    if (close != std::string::npos)
                    {
                        size_t semi = SkipSpace(t, close + 1);
                        if (semi < t.size() && t[semi] == ';')
                        {
                            out += fn(w, t.substr(open + 1, close - open - 1));
                            i = semi + 1;
                            continue;
                        }
                    }
                }
            }
            out += t.substr(i, e - i);
            i = e;
            continue;
        }
        out += t[i++];
    }
    return out;
}

// "type member : SEMANTIC < Semantic(Stream); >;"
std::string ReplaceMembers(const std::string& t, Stage& st)
{
    std::string out;
    size_t i = 0;
    while (i < t.size())
    {
        if (WordStart(t, i))
        {
            size_t e1 = WordEnd(t, i);
            size_t s2 = e1;
            bool ok = s2 < t.size() && IsSpace(t[s2]);
            s2 = SkipSpace(t, s2);
            size_t e2 = WordEnd(t, s2);
            ok = ok && e2 > s2;
            size_t colon = SkipSpace(t, e2);
            ok = ok && colon < t.size() && t[colon] == ':';
            size_t s3 = ok ? SkipSpace(t, colon + 1) : 0;
            size_t e3 = ok ? WordEnd(t, s3) : 0;
            ok = ok && e3 > s3;
            size_t lt = ok ? SkipSpace(t, e3) : 0;
            ok = ok && lt < t.size() && t[lt] == '<';
            size_t gt = ok ? t.find_first_of("<>", lt + 1) : 0;
            ok = ok && gt != std::string::npos && t[gt] == '>';
            size_t semi = ok ? SkipSpace(t, gt + 1) : 0;
            ok = ok && semi < t.size() && t[semi] == ';';
            if (ok)
            {
                std::string type = t.substr(i, e1 - i), member = t.substr(s2, e2 - s2), sem = t.substr(s3, e3 - s3);
                auto c = Calls(t.substr(lt + 1, gt - lt - 1));
                if (c.empty() || c[0].args.empty())
                    throw VfxError("vertex input " + member + ": no Semantic(...)");
                st.semantics[sem] = {member, c[0].args[0]};
                out += type + " " + member + " : " + sem + ";";
                i = semi + 1;
                continue;
            }
            out += t.substr(i, e1 - i);
            i = e1;
            continue;
        }
        out += t[i++];
    }
    return out;
}

bool AnnotatedType(const std::string& t, size_t i, size_t& end)
{
    static const char* TYPES[] = {"float", "int", "bool", "uint", "Texture2D", "Texture3D", "TextureCube",
                                  "SamplerState", "SamplerComparisonState"};
    for (const char* ty : TYPES)
    {
        std::string s = ty;
        if (t.compare(i, s.size(), s) != 0)
            continue;
        size_t e = i + s.size();
        // float[234]? and int[234]?
        if ((s == "float" || s == "int") && e < t.size() && t[e] >= '2' && t[e] <= '4' && e + 1 < t.size() &&
            IsSpace(t[e + 1]))
            e++;
        if (e < t.size() && IsSpace(t[e]))
        {
            end = e;
            return true;
        }
    }
    return false;
}

// "type name < annotations >;" for parameters, textures and samplers.
std::string ReplaceAnnotated(const std::string& t, Stage& st)
{
    std::string out;
    size_t i = 0;
    while (i < t.size())
    {
        size_t te;
        if (WordStart(t, i) && AnnotatedType(t, i, te))
        {
            size_t s2 = SkipSpace(t, te);
            size_t e2 = WordEnd(t, s2);
            size_t lt = SkipSpace(t, e2);
            bool ok = e2 > s2 && lt < t.size() && t[lt] == '<';
            size_t gt = 0;
            if (ok)
            {
                int depth = 0;
                ok = false;
                for (size_t k = lt + 1; k < t.size(); k++)
                {
                    if (t[k] == '<')
                    {
                        if (depth == 1)
                            break;
                        depth = 1;
                    }
                    else if (t[k] == '>')
                    {
                        if (depth == 0)
                        {
                            gt = k;
                            ok = true;
                            break;
                        }
                        depth = 0;
                    }
                }
            }
            size_t semi = ok ? SkipSpace(t, gt + 1) : 0;
            ok = ok && semi < t.size() && t[semi] == ';';
            if (ok)
            {
                Annotated a;
                a.type = t.substr(i, te - i);
                a.name = t.substr(s2, e2 - s2);
                a.annotations = Calls(t.substr(lt + 1, gt - lt - 1));
                if (a.type == "SamplerState" || a.type == "SamplerComparisonState")
                    st.samplers.push_back(a);
                else if (a.type.rfind("Texture", 0) == 0)
                    st.textures.push_back(a);
                else
                    st.params.push_back(a);
                out += a.type + " " + a.name + ";";
                i = semi + 1;
                continue;
            }
        }
        if (WordStart(t, i))
        {
            size_t e = WordEnd(t, i);
            out += t.substr(i, e - i);
            i = e;
            continue;
        }
        out += t[i++];
    }
    return out;
}

Stage ParseStage(std::string text)
{
    Stage st;
    text = ReplaceStatements(text, {"StaticCombo", "DynamicCombo"}, [&](const std::string& kind, const std::string& body) {
        auto args = SplitArgs(body);
        if (args.size() < 2)
            throw VfxError(kind + " takes a name and a range: " + body);
        if (kind == "StaticCombo")
        {
            StaticComboDecl c;
            c.name = args[0];
            const std::string& a = args[1];
            bool feature = a.size() > 2 && a.compare(0, 2, "F_") == 0;
            for (size_t k = 2; feature && k < a.size(); k++)
                feature = IsWord(a[k]);
            if (feature)
                c.feature = a;
            else
                std::tie(c.min, c.max) = ParseRange(a);
            for (size_t k = 2; k < args.size(); k++)
                for (const auto& call : Calls(args[k]))
                    if (call.name == "UiOnly")
                    {
                        if (call.args.size() != 1 || c.feature.empty())
                            throw VfxError("UiOnly(param) belongs to a static combo of a feature: " + body);
                        c.uiFlag = call.args[0];
                    }
            st.statics.push_back(c);
        }
        else
        {
            DynamicComboDecl c;
            c.name = args[0];
            std::tie(c.min, c.max) = ParseRange(args[1]);
            st.dynamics.push_back(c);
        }
        return std::string();
    });
    text = ReplaceStatements(text, {"CreateInputTexture2D"}, [&](const std::string&, const std::string& body) {
        auto a = SplitArgs(body);
        InputTexture tex;
        tex.name = a.at(0);
        if (a.size() > 1)
            tex.colorSpace = a[1];
        if (a.size() > 2)
            tex.bits = std::stoi(a[2]);
        if (a.size() > 3)
            tex.processing = Unquote(a[3]);
        if (a.size() > 4)
            tex.suffix = Unquote(a[4]);
        if (a.size() > 5)
            tex.group = Unquote(a[5]);
        std::string rest;
        for (size_t k = 6; k < a.size(); k++)
            rest += (k > 6 ? ", " : "") + a[k];
        for (const auto& c : Calls(rest))
            if (c.name.rfind("Default", 0) == 0)
                for (int k = 0; k < 4; k++)
                    tex.defaults[k] = k < static_cast<int>(c.args.size()) ? std::stof(c.args[k]) : 0.0f;
        st.inputs.push_back(tex);
        return std::string();
    });
    text = ReplaceStatements(text, {"RenderState"}, [&](const std::string&, const std::string& body) {
        auto a = SplitArgs(body);
        if (a.size() != 2)
            throw VfxError("RenderState takes a state and a value: RenderState(" + body + ");");
        st.renderStates.emplace_back(a[0], a[1]);
        return std::string();
    });
    text = ReplaceStatements(text, {"BoolAttribute", "IntAttribute", "FloatAttribute"},
                             [&](const std::string& kind, const std::string& body) {
        auto a = SplitArgs(body);
        if (a.size() != 2)
            throw VfxError(kind + " takes a name and a value: " + kind + "(" + body + ");");
        st.attributes.emplace_back(kind, a[0], a[1]);
        return std::string();
    });
    text = ReplaceMembers(text, st);
    text = ReplaceAnnotated(text, st);
    st.hlsl = text;
    return st;
}
} // namespace

std::vector<std::string> SplitArgs(const std::string& s)
{
    std::vector<std::string> out;
    int depth = 0;
    bool quoted = false;
    std::string cur;
    for (char ch : s)
    {
        if (ch == '"')
            quoted = !quoted;
        else if (quoted)
        {
        }
        else if (ch == '(' || ch == '<')
            depth++;
        else if (ch == ')' || ch == '>')
            depth--;
        if (ch == ',' && depth == 0 && !quoted)
        {
            out.push_back(Strip(cur));
            cur.clear();
        }
        else
            cur += ch;
    }
    if (!Strip(cur).empty())
        out.push_back(Strip(cur));
    return out;
}

std::vector<Call> Calls(const std::string& s)
{
    std::vector<Call> out;
    size_t i = 0;
    while (i < s.size())
    {
        if (WordStart(s, i))
        {
            size_t e = WordEnd(s, i);
            size_t open = SkipSpace(s, e);
            if (open < s.size() && s[open] == '(')
            {
                size_t close = CloseParen(s, open);
                if (close != std::string::npos)
                {
                    out.push_back({s.substr(i, e - i), SplitArgs(s.substr(open + 1, close - open - 1))});
                    i = close + 1;
                    continue;
                }
            }
            i = e;
            continue;
        }
        i++;
    }
    return out;
}

std::string Unquote(const std::string& s)
{
    std::string t = Strip(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        return t.substr(1, t.size() - 2);
    return t;
}

std::pair<int, int> ParseRange(const std::string& s)
{
    size_t i = SkipSpace(s, 0);
    auto number = [&](int& v) {
        size_t start = i;
        if (i < s.size() && s[i] == '-')
            i++;
        size_t digits = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9')
            i++;
        if (i == digits)
            return false;
        v = std::stoi(s.substr(start, i - start));
        return true;
    };
    int lo = 0, hi = 0;
    bool ok = number(lo);
    i = SkipSpace(s, i);
    ok = ok && s.compare(i, 2, "..") == 0;
    i += 2;
    i = SkipSpace(s, i);
    ok = ok && number(hi);
    if (!ok)
        throw VfxError("expected a range like 0..1, got " + s);
    return {lo, hi};
}

Shader Parse(const std::filesystem::path& path, const std::vector<std::filesystem::path>& includeDirs)
{
    std::set<std::string> seen;
    std::string text = Expand(path, includeDirs, seen);
    auto b = Blocks(text);
    for (const char* need : {"MODES", "VS", "PS"})
        if (!b.count(need))
            throw VfxError(path.string() + ": no " + need + " block");
    Shader shader;
    shader.name = path.stem().string();
    // HEADER: key = "value"; or key = value;
    const std::string& header = b["HEADER"];
    for (size_t i = 0; i < header.size();)
    {
        if (!IsWord(header[i]))
        {
            i++;
            continue;
        }
        size_t e = WordEnd(header, i);
        size_t eq = SkipSpace(header, e);
        if (eq < header.size() && header[eq] == '=')
        {
            size_t v = SkipSpace(header, eq + 1);
            size_t semi = header.find(';', v);
            if (semi != std::string::npos && semi > v)
            {
                if (header.substr(i, e - i) == "Description")
                    shader.description = Unquote(header.substr(v, semi - v));
                i = semi + 1;
                continue;
            }
        }
        i = e;
    }
    for (const auto& c : Calls(b["MODES"]))
    {
        ModeDecl m;
        m.name = c.name;
        for (const auto& a : c.args)
        {
            if (!a.empty() && a[0] == '"')
                m.fallback = Unquote(a);
            else if (!a.empty())
                m.combos.emplace_back(a, 1);
        }
        shader.modes.push_back(m);
    }
    for (const auto& c : Calls(b["FEATURES"]))
    {
        if (c.name != "Feature")
            continue;
        Feature f;
        f.name = c.args.at(0);
        std::tie(f.min, f.max) = ParseRange(c.args.at(1));
        // Feature(F_DETAIL_TEXTURE, 0..4 (0="None", 1="Mod2X", ...), "Group"): the names the Material Editor lists.
        std::map<int, std::string> names;
        const std::string& r = c.args[1];
        for (size_t q = r.find('"'); q != std::string::npos; q = r.find('"', q + 1))
        {
            if (q == 0)
                continue;
            size_t eq = r.find_last_not_of(" \t", q - 1);
            if (eq == std::string::npos || eq == 0 || r[eq] != '=')
                continue;
            size_t numEnd = r.find_last_not_of(" \t", eq - 1);
            size_t numStart = numEnd;
            while (numStart > 0 && (std::isdigit(static_cast<unsigned char>(r[numStart - 1])) || r[numStart - 1] == '-'))
                numStart--;
            size_t close = r.find('"', q + 1);
            if (numEnd == std::string::npos || close == std::string::npos ||
                !std::isdigit(static_cast<unsigned char>(r[numEnd])))
                continue;
            names[std::stoi(r.substr(numStart, numEnd - numStart + 1))] = r.substr(q + 1, close - q - 1);
            q = close;
        }
        if (!names.empty())
            for (int v = f.min; v <= f.max; v++)
                f.states.push_back(names.count(v) ? names[v] : std::to_string(v));
        if (c.args.size() > 2)
            f.group = Unquote(c.args[2]);
        shader.features.push_back(f);
    }
    std::string common = b["COMMON"];
    shader.stages["vs"] = ParseStage(common + "\n" + b["VS"]);
    shader.stages["ps"] = ParseStage(common + "\n" + b["PS"]);
    return shader;
}

} // namespace vfx
