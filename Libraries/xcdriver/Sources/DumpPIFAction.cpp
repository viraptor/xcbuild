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
#include <pbxbuild/Build/Environment.h>
#include <pbxsetting/Setting.h>
#include <pbxsetting/Value.h>
#include <libutil/Filesystem.h>
#include <libutil/FSUtil.h>
#include <process/Context.h>
#include <process/User.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
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
 * Pads a hex string out to the given width by prepending '0' characters.
 * Stable identifier scheme so cross-references inside one PIF dump line up.
 */
std::string padHex(std::string s, size_t width) {
    if (s.size() < width) s.insert(s.begin(), width - s.size(), '0');
    return s;
}

std::string projectGUID(pbxproj::PBX::Project const &project) {
    /* Use blueprintIdentifier or fall back to project name hash. */
    std::string id = project.blueprintIdentifier();
    if (id.empty()) id = "00000000000000000000000000000000";
    return padHex(id, 32);
}

std::string objectGUID(pbxproj::PBX::Project const &project, pbxproj::PBX::Object const &obj) {
    std::string id = obj.blueprintIdentifier();
    if (id.empty()) id = "00000000000000000000000000000000";
    return projectGUID(project) + padHex(id, 32);
}

/*
 * Synthesize a guid from an arbitrary tag (used for sub-objects like buildFile
 * entries that pbxproj doesn't always have a stable identifier for).
 */
std::string syntheticGUID(pbxproj::PBX::Project const &project, std::string const &tag, size_t idx) {
    std::string suffix = tag;
    char buf[16];
    snprintf(buf, sizeof(buf), "_%zu", idx);
    suffix += buf;
    /* Pad/repeat to 32 hex-ish characters. */
    std::string hex;
    for (char c : suffix) {
        char b[3];
        snprintf(b, sizeof(b), "%02x", (unsigned char)c);
        hex += b;
        if (hex.size() >= 32) break;
    }
    return projectGUID(project) + padHex(hex, 32);
}

std::string projectSignature(pbxproj::PBX::Project const &project) {
    return "PROJECT@v11_hash=" + projectGUID(project);
}

std::string targetSignature(pbxproj::PBX::Project const &project, pbxproj::PBX::Target const &target) {
    return "TARGET@v11_hash=" + padHex(target.blueprintIdentifier(), 32);
}

std::string workspaceSignature(pbxproj::PBX::Project const &project) {
    std::string g = projectGUID(project);
    return "WORKSPACE@v11_hash=" + g + "_subobjects=" + g;
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

JObject emitBuildConfiguration(pbxproj::PBX::Project const &project, pbxproj::XC::BuildConfiguration const &config) {
    JObject obj;
    obj["guid"] = objectGUID(project, config);
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
        obj["baseConfigurationFileReference"] = objectGUID(project, *config.baseConfigurationReference());
    }
    return obj;
}

JArray emitBuildConfigurations(pbxproj::PBX::Project const &project, pbxproj::XC::ConfigurationList const &list) {
    JArray arr;
    for (auto const &cfg : list.buildConfigurations()) {
        arr.push_back(emitBuildConfiguration(project, *cfg));
    }
    return arr;
}

