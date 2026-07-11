/**
 Copyright (c) 2026-present, Stanisław Pitucha
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree.
 */

#include <xcdriver/DumpPIFAction.h>
#include <xcdriver/Action.h>
#include <xcdriver/Options.h>
#include <pbxproj/PBX/AggregateTarget.h>
#include <pbxproj/PBX/CopyFilesBuildPhase.h>
#include <pbxproj/PBX/FileReference.h>
#include <pbxproj/PBX/FrameworksBuildPhase.h>
#include <pbxproj/PBX/Group.h>
#include <pbxproj/PBX/HeadersBuildPhase.h>
#include <pbxproj/PBX/LegacyTarget.h>
#include <pbxproj/PBX/NativeTarget.h>
#include <pbxproj/PBX/Project.h>
#include <pbxproj/PBX/ResourcesBuildPhase.h>
#include <pbxproj/PBX/ShellScriptBuildPhase.h>
#include <pbxproj/PBX/SourcesBuildPhase.h>
#include <pbxproj/PBX/SwiftPackageProductDependency.h>
#include <pbxproj/PBX/Target.h>
#include <pbxproj/PBX/TargetDependency.h>
#include <pbxproj/PBX/VariantGroup.h>
#include <pbxproj/XC/BuildConfiguration.h>
#include <pbxproj/XC/ConfigurationList.h>
#include <pbxproj/XC/VersionGroup.h>
#include <pbxbuild/WorkspaceContext.h>
#include <xcworkspace/XC/Workspace.h>
#include <pbxsetting/Environment.h>
#include <pbxsetting/Setting.h>
#include <pbxsetting/Type.h>
#include <pbxsetting/Value.h>
#include <pbxsetting/XC/Config.h>
#include <plist/Format/Any.h>
#include <plist/Format/JSON.h>
#include <plist/Array.h>
#include <plist/Boolean.h>
#include <plist/Dictionary.h>
#include <plist/Integer.h>
#include <plist/Real.h>
#include <plist/String.h>
#include <libutil/Filesystem.h>
#include <libutil/FSUtil.h>
#include <libutil/md5.h>
#include <process/Context.h>
#include <process/User.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using xcdriver::DumpPIFAction;
using xcdriver::Options;
using libutil::Filesystem;

DumpPIFAction::
DumpPIFAction()
{
}

DumpPIFAction::
~DumpPIFAction()
{
}

namespace {

/*
 * We're using an ad-hoc implementation of a JSON serialiser here. Normally
 * this would be a really bad idea, but in this case:
 * - we're not dealing with adverserial data
 * - we need encoding only
 * - we want minimal dependencies
 * - there's no reason for more advanced features like streaming
 */

class JSONOut {
public:
    std::string out;
    int indent = 0;

    void writeIndent() { out.append(indent * 2, ' '); }

