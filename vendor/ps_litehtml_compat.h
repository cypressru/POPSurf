/* KOSWeb compatibility shim for building litehtml under kos-cc.
 *
 * Force-included ahead of every litehtml translation unit (-include).
 *
 * KOS's libstdc++ is built without the C99 stdio support that std::to_string's
 * floating-point overloads depend on, so only the six integral overloads are
 * declared. litehtml calls std::to_string(float) in ~35 places (font hashing
 * and property dumps), and each one resolves as ambiguous rather than as a
 * missing overload. Supplying the two missing overloads is the smallest fix
 * that keeps the call sites untouched, so the patch series stays rebasable
 * against upstream.
 *
 * Adding to namespace std is formally undefined, but it is the standard move
 * for embedded toolchain gaps and it is contained to this header. The plan's
 * no-double rule (§2.2) governs ps core, not vendored code; %g promotes to
 * double through varargs regardless, and these paths are string formatting,
 * never layout math.
 */
#ifndef PS_LITEHTML_COMPAT_H
#define PS_LITEHTML_COMPAT_H

#include <string>
#include <cstdio>

namespace std {

inline std::string to_string(float v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%g", (double)v);
    return std::string(buf);
}

inline std::string to_string(double v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%g", v);
    return std::string(buf);
}

} /* namespace std */

#endif /* PS_LITEHTML_COMPAT_H */