JArray emitBuildFiles(pbxproj::PBX::Project const &project, pbxproj::PBX::BuildPhase const &phase) {
    JArray arr;
    for (auto const &bf : phase.files()) {
        JObject o;
        o["guid"] = objectGUID(project, *bf);
        if (bf->fileRef() != nullptr) {
            o["fileReference"] = objectGUID(project, *bf->fileRef());
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

JObject emitBuildPhase(pbxproj::PBX::Project const &project, pbxproj::PBX::BuildPhase const &phase) {
    JObject o;
    o["guid"] = objectGUID(project, phase);
    o["buildFiles"] = emitBuildFiles(project, phase);

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
            o["runOnlyForDeploymentPostprocessing"] = std::string(phase.runOnlyForDeploymentPostprocessing() ? "true" : "false");
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
 */
JValue emitNode(pbxproj::PBX::Project const &project, pbxproj::PBX::GroupItem const &item) {
    JObject o;
    o["guid"] = objectGUID(project, item);
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
                children.push_back(emitNode(project, *c));
            }
            o["children"] = children;
            break;
        }
        case GT::VariantGroup: {
            o["type"] = "variantGroup";
            auto const &g = static_cast<pbxproj::PBX::BaseGroup const &>(item);
            JArray children;
            for (auto const &c : g.children()) {
                children.push_back(emitNode(project, *c));
            }
            o["children"] = children;
            break;
        }
        case GT::VersionGroup: {
            o["type"] = "versionGroup";
            auto const &g = static_cast<pbxproj::PBX::BaseGroup const &>(item);
            JArray children;
            for (auto const &c : g.children()) {
                children.push_back(emitNode(project, *c));
            }
            o["children"] = children;
            break;
        }
        case GT::FileReference: {
            o["type"] = "file";
            auto const &fr = static_cast<pbxproj::PBX::FileReference const &>(item);
            std::string ft = fr.lastKnownFileType();
            if (ft.empty()) ft = fr.explicitFileType();
            if (!ft.empty()) o["fileType"] = ft;
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
 * found in the target's Sources build phase. Matches xcodebuild's behavior of
 * preferring Swift if it dominates, ObjC++ if any .mm/.cpp source is present,
 * and ObjC otherwise.
 */
std::string predominantSourceCodeLanguage(pbxproj::PBX::Target const &target) {
    int objc = 0, objcpp = 0, swift = 0;
    for (auto const &phase : target.buildPhases()) {
        if (phase->type() != pbxproj::PBX::BuildPhase::Type::Sources) continue;
        for (auto const &bf : phase->files()) {
            if (bf->fileRef() == nullptr) continue;
            std::string const &p = bf->fileRef()->path();
            auto dot = p.rfind('.');
            if (dot == std::string::npos) continue;
            std::string ext = p.substr(dot + 1);
            if (ext == "swift") swift++;
            else if (ext == "mm" || ext == "cpp" || ext == "cc" || ext == "cxx") objcpp++;
            else objc++;
        }
    }
    if (swift > objc + objcpp) return "Xcode.SourceCodeLanguage.Swift";
    if (objcpp > 0) return "Xcode.SourceCodeLanguage.Objective-C-Plus-Plus";
    return "Xcode.SourceCodeLanguage.Objective-C";
}

JObject emitProject(pbxproj::PBX::Project const &project) {
    JObject p;
    p["guid"] = projectGUID(project);
    p["path"] = project.projectFile();
    p["projectDirectory"] = project.basePath();
    p["developmentRegion"] = project.developmentRegion();
    p["classPrefix"] = std::string("");
    p["appPreferencesBuildSettings"] = JObject{};

    JArray known;
    for (auto const &r : project.knownRegions()) known.push_back(r);
    p["knownRegions"] = known;

    if (project.buildConfigurationList()) {
        p["buildConfigurations"] = emitBuildConfigurations(project, *project.buildConfigurationList());
        p["defaultConfigurationName"] = project.buildConfigurationList()->defaultConfigurationName();
    } else {
        p["buildConfigurations"] = JArray{};
        p["defaultConfigurationName"] = std::string("");
    }

    if (project.mainGroup()) {
        p["groupTree"] = emitNode(project, *project.mainGroup());
    }

    JArray targetSigs;
    for (auto const &t : project.targets()) {
        targetSigs.push_back(targetSignature(project, *t));
    }
    p["targets"] = targetSigs;

    return p;
}

std::string productTypeForTarget(pbxproj::PBX::Target const &target) {
    if (target.type() == pbxproj::PBX::Target::Type::Native) {
        auto const &nt = static_cast<pbxproj::PBX::NativeTarget const &>(target);
        return nt.productType();
    }
    return "";
}

JObject emitTarget(pbxproj::PBX::Project const &project, pbxproj::PBX::Target const &target) {
    JObject t;
    t["guid"] = padHex(target.blueprintIdentifier(), 32);
    t["name"] = target.name();
    t["classPrefix"] = std::string("");

    using TT = pbxproj::PBX::Target::Type;
    switch (target.type()) {
        case TT::Native:    t["type"] = "standard"; break;
        case TT::Aggregate: t["type"] = "aggregate"; break;
        case TT::Legacy:    t["type"] = "external"; break;
    }

    if (target.buildConfigurationList()) {
        t["buildConfigurations"] = emitBuildConfigurations(project, *target.buildConfigurationList());
    } else {
        t["buildConfigurations"] = JArray{};
    }

    JArray phases;
    for (auto const &phase : target.buildPhases()) {
        phases.push_back(emitBuildPhase(project, *phase));
    }
    t["buildPhases"] = phases;

    t["buildRules"] = JArray{};

    JArray deps;
    for (auto const &dep : target.dependencies()) {
        JObject d;
        if (dep->target()) {
            d["guid"] = padHex(dep->target()->blueprintIdentifier(), 32);
            d["name"] = dep->target()->name();
        } else {
            d["guid"] = std::string("");
            d["name"] = dep->name();
        }
        deps.push_back(d);
    }
    t["dependencies"] = deps;

    t["predominantSourceCodeLanguage"] = predominantSourceCodeLanguage(target);

    if (target.type() == pbxproj::PBX::Target::Type::Native) {
        auto const &nt = static_cast<pbxproj::PBX::NativeTarget const &>(target);
        t["productTypeIdentifier"] = nt.productType();
        if (nt.productReference() != nullptr) {
            JObject pr;
            pr["guid"] = objectGUID(project, *nt.productReference());
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

    ext::optional<pbxbuild::Build::Environment> buildEnvironment = pbxbuild::Build::Environment::Default(user, processContext, filesystem);
    if (!buildEnvironment) {
        fprintf(stderr, "error: couldn't create build environment\n");
        return -1;
    }

    std::vector<pbxsetting::Level> overrideLevels = Action::CreateOverrideLevels(processContext, filesystem, buildEnvironment->baseEnvironment(), options, processContext->currentDirectory());
    xcexecution::Parameters parameters = Action::CreateParameters(options, overrideLevels);

    ext::optional<pbxbuild::WorkspaceContext> workspaceContext = parameters.loadWorkspace(filesystem, user->userName(), *buildEnvironment, processContext->currentDirectory());
    if (!workspaceContext) {
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

    /* Workspace object. */
    {
        JObject ws;
        JObject contents;
        contents["guid"] = projects.empty() ? std::string("00000000000000000000000000000000")
                                            : projectGUID(*projects.front());
        contents["name"] = workspaceName;
        contents["path"] = workspacePath;
        JArray projectSigs;
        for (auto const &p : projects) projectSigs.push_back(projectSignature(*p));
        contents["projects"] = projectSigs;

        ws["type"] = "workspace";
        ws["signature"] = projects.empty() ? std::string("WORKSPACE@v11_hash=00000000000000000000000000000000_subobjects=00000000000000000000000000000000")
                                           : workspaceSignature(*projects.front());
        ws["contents"] = contents;
        pif.push_back(ws);
    }

    /* Project objects + their targets. */
    for (auto const &project : projects) {
        JObject po;
        po["type"] = "project";
        po["signature"] = projectSignature(*project);
        po["contents"] = emitProject(*project);
        pif.push_back(po);

        for (auto const &target : project->targets()) {
            JObject to;
            to["type"] = "target";
            to["signature"] = targetSignature(*project, *target);
            to["contents"] = emitTarget(*project, *target);
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