    void writeEscaped(std::string const &s) {
        out += '"';
        for (char c : s) {
            unsigned char uc = (unsigned char)c;
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (uc < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", uc);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        out += '"';
    }
};

/* Forward declarations of any JSON value type. */
struct JValue;
using JArray = std::vector<JValue>;
using JObject = std::map<std::string, JValue>;

struct JValue {
    enum class K { Null, Bool, Int, Str, Arr, Obj };
    K kind = K::Null;
    bool b = false;
    long long i = 0;
    std::string s;
    JArray a;
    JObject o;

    JValue() = default;
    JValue(bool v) : kind(K::Bool), b(v) {}
    JValue(int v) : kind(K::Int), i(v) {}
    JValue(long long v) : kind(K::Int), i(v) {}
    JValue(unsigned int v) : kind(K::Int), i(v) {}
    JValue(char const *v) : kind(K::Str), s(v) {}
    JValue(std::string const &v) : kind(K::Str), s(v) {}
    JValue(JArray const &v) : kind(K::Arr), a(v) {}
    JValue(JObject const &v) : kind(K::Obj), o(v) {}

    static JValue Str(std::string const &v) { JValue x; x.kind = K::Str; x.s = v; return x; }
    static JValue Arr(JArray v) { JValue x; x.kind = K::Arr; x.a = std::move(v); return x; }
    static JValue Obj(JObject v) { JValue x; x.kind = K::Obj; x.o = std::move(v); return x; }
};

void emit(JSONOut &j, JValue const &v);

void emitArray(JSONOut &j, JArray const &a) {
    if (a.empty()) {
        j.out += "[\n\n";
        j.writeIndent();
        j.out += "]";
        return;
    }
    j.out += "[\n";
    j.indent++;
    for (size_t i = 0; i < a.size(); i++) {
        j.writeIndent();
        emit(j, a[i]);
        if (i + 1 < a.size()) j.out += ",";
        j.out += "\n";
    }
    j.indent--;
    j.writeIndent();
    j.out += "]";
}

void emitObject(JSONOut &j, JObject const &o) {
    if (o.empty()) {
        j.out += "{\n\n";
        j.writeIndent();
        j.out += "}";
        return;
    }
    j.out += "{\n";
    j.indent++;
    size_t n = o.size(), i = 0;
    for (auto const &kv : o) {
        j.writeIndent();
        j.writeEscaped(kv.first);
        j.out += " : ";
        emit(j, kv.second);
        if (++i < n) j.out += ",";
        j.out += "\n";
    }
    j.indent--;
    j.writeIndent();
    j.out += "}";
}

void emit(JSONOut &j, JValue const &v) {
    switch (v.kind) {
        case JValue::K::Null: j.out += "null"; break;
        case JValue::K::Bool: j.out += (v.b ? "true" : "false"); break;
        case JValue::K::Int: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", v.i);
            j.out += buf;
            break;
        }
        case JValue::K::Str: j.writeEscaped(v.s); break;
        case JValue::K::Arr: emitArray(j, v.a); break;
        case JValue::K::Obj: emitObject(j, v.o); break;
    }
}

/*
 * MD5 of arbitrary bytes, returned as lowercase 32-char hex. The host
 * xcodebuild's PIF identifiers are all MD5 hashes rendered via
 * `-[NSData dvt_lowercaseHexString]`; using MD5 throughout lets our object
 * GUIDs and signatures line up with the host's, byte-for-byte where the
 * inputs are reproducible.
 */
std::string md5Hex(uint8_t const *data, size_t n) {
    md5_state_t st;
    md5_init(&st);
    md5_append(&st, (md5_byte_t const *)data, (int)n);
    md5_byte_t digest[16];
    md5_finish(&st, digest);
    char buf[33];
    for (int i = 0; i < 16; i++) snprintf(buf + i * 2, 3, "%02x", digest[i]);
    return std::string(buf);
}

std::string md5Hex(std::string const &s) {
    return md5Hex((uint8_t const *)s.data(), s.size());
}

std::string shellSingleQuote(std::string const &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

/* Recursively convert a parsed plist value tree into our JValue tree, so PIF
 * fragments SwiftPM emits can be re-serialized through the same writer. */
JValue plistToJValue(plist::Object const *o) {
    if (o == nullptr) return JValue();
    if (auto d = plist::CastTo<plist::Dictionary>(o)) {
        JObject obj;
        for (size_t i = 0; i < d->count(); i++) {
            std::string k = d->key(i);
            obj[k] = plistToJValue(d->value(k));
        }
        return JValue::Obj(std::move(obj));
    }
    if (auto a = plist::CastTo<plist::Array>(o)) {
        JArray arr;
        for (size_t i = 0; i < a->count(); i++) arr.push_back(plistToJValue(a->value(i)));
        return JValue::Arr(std::move(arr));
    }
    if (auto s = plist::CastTo<plist::String>(o)) return JValue::Str(s->value());
    if (auto b = plist::CastTo<plist::Boolean>(o)) return JValue(b->value());
    if (auto n = plist::CastTo<plist::Integer>(o)) return JValue((long long)n->value());
    if (auto r = plist::CastTo<plist::Real>(o)) return JValue((long long)r->value());
    return JValue();
}

/* Drop the redundant `name` (a copy of the absolute path) that SwiftPM emits on
 * file nodes of a package's group tree; the host omits it. */
void stripPackageFileNodeNames(JValue &node) {
    if (node.kind != JValue::K::Obj) return;
    auto ty = node.o.find("type");
    if (ty != node.o.end() && ty->second.kind == JValue::K::Str &&
        (ty->second.s == "file" || ty->second.s == "fileReference")) {
        node.o.erase("name");
    }
    auto ch = node.o.find("children");
    if (ch != node.o.end() && ch->second.kind == JValue::K::Arr) {
        for (auto &c : ch->second.a) stripPackageFileNodeNames(c);
    }
}

struct SplicedPackageObject {
    bool isProject;
    std::string guid;
    std::string signature; /* SwiftPM's own signature, kept so a project's
                            * `targets` list keeps matching its target objects. */
    JValue contents;
};

/* The three-setting build configurations of a host `packageProduct` target. */
JArray packageProductBuildConfigurations(std::string const &guid) {
    JArray configs;
    char const *names[2] = {"Debug", "Release"};
    for (int i = 0; i < 2; i++) {
        JObject c;
        c["guid"] = guid + "::BUILDCONFIG_" + std::to_string(i);
        c["name"] = std::string(names[i]);
        JObject s;
        s["SDK_VARIANT"] = std::string("auto");
        s["SDKROOT"] = std::string("auto");
        s["USES_SWIFTPM_UNSAFE_FLAGS"] = std::string("NO");
        c["buildSettings"] = s;
        JObject imp;
        imp["buildSettings"] = JObject{};
        c["impartedBuildProperties"] = imp;
        configs.push_back(c);
    }
    return configs;
}

/*
 * Convert SwiftPM's standalone product target into the host's app-embedded form.
 * Standalone `dump-pif` builds a library product as an actual static/dynamic
 * library named `<product>-product`; when a package is a *dependency*, the host
 * instead emits a linkable `packageProduct` (static/automatic) or a `framework`
 * target (dynamic) named exactly `<product>`. swift-build resolves a target's
 * package-product dependency by that name, so the `-product` suffix and wrong
 * type make it report "Missing package product". SwiftPM's dependencies and
 * frameworks phase already match the host after the GUID remap, so we reuse
 * them and only fix the envelope: for static products the whole shape becomes a
 * packageProduct; for dynamic products the product type becomes a framework.
 */
JValue toEmbeddedPackageProduct(JValue prod) {
    if (prod.kind != JValue::K::Obj) return prod;
    auto gi = prod.o.find("guid");
    if (gi == prod.o.end() || gi->second.kind != JValue::K::Str) return prod;
    std::string guid = gi->second.s;
    std::string name = guid.substr(std::string("PACKAGE-PRODUCT:").size());

    std::string pt;
    auto pti = prod.o.find("productTypeIdentifier");
    if (pti != prod.o.end() && pti->second.kind == JValue::K::Str) pt = pti->second.s;

    if (pt.find("library.static") != std::string::npos || pt.find("library.automatic") != std::string::npos) {
        JObject o;
        o["guid"] = guid;
        o["name"] = name;
        o["type"] = std::string("packageProduct");
        o["approvedByUser"] = std::string("true");
        o["customTasks"] = JArray{};
        auto deps = prod.o.find("dependencies");
        o["dependencies"] = deps != prod.o.end() ? deps->second : JValue::Arr({});
        o["buildConfigurations"] = packageProductBuildConfigurations(guid);
        /* Lift the frameworks phase out of buildPhases into frameworksBuildPhase. */
        auto bps = prod.o.find("buildPhases");
        if (bps != prod.o.end() && bps->second.kind == JValue::K::Arr) {
            for (auto &ph : bps->second.a) {
                if (ph.kind == JValue::K::Obj) {
                    auto ty = ph.o.find("type");
                    if (ty != ph.o.end() && ty->second.kind == JValue::K::Str &&
                        ty->second.s.find("frameworks") != std::string::npos) {
                        o["frameworksBuildPhase"] = ph;
                        break;
                    }
                }
            }
        }
        return JValue::Obj(std::move(o));
    }

    if (pt.find("library.dynamic") != std::string::npos) {
        prod.o["name"] = JValue::Str(name);
        prod.o["productTypeIdentifier"] = JValue::Str("com.apple.product-type.framework");
        return prod;
    }

    prod.o["name"] = JValue::Str(name);
    return prod;
}

/*
 * Obtain a Swift package's PIF directly from SwiftPM
 * (`swift package --build-system swiftbuild dump-pif`) and adapt it to the
 * host's app-embedded scheme. SwiftPM is the source of truth for the package's
 * group tree, build phases, and build settings, so we splice its output rather
 * than reproduce the synthesis. Adaptation:
 *   - remap the project GUID from SwiftPM's `PACKAGE:<identity>` to the host's
 *     `PACKAGE:<absolute-dir>`, and each product GUID
 *     `PACKAGE-PRODUCT:<identity>_<module>.<product>` to `PACKAGE-PRODUCT:<product>`,
 *     rewriting every reference (done as text replacement before re-parsing);
 *   - drop SwiftPM's standalone-only objects (its Workspace, the AGGREGATE
 *     project, and the ALL-*-TESTS targets);
 *   - strip the redundant file-node names from the group tree.
 * Returns nullopt if the toolchain is unavailable, so callers degrade to no
 * package projects rather than failing.
 */
ext::optional<std::vector<SplicedPackageObject>> loadPackagePIF(std::string const &dir) {
    /* Capture the command's stderr to a temp file so that, when it fails, we can
     * report *why* (missing toolchain, manifest error, ...) instead of silently
     * dropping the package — which downstream surfaces only as swift-build's
     * opaque "Missing package product". */
    char errTemplate[] = "/tmp/xcbuild-swiftpm-XXXXXX";
    int errFd = mkstemp(errTemplate);
    std::string errPath = errFd >= 0 ? errTemplate : "/dev/null";
    if (errFd >= 0) close(errFd);

    std::string cmd = "swift package --package-path " + shellSingleQuote(dir) +
                      " --build-system swiftbuild dump-pif 2>" + shellSingleQuote(errPath);
    FILE *pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        if (errFd >= 0) unlink(errPath.c_str());
        return ext::nullopt;
    }
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);
    int rc = pclose(pipe);
    if (rc != 0 || out.empty()) {
        std::string err;
        if (errFd >= 0) {
            if (FILE *ef = fopen(errPath.c_str(), "r")) {
                char eb[4096];
                size_t en;
                while ((en = fread(eb, 1, sizeof(eb), ef)) > 0) err.append(eb, en);
                fclose(ef);
            }
            unlink(errPath.c_str());
        }
        fprintf(stderr, "warning: `swift package ... dump-pif` failed for '%s' (exit %d); its package will be omitted from the PIF.\n%s\n",
                dir.c_str(), rc, err.c_str());
        return ext::nullopt;
    }
    if (errFd >= 0) unlink(errPath.c_str());

    /* First parse: discover the GUIDs that need remapping. */
    std::vector<uint8_t> bytes(out.begin(), out.end());
    auto parsed = plist::Format::JSON::Deserialize(bytes, plist::Format::JSON::Create());
    auto arr = plist::CastTo<plist::Array>(parsed.first.get());
    if (arr == nullptr) return ext::nullopt;

