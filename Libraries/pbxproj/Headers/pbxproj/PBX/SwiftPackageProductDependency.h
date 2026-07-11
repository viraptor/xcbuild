/**
 Copyright (c) 2026-present, Stanisław Pitucha
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree.
 */

#ifndef __pbxproj_PBX_SwiftPackageProductDependency_h
#define __pbxproj_PBX_SwiftPackageProductDependency_h

#include <pbxproj/PBX/Object.h>

namespace pbxproj { namespace PBX {

/*
 * A reference to a product vended by a Swift package (XCSwiftPackageProductDependency).
 * Targets link these through PBXBuildFile entries that carry a `productRef`
 * instead of a `fileRef`. In the PIF these surface as `PACKAGE-PRODUCT:<name>`
 * target references and dependencies.
 */
class SwiftPackageProductDependency : public Object {
public:
    typedef std::shared_ptr <SwiftPackageProductDependency> shared_ptr;
    typedef std::vector <shared_ptr> vector;

private:
    std::string _productName;

public:
    SwiftPackageProductDependency();

public:
    inline std::string const &productName() const
    { return _productName; }

protected:
    bool parse(Context &context, plist::Dictionary const *dict, std::unordered_set<std::string> *seen, bool check) override;

public:
    static inline char const *Isa()
    { return ISA::XCSwiftPackageProductDependency; }
};

} }

#endif  // !__pbxproj_PBX_SwiftPackageProductDependency_h
