/**
 Copyright (c) 2026-present, Stanisław Pitucha
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree.
 */

#ifndef __pbxproj_PBX_SwiftPackageReference_h
#define __pbxproj_PBX_SwiftPackageReference_h

#include <pbxproj/PBX/Object.h>

namespace pbxproj { namespace PBX {

/*
 * A reference to a local Swift package (XCLocalSwiftPackageReference), stored on
 * the project's `packageReferences`. `relativePath` is the package directory
 * relative to the project's source root; the package's manifest (Package.swift)
 * lives there. Remote packages (XCRemoteSwiftPackageReference) are not modeled
 * yet — resolving them requires the package checkout that `swift package`
 * produces.
 */
class SwiftPackageReference : public Object {
public:
    typedef std::shared_ptr <SwiftPackageReference> shared_ptr;
    typedef std::vector <shared_ptr> vector;

private:
    std::string _relativePath;

public:
    SwiftPackageReference();

public:
    inline std::string const &relativePath() const
    { return _relativePath; }

protected:
    bool parse(Context &context, plist::Dictionary const *dict, std::unordered_set<std::string> *seen, bool check) override;

public:
    static inline char const *Isa()
    { return ISA::XCLocalSwiftPackageReference; }
};

} }

#endif  // !__pbxproj_PBX_SwiftPackageReference_h