    std::vector<std::pair<std::string, std::string>> remaps; /* from -> to */
    for (size_t i = 0; i < arr->count(); i++) {
        auto o = arr->value<plist::Dictionary>(i);
        if (o == nullptr) continue;
        auto type = o->value<plist::String>("type");
        auto contents = o->value<plist::Dictionary>("contents");
        if (type == nullptr || contents == nullptr) continue;
        auto guidS = contents->value<plist::String>("guid");
        if (guidS == nullptr) continue;
        std::string guid = guidS->value();
        if (type->value() == "project" && guid.rfind("PACKAGE:", 0) == 0) {
            remaps.emplace_back(guid, "PACKAGE:" + dir);
        } else if (type->value() == "target" && guid.rfind("PACKAGE-PRODUCT:", 0) == 0) {
            /* SwiftPM's product GUID is `PACKAGE-PRODUCT:<identity>_<module>.<product>`
             * and its name is `<product>-product`; the host uses just
             * `PACKAGE-PRODUCT:<product>`. Recover the product name from the guid
             * (after the final dot), matching how the consuming app references it. */
            auto dot = guid.rfind('.');
            std::string product = (dot == std::string::npos)
                ? guid.substr(std::string("PACKAGE-PRODUCT:").size())
                : guid.substr(dot + 1);
            remaps.emplace_back(guid, "PACKAGE-PRODUCT:" + product);
        }
    }
    if (remaps.empty()) return ext::nullopt;
    /* Replace longer GUIDs first so no remap key is a prefix of another. */
    std::sort(remaps.begin(), remaps.end(), [](std::pair<std::string, std::string> const &a, std::pair<std::string, std::string> const &b) {
        return a.first.size() > b.first.size();
    });
    for (auto const &r : remaps) {
        size_t pos = 0;
        while ((pos = out.find(r.first, pos)) != std::string::npos) {
            out.replace(pos, r.first.size(), r.second);
            pos += r.second.size();
        }
    }

    /* Second parse: the remapped PIF. */
    std::vector<uint8_t> bytes2(out.begin(), out.end());
    auto parsed2 = plist::Format::JSON::Deserialize(bytes2, plist::Format::JSON::Create());
    auto arr2 = plist::CastTo<plist::Array>(parsed2.first.get());
    if (arr2 == nullptr) return ext::nullopt;

