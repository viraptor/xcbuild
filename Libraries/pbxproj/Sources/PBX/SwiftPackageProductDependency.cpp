/**
 Copyright (c) 2026-present, Stanisław Pitucha
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree.
 */

#include <pbxproj/PBX/SwiftPackageProductDependency.h>
#include <pbxproj/Context.h>
#include <plist/String.h>
#include <plist/Keys/Unpack.h>

using pbxproj::PBX::SwiftPackageProductDependency;
using pbxproj::Context;

SwiftPackageProductDependency::
SwiftPackageProductDependency() :
    Object(Isa())
{
}

bool SwiftPackageProductDependency::
parse(Context &context, plist::Dictionary const *dict, std::unordered_set<std::string> *seen, bool check)
{
    if (!Object::parse(context, dict, seen, false)) {
        return false;
    }

    auto unpack = plist::Keys::Unpack("SwiftPackageProductDependency", dict, seen);

    auto PN = unpack.cast <plist::String> ("productName");
    /* `package` points at the XCLocal/XCRemoteSwiftPackageReference; we don't
     * resolve the package graph here, so consume it to avoid a spurious
     * unhandled-key warning. */
    (void)unpack.cast <plist::String> ("package");

    if (!unpack.complete(check)) {
        fprintf(stderr, "%s", unpack.errorText().c_str());
    }

    if (PN != nullptr) {
        _productName = PN->value();
    }

    return true;
}
