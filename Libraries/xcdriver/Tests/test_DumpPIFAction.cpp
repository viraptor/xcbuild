/**
 Copyright (c) 2026-present, Stanisław Pitucha
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <xcdriver/DumpPIFAction.h>
#include <xcdriver/Options.h>
#include <libutil/MemoryFilesystem.h>
#include <libutil/Options.h>
#include <process/MemoryContext.h>
#include <process/DefaultUser.h>

#include <string>
#include <unordered_set>
#include <vector>

using xcdriver::DumpPIFAction;
using xcdriver::Options;
using libutil::MemoryFilesystem;

static std::vector<uint8_t>
Contents(std::string const &string)
{
    return std::vector<uint8_t>(string.begin(), string.end());
}

/*
 * A minimal project that mirrors the LuaSkin failure mode: the "LuaSkin"
 * framework target links the "lua" target's product (liblua.a) through its
 * Frameworks build phase, but declares no explicit PBXTargetDependency on it
 * (dependencies = ()). The dependency is implicit, inferred from the linked
 * product. This is the one path where the PIF generator has to synthesise a
 * target reference from a product file reference.
 */
static char const kProjectPBXProj[] = R"PBX(// !$*UTF8*$!
{
    archiveVersion = 1;
    classes = { };
    objectVersion = 46;
    objects = {

        PROJECT0000000000000001 = {
            isa = PBXProject;
            buildConfigurationList = CFGLISTPROJECT000000001;
            mainGroup = GROUPMAIN00000000000001;
            targets = (
                TARGETLUA00000000000001,
                TARGETLUASKIN000000001,
            );
        };

        GROUPMAIN00000000000001 = {
            isa = PBXGroup;
            children = (
                GROUPPRODUCTS000000001,
            );
            sourceTree = "<group>";
        };

        GROUPPRODUCTS000000001 = {
            isa = PBXGroup;
            children = (
                FILELIBLUA0000000000001,
                FILEFRAMEWORK000000001,
            );
            name = Products;
            sourceTree = "<group>";
        };

        FILELIBLUA0000000000001 = {
            isa = PBXFileReference;
            explicitFileType = archive.ar;
            path = liblua.a;
            includeInIndex = 0;
            sourceTree = BUILT_PRODUCTS_DIR;
        };

        FILEFRAMEWORK000000001 = {
            isa = PBXFileReference;
            explicitFileType = wrapper.framework;
            path = LuaSkin.framework;
            includeInIndex = 0;
            sourceTree = BUILT_PRODUCTS_DIR;
        };

        BUILDFILELIBLUA0000001 = {
            isa = PBXBuildFile;
            fileRef = FILELIBLUA0000000000001;
        };

        PHASESRCLUA0000000001 = {
            isa = PBXSourcesBuildPhase;
            buildActionMask = 2147483647;
            files = ( );
            runOnlyForDeploymentPostprocessing = 0;
        };

        PHASESRCSKIN000000001 = {
            isa = PBXSourcesBuildPhase;
            buildActionMask = 2147483647;
            files = ( );
            runOnlyForDeploymentPostprocessing = 0;
        };

        PHASEFRAMEWORKS00001 = {
            isa = PBXFrameworksBuildPhase;
            buildActionMask = 2147483647;
            files = (
                BUILDFILELIBLUA0000001,
            );
            runOnlyForDeploymentPostprocessing = 0;
        };

        TARGETLUA00000000000001 = {
            isa = PBXNativeTarget;
            buildConfigurationList = CFGLISTLUA0000000000001;
            buildPhases = (
                PHASESRCLUA0000000001,
            );
            buildRules = ( );
            dependencies = ( );
            name = lua;
            productName = lua;
            productReference = FILELIBLUA0000000000001;
            productType = "com.apple.product-type.library.static";
        };

        TARGETLUASKIN000000001 = {
            isa = PBXNativeTarget;
            buildConfigurationList = CFGLISTLUASKIN00000001;
            buildPhases = (
                PHASESRCSKIN000000001,
                PHASEFRAMEWORKS00001,
            );
            buildRules = ( );
            dependencies = ( );
            name = LuaSkin;
            productName = LuaSkin;
            productReference = FILEFRAMEWORK000000001;
            productType = "com.apple.product-type.framework";
        };

        CFGBUILDPROJECT0000001 = {
            isa = XCBuildConfiguration;
            buildSettings = { };
            name = Release;
        };

        CFGBUILDLUA000000001 = {
            isa = XCBuildConfiguration;
            buildSettings = { };
            name = Release;
        };

        CFGBUILDLUASKIN00001 = {
            isa = XCBuildConfiguration;
            buildSettings = { };
            name = Release;
        };

        CFGLISTPROJECT000000001 = {
            isa = XCConfigurationList;
            buildConfigurations = (
                CFGBUILDPROJECT0000001,
            );
            defaultConfigurationIsVisible = 0;
            defaultConfigurationName = Release;
        };

        CFGLISTLUA0000000000001 = {
            isa = XCConfigurationList;
            buildConfigurations = (
                CFGBUILDLUA000000001,
            );
            defaultConfigurationIsVisible = 0;
            defaultConfigurationName = Release;
        };

        CFGLISTLUASKIN00000001 = {
            isa = XCConfigurationList;
            buildConfigurations = (
                CFGBUILDLUASKIN00001,
            );
            defaultConfigurationIsVisible = 0;
            defaultConfigurationName = Release;
        };

    };
    rootObject = PROJECT0000000000000001;
}
)PBX";