    std::vector<SplicedPackageObject> result;
    for (size_t i = 0; i < arr2->count(); i++) {
        auto o = arr2->value<plist::Dictionary>(i);
        if (o == nullptr) continue;
        auto type = o->value<plist::String>("type");
        auto contents = o->value<plist::Dictionary>("contents");
        if (type == nullptr || contents == nullptr) continue;
        auto guidS = contents->value<plist::String>("guid");
        if (guidS == nullptr) continue;
        std::string guid = guidS->value();
        std::string t = type->value();

        if (t == "workspace") continue;
        if (guid == "AGGREGATE" || guid == "ALL-INCLUDING-TESTS" || guid == "ALL-EXCLUDING-TESTS") continue;
        bool isProject = (t == "project");
        if (!isProject && t != "target") continue;

        /* Keep SwiftPM's signature: a project's `targets` array lists its
         * targets by signature, so the emitted target objects must carry those
         * same signatures or swift-build won't associate them with the project
         * (and the package products won't be found). */
        std::string signature;
        if (auto sigS = o->value<plist::String>("signature")) signature = sigS->value();

        JValue jc = plistToJValue(contents);
        if (isProject) {
            auto gt = jc.o.find("groupTree");
            if (gt != jc.o.end()) stripPackageFileNodeNames(gt->second);
        } else if (guid.rfind("PACKAGE-PRODUCT:", 0) == 0) {
            jc = toEmbeddedPackageProduct(std::move(jc));
        }
        result.push_back({isProject, guid, signature, std::move(jc)});
    }
    return result;
}

/*
 * Per-dump signing context: maps each project to its 32-char PIF hash
 * (= MD5 of the project's path relative to the workspace's base dir).
 * Object GUIDs within a project use this prefix, matching the host's scheme
 * `<projectPifGuid><MD5(blueprintIdentifier)>`.
 */
struct PIFContext {
    std::unordered_map<pbxproj::PBX::Project const *, std::string> projectHash;
};

std::string projectGUID(PIFContext const &ctx, pbxproj::PBX::Project const &project) {
    auto it = ctx.projectHash.find(&project);
    if (it != ctx.projectHash.end()) return it->second;
    return std::string(32, '0');
}

std::string objectGUID(PIFContext const &ctx, pbxproj::PBX::Project const &project, pbxproj::PBX::Object const &obj) {
    std::string id = obj.blueprintIdentifier();
    std::string suffix = id.empty() ? std::string(32, '0') : md5Hex(id);
    return projectGUID(ctx, project) + suffix;
}

/*
 * Hardcoded plugin signature observed in every host-generated PIF: this is
 * the base-36 digest of the default empty plugin-result set, which is stable
 * across projects. We don't load plugins, so emitting the same constant keeps
 * the signature shape interchangeable with the host's output.
 */
constexpr char const *kDefaultPluginsSignature = "1OJSG6M1FOV3XYQCBH7Z29RZ0FPR9XDE1";

std::string projectSignature(PIFContext const &ctx, pbxproj::PBX::Project const &project, std::string const &modHash) {
    /* Format mirrors the host's: PROJECT@v%d_mod=%@_hash=%@plugins=%@
     * - mod: MD5 of the project's pbxproj contents (host uses the
     *   NSJSONSerialization of pifRepresentation; we can't reproduce that
     *   exactly, so we substitute a stable proxy that still changes when
     *   the project changes).
     * - hash: MD5 of relative project path (matches the host exactly).
     * - plugins: empty plugin set digest (constant). */
    return "PROJECT@v11_mod=" + modHash + "_hash=" + projectGUID(ctx, project) +
           "plugins=" + std::string(kDefaultPluginsSignature);
}

std::string targetSignature(PIFContext const &ctx, pbxproj::PBX::Project const &project, pbxproj::PBX::Target const &target) {
    /* Host computes MD5 of NSJSONSerialization output of the target's
     * pifRepresentation; we approximate with MD5(blueprintIdentifier) so the
     * signature is stable and structurally identical (32 hex chars). */
    std::string id = target.blueprintIdentifier();
    std::string h = id.empty() ? std::string(32, '0') : md5Hex(id);
    return "TARGET@v11_hash=" + h;
}

std::string workspaceGUID(std::string const &workspaceName) {
    return md5Hex(workspaceName);
}

/*
 * Reads `<workspacePath>/contents.xcworkspacedata` if present; otherwise
 * returns the canonical self-referencing XML Xcode would synthesize for a
 * bare project's `project.xcworkspace`. The host hashes this byte stream
 * along with the absolute file path to produce the workspace contents hash.
 */
std::vector<uint8_t> readWorkspaceContentsData(Filesystem *filesystem, std::string const &contentsPath) {
    std::vector<uint8_t> bytes;
    if (filesystem->isReadable(contentsPath) && filesystem->read(&bytes, contentsPath)) {
        return bytes;
    }
    static char const synthetic[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Workspace\n"
        "   version = \"1.0\">\n"
        "   <FileRef\n"
        "      location = \"self:\">\n"
        "   </FileRef>\n"
        "</Workspace>\n";
    return std::vector<uint8_t>(synthetic, synthetic + sizeof(synthetic) - 1);
}

std::string workspaceSignature(Filesystem *filesystem,
                               std::string const &workspacePath,
                               std::vector<std::string> const &projectSignatures) {
    std::string contentsPath = workspacePath + "/contents.xcworkspacedata";
    std::vector<uint8_t> data = readWorkspaceContentsData(filesystem, contentsPath);

    /* hash = MD5(file bytes ++ absolute contents path). */
    std::vector<uint8_t> hashInput = data;
    hashInput.insert(hashInput.end(), contentsPath.begin(), contentsPath.end());
    std::string contentsHash = md5Hex(hashInput.data(), hashInput.size());

    /* subobjects = MD5(concatenation of subobject signatures, no separator). */
    std::string concat;
    for (auto const &s : projectSignatures) concat += s;
    std::string subobjectsHash = md5Hex(concat);

    return "WORKSPACE@v11_hash=" + contentsHash + "_subobjects=" + subobjectsHash;
}

/*
 * Infer a file type identifier from a file's path extension. Mirrors the
 * fallback Xcode uses when a PBXFileReference has neither lastKnownFileType
 * nor explicitFileType. Unknown extensions resolve to "file".
 */
std::string inferFileType(std::string const &path) {
    auto slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    auto dot = base.rfind('.');
    if (dot == std::string::npos) return "file";
    std::string ext = base.substr(dot + 1);
    if (ext == "h")           return "sourcecode.c.h";
    if (ext == "c")           return "sourcecode.c.c";
    if (ext == "m")           return "sourcecode.c.objc";
    if (ext == "mm")          return "sourcecode.cpp.objcpp";
    if (ext == "cpp" || ext == "cc" || ext == "cxx") return "sourcecode.cpp.cpp";
    if (ext == "hpp" || ext == "hh" || ext == "hxx") return "sourcecode.cpp.h";
    if (ext == "swift")       return "sourcecode.swift";
    if (ext == "plist")       return "text.plist.xml";
    if (ext == "entitlements") return "text.plist.entitlements";
    if (ext == "xib")         return "file.xib";
    if (ext == "storyboard")  return "file.storyboard";
    if (ext == "json")        return "text.json";
    if (ext == "xml")         return "text.xml";
    if (ext == "txt")         return "text";
    if (ext == "md")          return "net.daringfireball.markdown";
    if (ext == "sh")          return "text.script.sh";
    if (ext == "py")          return "text.script.python";
    if (ext == "a")           return "archive.ar";
    if (ext == "dylib")       return "compiled.mach-o.dylib";
    if (ext == "framework")   return "wrapper.framework";
    if (ext == "app")         return "wrapper.application";
    if (ext == "xctest")      return "wrapper.cfbundle";
    if (ext == "xcconfig")    return "text.xcconfig";
    if (ext == "css")         return "text.css";
    if (ext == "html")        return "text.html";
    if (ext == "rtf")         return "text.rtf";
    return "file";
}

/* Map a CopyFilesBuildPhase destination enum to the build setting macro Xcode emits. */
std::string copyFilesDestination(uint32_t spec) {
    using D = pbxproj::PBX::CopyFilesBuildPhase;
    switch (spec) {
        case D::kDestinationAbsolute:         return "";
        case D::kDestinationWrapper:          return "$(WRAPPER_NAME)";
        case D::kDestinationExecutables:      return "$(EXECUTABLE_FOLDER_PATH)";
        case D::kDestinationResources:        return "$(UNLOCALIZED_RESOURCES_FOLDER_PATH)";
        case D::kDestinationPublicHeaders:    return "$(PUBLIC_HEADERS_FOLDER_PATH)";
        case D::kDestinationPrivateHeaders:   return "$(PRIVATE_HEADERS_FOLDER_PATH)";
        case D::kDestinationFrameworks:       return "$(FRAMEWORKS_FOLDER_PATH)";
        case D::kDestinationSharedFrameworks: return "$(SHARED_FRAMEWORKS_FOLDER_PATH)";
        case D::kDestinationSharedSupport:    return "$(SHARED_SUPPORT_FOLDER_PATH)";
        case D::kDestinationPlugIns:          return "$(PLUGINS_FOLDER_PATH)";
        case D::kDestinationJavaResources:    return "$(JAVA_FOLDER_PATH)";
        case D::kDestinationProducts:         return "$(BUILT_PRODUCTS_DIR)";
        default:                              return "";
    }
}

JObject emitBuildConfiguration(PIFContext const &ctx, pbxproj::PBX::Project const &project, pbxproj::XC::BuildConfiguration const &config) {
    JObject obj;
    obj["guid"] = objectGUID(ctx, project, config);
    obj["name"] = config.name();

    /* Emit build settings verbatim — original key (with any `[condition]`
     * suffix) and value exactly as written. Going through the parsed
     * pbxsetting::Value would normalize ${X}/$X reference syntax to $(X),
     * diverging from the host's byte-for-byte output. */
    JObject settings;
    for (auto const &kv : config.buildSettingsRaw()) {
        settings[kv.first] = kv.second;
    }
    obj["buildSettings"] = settings;

    if (config.baseConfigurationReference() != nullptr) {
        obj["baseConfigurationFileReference"] = objectGUID(ctx, project, *config.baseConfigurationReference());
    }
    return obj;
}

JArray emitBuildConfigurations(PIFContext const &ctx, pbxproj::PBX::Project const &project, pbxproj::XC::ConfigurationList const &list) {
    JArray arr;
    for (auto const &cfg : list.buildConfigurations()) {
        arr.push_back(emitBuildConfiguration(ctx, project, *cfg));
    }
    return arr;
}

/*
 * Build a map from a FileReference's GUID (within this project) to the GUID
 * of the target whose productReference points at it. Used to translate
 * buildFile.fileReference into a buildFile.targetReference when the
 * referenced file is another target's product, since the file reference
 * itself isn't emitted as a top-level entry.
 */
std::unordered_map<std::string, std::string>
productFileToTarget(PIFContext const &ctx, pbxproj::PBX::Project const &project) {
    std::unordered_map<std::string, std::string> m;
    for (auto const &t : project.targets()) {
        if (t->type() != pbxproj::PBX::Target::Type::Native) continue;
        auto const &nt = static_cast<pbxproj::PBX::NativeTarget const &>(*t);
        if (nt.productReference()) {
            /* The target reference must be the target's full object GUID
             * (`<projectGuid><MD5(blueprintId)>`), the same 64-char form the
             * target is registered under. Emitting only the bare 32-char
             * `MD5(blueprintId)` suffix here — dropping the project prefix —
             * yields a targetReference the PIF loader can't resolve, which is
             * how an implicit intra-project product link (a Frameworks-phase
             * link to another target's .a/.framework with no explicit
             * PBXTargetDependency) fails to build. */
            m[objectGUID(ctx, project, *nt.productReference())] =
                objectGUID(ctx, project, *t);
        }
    }
    return m;
}

JArray emitBuildFiles(PIFContext const &ctx, pbxproj::PBX::Project const &project, pbxproj::PBX::BuildPhase const &phase, std::unordered_map<std::string, std::string> const &prodToTarget) {
    JArray arr;
    for (auto const &bf : phase.files()) {
        /* A build file that links a Swift package product carries a productRef
         * (XCSwiftPackageProductDependency) instead of a fileRef. Emit it with
         * a `PACKAGE-PRODUCT:<name>` target reference, matching the host. Its
         * guid is the bare MD5 of the package product dependency's identifier —
         * no project-signature prefix — so the same product linked from several
         * phases (e.g. Frameworks and Embed Frameworks) shares one guid. */
        if (bf->productRef() != nullptr) {
            JObject o;
            std::string id = bf->productRef()->blueprintIdentifier();
            o["guid"] = id.empty() ? std::string(32, '0') : md5Hex(id);
            o["targetReference"] = "PACKAGE-PRODUCT:" + bf->productRef()->productName();
            arr.push_back(o);
            continue;
        }

        /* Skip orphaned build files with no file or target reference — pbxproj
         * leaves these around as `(null)` entries when the underlying file
         * reference was deleted. The PIF loader rejects BuildFile dictionaries
         * lacking a fileReference/targetReference. */
        if (bf->fileRef() == nullptr) continue;

        JObject o;
        o["guid"] = objectGUID(ctx, project, *bf);

        /* If the referenced file is another target's product, emit a
         * targetReference instead — the product file itself is not declared
         * as a top-level entry in the PIF. */
        std::string frGuid = objectGUID(ctx, project, *bf->fileRef());
        auto it = prodToTarget.find(frGuid);
        if (it != prodToTarget.end()) {
            o["targetReference"] = it->second;
        } else {
            o["fileReference"] = frGuid;
        }

        /* Translate per-file attributes (CodeSignOnCopy, header visibility) to
         * the keys the host emits on each buildFile entry. */
        for (auto const &attr : bf->attributes()) {
            if (attr == "CodeSignOnCopy") {
                o["codeSignOnCopy"] = std::string("true");
            } else if (attr == "Public") {
                o["headerVisibility"] = std::string("public");
            } else if (attr == "Private") {
                o["headerVisibility"] = std::string("private");
            }
        }

        /* .intentdefinition sources default to public codegen visibility, like
         * the host. Also emit intentsCodegenFiles: "true" — the legacy boolean
         * key that swift-build still honors as a fallback. */
        if (bf->fileRef()->type() == pbxproj::PBX::GroupItem::Type::FileReference) {
            auto const &fr = static_cast<pbxproj::PBX::FileReference const &>(*bf->fileRef());
            std::string ft = fr.lastKnownFileType();
            if (ft.empty()) ft = fr.explicitFileType();
            if (ft == "file.intentdefinition") {
                o["intentsCodegenVisibility"] = std::string("public");
                o["intentsCodegenFiles"] = std::string("true");
            }
        }

        if (!bf->compilerFlags().empty()) {
            std::string joined;
            for (size_t i = 0; i < bf->compilerFlags().size(); i++) {
                if (i) joined += " ";
                joined += bf->compilerFlags()[i];
            }
            o["additionalCompilerOptions"] = joined;
        }
        arr.push_back(o);
    }
    return arr;
}

JObject emitBuildPhase(PIFContext const &ctx, pbxproj::PBX::Project const &project, pbxproj::PBX::BuildPhase const &phase, std::unordered_map<std::string, std::string> const &prodToTarget) {
    JObject o;
    o["guid"] = objectGUID(ctx, project, phase);
    o["buildFiles"] = emitBuildFiles(ctx, project, phase, prodToTarget);

    using T = pbxproj::PBX::BuildPhase::Type;
    switch (phase.type()) {
        case T::Headers:
            o["type"] = "com.apple.buildphase.headers";
            break;
        case T::Sources:
            o["type"] = "com.apple.buildphase.sources";
            break;
        case T::Resources:
            o["type"] = "com.apple.buildphase.resources";
            break;
        case T::Frameworks:
            o["type"] = "com.apple.buildphase.frameworks";
            break;
        case T::CopyFiles: {
            o["type"] = "com.apple.buildphase.copy-files";
            auto const &cp = static_cast<pbxproj::PBX::CopyFilesBuildPhase const &>(phase);
            o["destinationSubfolder"] = copyFilesDestination(cp.dstSubfolderSpec());
            o["destinationSubpath"] = cp.dstPath().raw();
            if (phase.runOnlyForDeploymentPostprocessing()) {
                o["runOnlyForDeploymentPostprocessing"] = std::string("true");
            }
            break;
        }
        case T::ShellScript: {
            o["type"] = "com.apple.buildphase.shell-script";
            auto const &ss = static_cast<pbxproj::PBX::ShellScriptBuildPhase const &>(phase);
            o["name"] = ss.name();
            o["shellPath"] = ss.shellPath();
            o["scriptContents"] = ss.shellScript();
            o["emitEnvironment"] = std::string(ss.showEnvVarsInLog() ? "true" : "false");
            o["alwaysOutOfDate"] = std::string("false");
            o["alwaysRunForInstallHdrs"] = std::string("false");
            o["sandboxingOverride"] = std::string("basedOnBuildSetting");
            JArray ip;
            for (auto const &p : ss.inputPaths()) ip.push_back(p.raw());
            o["inputFilePaths"] = ip;
            JArray op;
            for (auto const &p : ss.outputPaths()) op.push_back(p.raw());
            o["outputFilePaths"] = op;
            JArray ifl;
            for (auto const &p : ss.inputFileListPaths()) ifl.push_back(p.raw());
            o["inputFileListPaths"] = ifl;
            JArray ofl;
            for (auto const &p : ss.outputFileListPaths()) ofl.push_back(p.raw());
            o["outputFileListPaths"] = ofl;
            o["originalObjectID"] = phase.blueprintIdentifier();
            break;
        }
        case T::AppleScript:
            o["type"] = "com.apple.buildphase.applescript";
            break;
        case T::Rez:
            o["type"] = "com.apple.buildphase.rez";
            break;
    }
    return o;
}

/*
 * Recursively emit a node from the project's group tree.
 * Files and groups have slightly different shapes in the PIF.
 *
 * `excluded` holds GUIDs of FileReferences that are productReferences of
 * targets in this project; we drop them from the group tree because the host
 * emits them only as productReference on the target. Otherwise both entries
 * would share a GUID and the PIF loader would reject it as a duplicate.
 */
JValue emitNode(PIFContext const &ctx, pbxproj::PBX::Project const &project, pbxproj::PBX::GroupItem const &item, std::unordered_set<std::string> const &excluded) {
    JObject o;
    o["guid"] = objectGUID(ctx, project, item);
    o["sourceTree"] = item.sourceTree().empty() ? std::string("<group>") : item.sourceTree();

    using GT = pbxproj::PBX::GroupItem::Type;
    bool isGroup = (item.type() == GT::Group || item.type() == GT::VariantGroup || item.type() == GT::VersionGroup);

    if (isGroup) {
        /* Groups always emit both name and path, even when empty. */
        o["name"] = item.name();
        o["path"] = item.path();
    } else {
        /* Files only emit path. */
        if (!item.path().empty()) {
            o["path"] = item.path();
        } else if (!item.name().empty()) {
            o["path"] = item.name();
        }
    }

    switch (item.type()) {
        case GT::Group: {
            o["type"] = "group";
            auto const &g = static_cast<pbxproj::PBX::BaseGroup const &>(item);
            JArray children;
            for (auto const &c : g.children()) {
                if (excluded.count(objectGUID(ctx, project, *c))) continue;
                children.push_back(emitNode(ctx, project, *c, excluded));
            }
            if (!children.empty()) {
                o["children"] = children;
            }
            break;
        }
        case GT::VariantGroup: {
            o["type"] = "variantGroup";
            auto const &g = static_cast<pbxproj::PBX::BaseGroup const &>(item);
            JArray children;
            for (auto const &c : g.children()) {
                if (excluded.count(objectGUID(ctx, project, *c))) continue;
                children.push_back(emitNode(ctx, project, *c, excluded));
            }
            if (!children.empty()) {
                o["children"] = children;
            }
            break;
        }
        case GT::VersionGroup: {
            o["type"] = "versionGroup";
            auto const &g = static_cast<pbxproj::PBX::BaseGroup const &>(item);
            JArray children;
            for (auto const &c : g.children()) {
                if (excluded.count(objectGUID(ctx, project, *c))) continue;
                children.push_back(emitNode(ctx, project, *c, excluded));
            }
            if (!children.empty()) {
                o["children"] = children;
            }
            break;
        }
        case GT::FileReference: {
            o["type"] = "file";
            auto const &fr = static_cast<pbxproj::PBX::FileReference const &>(item);
            std::string ft = fr.lastKnownFileType();
            if (ft.empty()) ft = fr.explicitFileType();
            if (ft.empty()) ft = inferFileType(item.path().empty() ? item.name() : item.path());
            o["fileType"] = ft;
            /* Only emit fileTextEncoding for text-based file types — skip
             * binaries like .xib, images, archives, wrappers. */
            bool isTextual = ft.compare(0, 11, "sourcecode.") == 0
                          || ft.compare(0, 5,  "text.") == 0
                          || ft == "text"
                          || ft.empty();
            if (isTextual) {
                using FE = pbxproj::PBX::FileReference::FileEncoding;
                switch (fr.fileEncoding()) {
                    case FE::UTF8:    o["fileTextEncoding"] = std::string("utf-8"); break;
                    case FE::UTF16:   o["fileTextEncoding"] = std::string("utf-16"); break;
                    case FE::UTF16BE: o["fileTextEncoding"] = std::string("utf-16be"); break;
                    case FE::UTF16LE: o["fileTextEncoding"] = std::string("utf-16le"); break;
                    case FE::Default: break;
                    default: break;
                }
            }
            break;
        }
        case GT::ReferenceProxy: {
            o["type"] = "fileReference";
            break;
        }
    }
    return o;
}

/*
 * Classify a source file into a compiler language family, preferring the
 * file reference's declared type and falling back to its path extension.
 * Non-source files (headers, resources, unknown types) return None so they
 * don't sway the predominant-language tally.
 */
enum class SourceLanguage { None, C, ObjC, Cpp, ObjCpp, Swift };

SourceLanguage classifySourceLanguage(pbxproj::PBX::GroupItem const &item) {
    std::string type;
    if (item.type() == pbxproj::PBX::GroupItem::Type::FileReference) {
        auto const &fr = static_cast<pbxproj::PBX::FileReference const &>(item);
        type = fr.lastKnownFileType();
        if (type.empty()) type = fr.explicitFileType();
    }
    if (type.empty()) {
        std::string const &p = item.path();
        auto dot = p.rfind('.');
        std::string ext = dot == std::string::npos ? std::string() : p.substr(dot + 1);
        if (ext == "swift") type = "sourcecode.swift";
        else if (ext == "mm") type = "sourcecode.cpp.objcpp";
        else if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c++") type = "sourcecode.cpp.cpp";
        else if (ext == "m") type = "sourcecode.c.objc";
        else if (ext == "c") type = "sourcecode.c.c";
    }

    if (type == "sourcecode.swift")       return SourceLanguage::Swift;
    if (type == "sourcecode.cpp.objcpp")  return SourceLanguage::ObjCpp;
    if (type == "sourcecode.cpp.cpp")     return SourceLanguage::Cpp;
    if (type == "sourcecode.c.objc")      return SourceLanguage::ObjC;
    if (type == "sourcecode.c.c")         return SourceLanguage::C;
    return SourceLanguage::None;
}

/*
 * Pick the predominant source language from the files in the target's Sources
 * build phases: the language family with the most files wins. Ties break toward
 * the higher-level language (Swift > Objective-C++ > C++ > Objective-C > C),
 * mirroring that e.g. an Objective-C file implies the C toolchain too. Returns
 * nullopt when the target has no source files (e.g. aggregate or copy-only
 * targets), so callers omit the field as the host does.
 */
ext::optional<std::string> predominantSourceCodeLanguage(pbxproj::PBX::Target const &target) {
    int counts[6] = {0, 0, 0, 0, 0, 0};
    int total = 0;
    for (auto const &phase : target.buildPhases()) {
        if (phase->type() != pbxproj::PBX::BuildPhase::Type::Sources) continue;
        for (auto const &bf : phase->files()) {
            if (bf->fileRef() == nullptr) continue;
            total++;
            counts[static_cast<int>(classifySourceLanguage(*bf->fileRef()))]++;
        }
    }
    if (total == 0) return ext::nullopt;

    /* Highest count wins; on a tie the earlier (higher-level) language in this
     * priority order is chosen. */
    struct { SourceLanguage lang; char const *id; } const order[] = {
        { SourceLanguage::Swift,  "Xcode.SourceCodeLanguage.Swift" },
        { SourceLanguage::ObjCpp, "Xcode.SourceCodeLanguage.Objective-C-Plus-Plus" },
        { SourceLanguage::Cpp,    "Xcode.SourceCodeLanguage.C-Plus-Plus" },
        { SourceLanguage::ObjC,   "Xcode.SourceCodeLanguage.Objective-C" },
        { SourceLanguage::C,      "Xcode.SourceCodeLanguage.C" },
    };
    char const *best = nullptr;
    int bestCount = 0;
    for (auto const &entry : order) {
        int c = counts[static_cast<int>(entry.lang)];
        if (c > bestCount) {
            bestCount = c;
            best = entry.id;
        }
    }
    /* Only non-source files (headers etc.) — fall back to Objective-C, the
     * host's default for a target with no recognized source language. */
    if (best == nullptr) return std::string("Xcode.SourceCodeLanguage.Objective-C");
    return std::string(best);
}

JObject emitProject(PIFContext const &ctx, pbxproj::PBX::Project const &project) {
    JObject p;
    /* The project's own contents.guid is just the 32-char projectGUID
     * (= MD5 of relative project path) — not the `<prefix><MD5(blueprint)>`
     * form used by every other object. */
    p["guid"] = projectGUID(ctx, project);
    p["path"] = project.projectFile();
    p["projectDirectory"] = project.basePath();
    p["developmentRegion"] = project.developmentRegion();
    p["classPrefix"] = project.classPrefix();
    p["appPreferencesBuildSettings"] = JObject{};

    JArray known;
    for (auto const &r : project.knownRegions()) known.push_back(r);
    p["knownRegions"] = known;

    if (project.buildConfigurationList()) {
        p["buildConfigurations"] = emitBuildConfigurations(ctx, project, *project.buildConfigurationList());
        p["defaultConfigurationName"] = project.buildConfigurationList()->defaultConfigurationName();
    } else {
        p["buildConfigurations"] = JArray{};
        p["defaultConfigurationName"] = std::string("");
    }

    /* Build the set of file refs that are productReferences of any target in
     * this project, so we can omit them from the group tree. */
    std::unordered_set<std::string> productRefs;
    for (auto const &target : project.targets()) {
        if (target->type() != pbxproj::PBX::Target::Type::Native) continue;
        auto const &nt = static_cast<pbxproj::PBX::NativeTarget const &>(*target);
        if (nt.productReference()) {
            productRefs.insert(objectGUID(ctx, project, *nt.productReference()));
        }
    }

    if (project.mainGroup()) {
        p["groupTree"] = emitNode(ctx, project, *project.mainGroup(), productRefs);
    }

    JArray targetSigs;
    for (auto const &t : project.targets()) {
        targetSigs.push_back(targetSignature(ctx, project, *t));
    }
    p["targets"] = targetSigs;

    return p;
}

/*
 * The bundle identifier the host records in each target's provisioningSourceData.
 * It is read verbatim (not resolved) from the CFBundleIdentifier of the target's
 * Info.plist — INFOPLIST_FILE resolved through the project/target build settings,
 * including any xcconfig base configurations. When there is no readable Info.plist
 * (e.g. GENERATE_INFOPLIST_FILE targets), the host falls back to the standard
 * `$(PRODUCT_BUNDLE_IDENTIFIER)` expression, which is what a generated Info.plist
 * would contain.
 */
std::string infoPlistBundleIdentifier(Filesystem *filesystem,
                                      pbxproj::PBX::Project const &project,
                                      pbxproj::PBX::Target const &target,
                                      pbxproj::XC::BuildConfiguration::shared_ptr const &config) {
    static std::string const kDefault = "$(PRODUCT_BUNDLE_IDENTIFIER)";

    auto configNamed = [](pbxproj::XC::ConfigurationList::shared_ptr const &list, std::string const &name)
        -> pbxproj::XC::BuildConfiguration::shared_ptr {
        if (list == nullptr) return nullptr;
        for (auto const &c : list->buildConfigurations()) {
            if (c->name() == name) return c;
        }
        return nullptr;
    };

    /* Layer the setting sources the way a build does, lowest priority first:
     * a source-root base (so group-relative xcconfig paths resolve), project
     * settings, project config (xcconfig then inline), target settings, target
     * config (xcconfig then inline). SDK/spec levels are irrelevant to a
     * user-set INFOPLIST_FILE, so we omit them. We load the xcconfig base
     * configurations here rather than reusing the workspace's, because dumpPIF
     * loads the workspace with an empty environment in which their group paths
     * can't resolve. */
    pbxsetting::Environment env;
    env.insertFront(pbxsetting::Level({
        pbxsetting::Setting::Create("SOURCE_ROOT", project.basePath()),
        pbxsetting::Setting::Create("SRCROOT", project.basePath()),
        pbxsetting::Setting::Create("PROJECT_DIR", project.basePath()),
    }), false);
    env.insertFront(project.settings(), false);

    auto applyConfig = [&](pbxproj::XC::BuildConfiguration::shared_ptr const &c) {
        if (c == nullptr) return;
        if (auto ref = c->baseConfigurationReference()) {
            std::string path = env.expand(ref->resolve());
            if (auto file = pbxsetting::XC::Config::Load(filesystem, env, path)) {
                env.insertFront(file->level(), false);
            }
        }
        env.insertFront(c->buildSettings(), false);
    };
    applyConfig(configNamed(project.buildConfigurationList(), config->name()));
    env.insertFront(target.settings(), false);
    applyConfig(config);

    std::string infoPlist = env.resolve("INFOPLIST_FILE");
    if (infoPlist.empty()) {
        /* No Info.plist file. Xcode still synthesizes one when
         * GENERATE_INFOPLIST_FILE is enabled, defaulting the identifier to
         * $(PRODUCT_BUNDLE_IDENTIFIER); otherwise the target has no bundle (e.g.
         * a static library) and the host records an empty identifier. */
        if (pbxsetting::Type::ParseBoolean(env.resolve("GENERATE_INFOPLIST_FILE"))) {
            return kDefault;
        }
        return std::string();
    }

    /* An Info.plist is expected: read its CFBundleIdentifier verbatim, falling
     * back to the standard $(PRODUCT_BUNDLE_IDENTIFIER) if the file or key is
     * missing (which is also Xcode's default for a bundle target). */
    std::string path = libutil::FSUtil::ResolveRelativePath(infoPlist, project.basePath());
    std::vector<uint8_t> bytes;
    if (filesystem->isReadable(path) && filesystem->read(&bytes, path)) {
        auto result = plist::Format::Any::Deserialize(bytes);
        if (result.first != nullptr) {
            if (auto dict = plist::CastTo<plist::Dictionary>(result.first.get())) {
                if (auto id = dict->value<plist::String>("CFBundleIdentifier")) {
                    return id->value();
                }
            }
        }
    }
    return kDefault;
}

JObject emitTarget(PIFContext const &ctx, Filesystem *filesystem, pbxproj::PBX::Project const &project, pbxproj::PBX::Target const &target) {
    JObject t;
    /* Target objects use the same `<projectGuid><MD5(blueprintId)>` GUID
     * scheme as any other object in the project — both for the target's own
     * contents.guid and for its productReference. */
    t["guid"] = objectGUID(ctx, project, target);
    t["name"] = target.name();
    /* Match host: target.classPrefix is only emitted when the project has one. */
    if (!project.classPrefix().empty()) {
        t["classPrefix"] = project.classPrefix();
    }

    using TT = pbxproj::PBX::Target::Type;
    switch (target.type()) {
        case TT::Native:    t["type"] = "standard"; break;
        case TT::Aggregate: t["type"] = "aggregate"; break;
        case TT::Legacy:    t["type"] = "external"; break;
    }

    if (target.buildConfigurationList()) {
        t["buildConfigurations"] = emitBuildConfigurations(ctx, project, *target.buildConfigurationList());
    } else {
        t["buildConfigurations"] = JArray{};
    }

    auto prodToTarget = productFileToTarget(ctx, project);
    JArray phases;
    for (auto const &phase : target.buildPhases()) {
        phases.push_back(emitBuildPhase(ctx, project, *phase, prodToTarget));
    }
    t["buildPhases"] = phases;

    t["buildRules"] = JArray{};

    JArray deps;
    for (auto const &dep : target.dependencies()) {
        JObject d;
        if (dep->target()) {
            /* Dependency GUID follows the same `<projectGuid><MD5(blueprintId)>`
             * scheme. We use the current project's prefix; cross-project
             * dependencies would need to look up the target's home project,
             * but that information isn't readily available here. */
            d["guid"] = objectGUID(ctx, project, *dep->target());
            d["name"] = dep->target()->name();
        } else {
            d["guid"] = std::string("");
            d["name"] = dep->name();
        }
        deps.push_back(d);
    }

    /* Swift package products linked in the Frameworks phase become implicit
     * PACKAGE-PRODUCT dependencies, in link order (deduplicated). The host
     * synthesizes these from the productRef build files — the pbxproj often
     * carries no explicit packageProductDependencies array — so we do the same.
     * Only the linking (Frameworks) phase contributes a dependency; embedding
     * (Copy Files) phases reuse the same product without adding another. */
    std::unordered_set<std::string> seenPackageProducts;
    for (auto const &phase : target.buildPhases()) {
        if (phase->type() != pbxproj::PBX::BuildPhase::Type::Frameworks) continue;
        for (auto const &bf : phase->files()) {
            if (bf->productRef() == nullptr) continue;
            std::string const &name = bf->productRef()->productName();
            if (!seenPackageProducts.insert(name).second) continue;
            JObject d;
            d["guid"] = "PACKAGE-PRODUCT:" + name;
            d["name"] = name;
            deps.push_back(d);
        }
    }
    t["dependencies"] = deps;

    if (auto pl = predominantSourceCodeLanguage(target)) {
        t["predominantSourceCodeLanguage"] = *pl;
    }

    if (target.type() == pbxproj::PBX::Target::Type::Native) {
        auto const &nt = static_cast<pbxproj::PBX::NativeTarget const &>(target);
        t["productTypeIdentifier"] = nt.productType();
        if (nt.productReference() != nullptr) {
            JObject pr;
            pr["guid"] = objectGUID(ctx, project, *nt.productReference());
            pr["name"] = nt.productReference()->name();
            pr["type"] = std::string("product");
            t["productReference"] = pr;
        }
        /* Test targets get a baselines path under the project's xcshareddata. */
        if (nt.productType().find(".bundle.unit-test") != std::string::npos ||
            nt.productType().find(".bundle.ui-testing") != std::string::npos) {
            std::string id = target.blueprintIdentifier();
            std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return std::toupper(c); });
            t["performanceTestsBaselinesPath"] = project.projectFile() + "/xcshareddata/xcbaselines/" + id + ".xcbaseline";
        }
    }

