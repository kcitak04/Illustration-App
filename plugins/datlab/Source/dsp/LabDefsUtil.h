// Helpers for declaring processes in the registry (LabDefs.cpp and the CDP batches, LabCdp*.cpp).
#pragma once
#include "LabCore.h"

namespace lab
{
namespace defs
{
using O = std::vector<std::string>;
inline ParamDef P (std::string id, std::string label, double mn, double mx, double def, int flags = 0, std::string unit = {}, Fmt fmt = Fmt::num, O options = {})
{
    ParamDef d; d.id = std::move (id); d.label = std::move (label); d.min = mn; d.max = mx; d.def = def; d.flags = flags; d.unit = std::move (unit); d.fmt = fmt; d.options = std::move (options);
    if (! d.unit.empty() && d.fmt == Fmt::num) d.fmt = Fmt::unit;
    return d;
}
inline ParamDef Opt (std::string id, std::string label, O options, int def = 0)
{
    const double mx = (double) options.size() - 1;
    return P (std::move (id), std::move (label), 0, mx, def, PInt, {}, Fmt::num, std::move (options));
}
inline ParamDef Pct (std::string id, std::string label, double def) { return P (std::move (id), std::move (label), 0, 1, def, PPct, {}, Fmt::pct); }
inline ParamDef Seed() { return P ("seed", "SEED", 1, 99, 1, PInt); }
inline ParamDef Win (double mn, double mx, double def) { return P ("win", "WINDOW", mn, mx, def, PExp, {}, Fmt::time); }
inline const O DIVS { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T", "1/8D", "1/16D" };
inline const O SYNC_FREE = [] { O o { "FREE" }; o.insert (o.end(), DIVS.begin(), DIVS.end()); return o; }();

// Appends one process. cdp = the CDP program it follows ("distort repeat"); aux = label of the second sample it needs, if any.
inline void add (std::vector<ProcDef>& v, std::string cat, std::string id, std::string name, std::string tag, std::string desc,
                 std::vector<ParamDef> ps, ProcFn fn, std::string aux = {}, bool isNew = false, std::string cdp = {})
{
    ProcDef d; d.category = std::move (cat); d.id = std::move (id); d.name = std::move (name); d.tag = std::move (tag); d.desc = std::move (desc);
    d.params = std::move (ps); d.fn = fn; d.aux = std::move (aux); d.isNew = isNew; d.cdp = std::move (cdp); v.push_back (std::move (d));
}
} // namespace defs
} // namespace lab