/*
 * Collect every value that follows `"<key>" : "` in the emitted PIF JSON,
 * reading up to the closing quote. GUIDs and references are plain hex, so no
 * escape handling is needed.
 */
static std::vector<std::string>
ExtractValues(std::string const &json, std::string const &key)
{
    std::vector<std::string> values;
    std::string needle = "\"" + key + "\" : \"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t start = pos + needle.size();
        size_t end = json.find('"', start);
        if (end == std::string::npos) break;
        values.push_back(json.substr(start, end - start));
        pos = end;
    }
    return values;
}

/*
 * Regression test for an implicit intra-project product link. The LuaSkin
 * target's Frameworks phase links liblua.a (the lua target's product); the PIF
 * generator must translate that build file's fileReference into a
 * targetReference carrying the lua target's *full* object GUID
 * (`<projectGuid><MD5(blueprintId)>`, 64 hex chars) — the same GUID under
 * which the target is registered. A previous bug emitted only the bare 32-char
 * `MD5(blueprintId)` suffix, dropping the project-signature prefix, so the PIF
 * loader could not resolve the target ("Unable to resolve build file ...
 * reference to a missing target").
 */
TEST(DumpPIFAction, ImplicitProductLinkTargetReference)
{
    MemoryFilesystem filesystem = MemoryFilesystem({
        MemoryFilesystem::Entry::Directory("Workspace", {
            MemoryFilesystem::Entry::Directory("LuaSkin.xcodeproj", {
                MemoryFilesystem::Entry::File("project.pbxproj", Contents(kProjectPBXProj)),
            }),
        }),
        MemoryFilesystem::Entry::Directory("out", { }),
    });

    std::string projectPath = filesystem.path("Workspace/LuaSkin.xcodeproj");
    std::string outPath = filesystem.path("out/pif.json");

    Options options;
    auto parsed = libutil::Options::Parse<Options>(&options, {
        "-project", projectPath,
        "-dumpPIF", outPath,
    });
    ASSERT_TRUE(parsed.first) << parsed.second;

    process::DefaultUser user;
    process::MemoryContext processContext = process::MemoryContext(
        "xcodebuild",
        filesystem.path(""),
        { },
        std::unordered_map<std::string, std::string>());

    ASSERT_EQ(0, DumpPIFAction::Run(&user, &processContext, &filesystem, options));

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(filesystem.read(&bytes, outPath));
    std::string json(bytes.begin(), bytes.end());

    /* Every GUID declared anywhere in the PIF. A valid targetReference must be
     * one of them. */
    std::vector<std::string> guids = ExtractValues(json, "guid");
    std::unordered_set<std::string> guidSet(guids.begin(), guids.end());

    std::vector<std::string> targetRefs = ExtractValues(json, "targetReference");

    /* The liblua.a link must have produced exactly one target reference. */
    ASSERT_EQ(1u, targetRefs.size());

    for (auto const &ref : targetRefs) {
        /* Full object GUIDs are 64 hex chars; the bug produced a 32-char one. */
        EXPECT_EQ(64u, ref.size()) << "targetReference dropped its project prefix: " << ref;
        /* And it must resolve to a real object GUID in the PIF. */
        EXPECT_EQ(1u, guidSet.count(ref)) << "targetReference does not resolve to any registered GUID: " << ref;
    }
}
