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
#include <pbxsetting/Value.h>
#include <libutil/Filesystem.h>
#include <libutil/FSUtil.h>
#include <libutil/md5.h>
#include <process/Context.h>
#include <process/User.h>

#include <algorithm>
#include <cctype>
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

    JObject settings;
    for (auto const &setting : config.buildSettings().settings()) {
        std::string key = setting.name();
        if (!setting.condition().values().empty()) {
            key += "[";
            bool first = true;
            for (auto const &cv : setting.condition().values()) {
                if (!first) key += ",";
                first = false;
                key += cv.first + "=" + cv.second;
            }
            key += "]";
        }
        settings[key] = setting.value().raw();
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
            std::string id = t->blueprintIdentifier();
            m[objectGUID(ctx, project, *nt.productReference())] =
                id.empty() ? std::string(32, '0') : md5Hex(id);
        }
    }
    return m;
}

JArray emitBuildFiles(PIFContext const &ctx, pbxproj::PBX::Project const &project, pbxproj::PBX::BuildPhase const &phase, std::unordered_map<std::string, std::string> const &prodToTarget) {
    JArray arr;
    for (auto const &bf : phase.files()) {
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
            o["inputFileListPaths"] = JArray{};
            o["outputFileListPaths"] = JArray{};
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
 * Pick a reasonable predominant source language based on the file extensions
 * found in the target's Sources build phase. Returns nullopt when the target
 * has no source files (e.g. aggregate or copy-only targets) so callers can
 * skip emitting the field, matching the host's behavior.
 */
ext::optional<std::string> predominantSourceCodeLanguage(pbxproj::PBX::Target const &target) {
    int total = 0, objcpp = 0, swift = 0;
    for (auto const &phase : target.buildPhases()) {
        if (phase->type() != pbxproj::PBX::BuildPhase::Type::Sources) continue;
        for (auto const &bf : phase->files()) {
            if (bf->fileRef() == nullptr) continue;
            std::string const &p = bf->fileRef()->path();
            auto dot = p.rfind('.');
            if (dot == std::string::npos) { total++; continue; }
            std::string ext = p.substr(dot + 1);
            total++;
            if (ext == "swift") swift++;
            else if (ext == "mm") objcpp++;
        }
    }
    if (total == 0) return ext::nullopt;
    if (swift * 2 > total) return std::string("Xcode.SourceCodeLanguage.Swift");
    if (objcpp > 0) return std::string("Xcode.SourceCodeLanguage.Objective-C-Plus-Plus");
    return std::string("Xcode.SourceCodeLanguage.Objective-C");
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

JObject emitTarget(PIFContext const &ctx, pbxproj::PBX::Project const &project, pbxproj::PBX::Target const &target) {
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
            p["bundleIdentifierFromInfoPlist"] = std::string("");
            p["configurationName"] = cfg->name();
            p["provisioningStyle"] = (long long)1;
            prov.push_back(p);
        }
    }
    t["provisioningSourceData"] = prov;

    return t;
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
            to["contents"] = emitTarget(ctx, *project, *target);
            pif.push_back(to);
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