    /* Provisioning data per configuration. */
    JArray prov;
    if (target.buildConfigurationList()) {
        for (auto const &cfg : target.buildConfigurationList()->buildConfigurations()) {
            JObject p;
            p["bundleIdentifierFromInfoPlist"] = infoPlistBundleIdentifier(filesystem, project, target, cfg);
            p["configurationName"] = cfg->name();
            p["provisioningStyle"] = (long long)1;
            prov.push_back(p);
        }
    }
    t["provisioningSourceData"] = prov;

    return t;
}

/*
 * Signatures for spliced package objects. The host's are MD5s of its internal
 * NSJSONSerialization output, which we can't reproduce, so we emit stable
 * placeholders keyed on the GUID (as for the main project's `_mod=`);
 * signatures only affect the host's incremental caching, not loading.
 */
std::string packageSignature(std::string const &guid) {
    return "PACKAGE@v12_hash=" + md5Hex(guid);
}

std::string packageTargetSignature(std::string const &guid) {
    return "TARGET@v12_hash=" + md5Hex(guid);
}

} /* namespace */

int DumpPIFAction::
Run(process::User const *user, process::Context const *processContext, Filesystem *filesystem, Options const &options)
{
    if (!options.dumpPIF()) {
        fprintf(stderr, "error: -dumpPIF requires an output path\n");
        return -1;
    }
    std::string const &outPath = *options.dumpPIF();

    /*
     * dumpPIF doesn't actually need SDKs, build rules, or specs — it just
     * reads project/workspace files. Skip Build::Environment::Default (which
     * requires a working DEVELOPER_DIR / xcrun) and load the workspace with
     * an empty base environment, matching the host's behavior.
     */
    pbxsetting::Environment baseEnvironment;
    ext::optional<pbxbuild::WorkspaceContext> workspaceContext;

    if (options.workspace()) {
        xcworkspace::XC::Workspace::shared_ptr workspace = xcworkspace::XC::Workspace::Open(filesystem, *options.workspace());
        if (workspace == nullptr) {
            fprintf(stderr, "error: unable to open workspace '%s'\n", options.workspace()->c_str());
            return -1;
        }
        workspaceContext = pbxbuild::WorkspaceContext::Workspace(filesystem, user->userName(), baseEnvironment, workspace);
    } else if (options.project()) {
        std::string projectPath = libutil::FSUtil::ResolveRelativePath(*options.project(), processContext->currentDirectory());
        pbxproj::PBX::Project::shared_ptr project = pbxproj::PBX::Project::Open(filesystem, projectPath);
        if (project == nullptr) {
            fprintf(stderr, "error: unable to open project '%s'\n", options.project()->c_str());
            return -1;
        }
        workspaceContext = pbxbuild::WorkspaceContext::Project(filesystem, user->userName(), baseEnvironment, project);
    } else {
        fprintf(stderr, "error: -dumpPIF requires -workspace or -project\n");
        return -1;
    }

    /*
     * Top-level PIF is a JSON array containing one workspace object, then one
     * object per project, then one object per target.
     */
    JArray pif;

    /* Collect projects. For a real workspace there can be many; for a legacy
     * project-only build we synthesize a workspace pointing at the project's
     * embedded project.xcworkspace, matching what xcodebuild does. */
    std::vector<pbxproj::PBX::Project::shared_ptr> projects;
    std::string workspaceName;
    std::string workspacePath;

    if (workspaceContext->workspace() != nullptr) {
        workspaceName = workspaceContext->workspace()->name();
        workspacePath = workspaceContext->workspace()->projectFile();
        for (auto const &kv : workspaceContext->projects()) {
            projects.push_back(kv.second);
        }
        std::sort(projects.begin(), projects.end(), [](pbxproj::PBX::Project::shared_ptr const &a, pbxproj::PBX::Project::shared_ptr const &b) {
            return a->projectFile() < b->projectFile();
        });
    } else if (workspaceContext->project() != nullptr) {
        auto const &p = workspaceContext->project();
        projects.push_back(p);
        workspaceName = p->name();
        workspacePath = p->projectFile() + "/project.xcworkspace";
    } else {
        fprintf(stderr, "error: no workspace or project to dump\n");
        return -1;
    }

    /*
     * Per-project hashes go into the signing context. The host derives the
     * 32-char project PIF GUID from MD5 of the project's filesystem path
     * relative to the workspace's base directory. For a real workspace, that
     * base dir is the workspace's parent; for the synthesized project-only
     * workspace (which lives inside the .xcodeproj bundle), the host instead
     * treats the project's own parent dir as the base — so relative project
     * paths come out as just the basename, not "..".
     */
    std::string baseDir;
    if (workspaceContext->workspace() != nullptr) {
        baseDir = libutil::FSUtil::GetDirectoryName(workspacePath);
    } else {
        baseDir = libutil::FSUtil::GetDirectoryName(workspaceContext->project()->projectFile());
    }

    PIFContext ctx;
    std::unordered_map<pbxproj::PBX::Project const *, std::string> projectMod;
    for (auto const &project : projects) {
        std::string rel = libutil::FSUtil::GetRelativePath(project->projectFile(), baseDir);
        ctx.projectHash[project.get()] = md5Hex(rel);

        /* Host's `_mod=` is MD5 of NSJSONSerialization output of the project's
         * pifRepresentation — not reproducible without bit-exact JSON parity.
         * Substitute MD5 of the .pbxproj file contents: changes whenever the
         * project model changes, which is the property `_mod=` exists for. */
        std::vector<uint8_t> pbxprojBytes;
        std::string pbxprojPath = project->projectFile() + "/project.pbxproj";
        if (filesystem->isReadable(pbxprojPath) && filesystem->read(&pbxprojBytes, pbxprojPath)) {
            projectMod[project.get()] = md5Hex(pbxprojBytes.data(), pbxprojBytes.size());
        } else {
            projectMod[project.get()] = std::string(32, '0');
        }
    }

    /*
     * Discover Swift packages referenced by any project and obtain each one's
     * PIF from SwiftPM (`swift package ... dump-pif`), adapted to the host's
     * app-embedded GUID scheme. Packages are keyed by their resolved directory
     * and ordered by it for determinism. Each entry is the package's list of
     * spliced PIF objects (its project plus targets).
     */
    std::vector<std::vector<SplicedPackageObject>> packagePIFs;
    {
        std::unordered_set<std::string> seenPackageDirs;
        std::vector<std::string> packageDirs;
        for (auto const &project : projects) {
            for (auto const &ref : project->packageReferences()) {
                std::string dir = libutil::FSUtil::ResolveRelativePath(ref->relativePath(), project->basePath());
                if (seenPackageDirs.insert(dir).second) {
                    packageDirs.push_back(dir);
                }
            }
        }
        std::sort(packageDirs.begin(), packageDirs.end());
        for (auto const &dir : packageDirs) {
            if (auto objs = loadPackagePIF(dir)) {
                packagePIFs.push_back(std::move(*objs));
            } else {
                fprintf(stderr, "warning: unable to load Swift package at '%s' (swift toolchain required); its targets will be omitted from the PIF\n", dir.c_str());
            }
        }
    }

    /* Workspace object. */
    {
        JObject ws;
        JObject contents;
        contents["guid"] = workspaceGUID(workspaceName);
        contents["name"] = workspaceName;
        contents["path"] = workspacePath;
        JArray projectSigsForJSON;
        std::vector<std::string> projectSigsForHash;
        for (auto const &p : projects) {
            std::string sig = projectSignature(ctx, *p, projectMod[p.get()]);
            projectSigsForJSON.push_back(sig);
            projectSigsForHash.push_back(sig);
        }
        for (auto const &objs : packagePIFs) {
            for (auto const &obj : objs) {
                if (obj.isProject) {
                    std::string sig = obj.signature.empty() ? packageSignature(obj.guid) : obj.signature;
                    projectSigsForJSON.push_back(sig);
                    projectSigsForHash.push_back(sig);
                }
            }
        }
        contents["projects"] = projectSigsForJSON;

        ws["type"] = "workspace";
        ws["signature"] = workspaceSignature(filesystem, workspacePath, projectSigsForHash);
        ws["contents"] = contents;
        pif.push_back(ws);
    }

    /* Project objects + their targets. */
    for (auto const &project : projects) {
        JObject po;
        po["type"] = "project";
        po["signature"] = projectSignature(ctx, *project, projectMod[project.get()]);
        po["contents"] = emitProject(ctx, *project);
        pif.push_back(po);

        for (auto const &target : project->targets()) {
            JObject to;
            to["type"] = "target";
            to["signature"] = targetSignature(ctx, *project, *target);
            to["contents"] = emitTarget(ctx, filesystem, *project, *target);
            pif.push_back(to);
        }
    }

    /* Swift package projects and targets, spliced from SwiftPM's PIF. Keep
     * SwiftPM's signatures so each project's `targets` list keeps matching its
     * target objects. */
    for (auto const &objs : packagePIFs) {
        for (auto const &obj : objs) {
            JObject o;
            o["type"] = obj.isProject ? "project" : "target";
            if (!obj.signature.empty()) {
                o["signature"] = obj.signature;
            } else {
                o["signature"] = obj.isProject ? packageSignature(obj.guid) : packageTargetSignature(obj.guid);
            }
            o["contents"] = obj.contents;
            pif.push_back(o);
        }
    }

    JSONOut j;
    emit(j, JValue::Arr(pif));
    j.out += "\n";

    std::vector<uint8_t> bytes(j.out.begin(), j.out.end());
    if (!filesystem->write(bytes, outPath)) {
        fprintf(stderr, "error: failed to write PIF to '%s'\n", outPath.c_str());
        return -1;
    }
    printf("Wrote PIF to %s.\n", outPath.c_str());
    return 0;
}
